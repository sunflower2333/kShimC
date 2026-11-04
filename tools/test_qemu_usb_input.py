#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""End-to-end USB input on QEMU virt: gpex PCIe -> xHCI -> usb-hub -> HID.

Each case boots a Manifest image (the packer and payload fixture of
tests/manifest_qemu.py) on `-M virt,gic-version=3` with a PCI xHCI
controller (qemu-xhci or nec-usb-xhci), a usb-hub on its first port and a
usb-kbd plus a usb-mouse or usb-tablet behind the hub. Once the boot menu
is up, keys are typed over QMP (send-key: Down, then Enter) and the case
passes when the menu boots the second entry (PAYLOAD_B_PASS), which also
means the handoff quiesce (USB, xHCI, PCI) succeeded. Pointer traffic
(QMP input-send-event) is sent before the keys and must not disturb them.

Cases:
  serial-qemu-xhci   configs/qemu_io_defconfig, qemu-xhci, kbd + mouse
  serial-nec-xhci    configs/qemu_io_defconfig, nec-usb-xhci, kbd + tablet
  lvgl-qemu-xhci     configs/qemu_manifest_ui_defconfig + the I/O options and
                     KSHIM_TOUCH (pointer service), qemu-xhci, kbd + tablet;
                     the LVGL keypad group moves with Down and Enter boots
                     (--lvgl)

The runtime config adds CONFIG_KSHIM_PCI, CONFIG_KSHIM_PCI_ECAM and
CONFIG_KSHIM_USB_XHCI. Those drivers belong to the pci and xhci branches;
without them in the tree the script reports SKIP and exits 77.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time

IO_FRAGMENT = """
CONFIG_KSHIM_PCI=y
CONFIG_KSHIM_PCI_ECAM=y
CONFIG_KSHIM_USB=y
CONFIG_KSHIM_USB_XHCI=y
CONFIG_KSHIM_USB_HUB=y
CONFIG_KSHIM_USB_HID=y
"""
SKIP = 77


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, **kwargs)


def drivers_present(root):
    """The PCI ECAM host and the xHCI driver are other branches' work."""
    pci = list((root / "kernel/drivers/pci").glob("*.c"))
    host = list((root / "kernel/drivers/usb/host").glob("*.c"))
    return bool(pci) and bool(host)


def build_runtime(root, base_config, extra, out, jobs):
    out.mkdir(parents=True, exist_ok=True)
    config = out / "input_defconfig"
    config.write_text((root / base_config).read_text() + IO_FRAGMENT + extra)
    run([sys.executable, root / "tools/build_runtime.py", "--config", config, "--out", out / "build",
         "--jobs", str(jobs)], stdout=subprocess.DEVNULL)
    return out / "build/kShimC.bin"


def build_packer(packer_root, out):
    with (out / "packer-build.log").open("w") as log:
        run(["cmake", "-S", packer_root, "-B", out / "packer", "-DBUILD_SHIMS=OFF"],
            stdout=log, stderr=subprocess.STDOUT)
        run(["cmake", "--build", out / "packer", "-j4"], stdout=log, stderr=subprocess.STDOUT)
    return out / "packer/MultiBootKernelPatcher"


PAYLOAD = '''.global _start
.text
_start:
    b start
    nop
    .quad 0
    .quad 4096
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .ascii "ARM\\x64"
    .long 0
start:
    adr x4, message
    mov x5, #0x09000000
1:  ldrb w6, [x4], #1
    cbz w6, finish
    str w6, [x5]
    b 1b
finish:
    mov x0, #8
    movk x0, #0x8400, lsl #16
    hvc #0
    b .
message: .asciz "PAYLOAD_{name}_PASS\\n"
'''


def build_payloads(out):
    for name in ("A", "B"):
        source = out / f"{name}.S"
        source.write_text(PAYLOAD.replace("{name}", name))
        run(["aarch64-linux-gnu-gcc", "-nostdlib", "-static", "-Wl,--build-id=none", "-Wl,-Ttext=0",
             source, "-o", out / f"{name}.elf"], capture_output=True)
        run(["aarch64-linux-gnu-objcopy", "-O", "binary", out / f"{name}.elf", out / f"{name}.bin"])


def pack(packer, runtime, out, name):
    config = out / f"{name}.cfg"
    packed = out / f"{name}.bin"
    # A long autoboot timeout: the first injected key cancels it.
    config.write_text(f"""[Pack]
Shim={runtime}
Output={packed}
Default=Image-A
Timeout=30000
[Image-A]
Name=Linux A
Path={out / 'A.bin'}
Type=Linux
BaseImage=true
[Image-B]
Name=Alternate B
Path={out / 'B.bin'}
Type=Linux
BaseImage=false
[Manifest]
Type=Manifest
""")
    with (out / f"{name}-pack.log").open("w") as log:
        run([packer, config], stdout=log, stderr=subprocess.STDOUT)
    return packed


class Qmp:
    def __init__(self, path, timeout):
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(str(path))
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.05)
        self.file = self.sock.makefile("rwb")
        self.file.readline()  # greeting
        self.command("qmp_capabilities")

    def command(self, name, **arguments):
        message = {"execute": name}
        if arguments:
            message["arguments"] = arguments
        self.file.write(json.dumps(message).encode() + b"\n")
        self.file.flush()
        while True:
            reply = json.loads(self.file.readline())
            if "return" in reply or "error" in reply:
                if "error" in reply:
                    raise RuntimeError(f"QMP {name}: {reply['error']}")
                return reply["return"]

    def key(self, qcode):
        self.command("send-key", keys=[{"type": "qcode", "data": qcode}], **{"hold-time": 80})

    def pointer(self, events):
        self.command("input-send-event", events=events)


class Serial:
    """Console capture in raw chunks: the menu prompt has no newline."""

    def __init__(self, process):
        self.text = []
        self.changed = threading.Condition()
        threading.Thread(target=self._read, args=(process,), daemon=True).start()

    def _read(self, process):
        while True:
            chunk = os.read(process.stdout.fileno(), 4096)
            if not chunk:
                break
            with self.changed:
                self.text.append(chunk.decode(errors="replace"))
                self.changed.notify_all()

    def wait(self, needle, timeout):
        deadline = time.monotonic() + timeout
        with self.changed:
            while needle not in "".join(self.text):
                left = deadline - time.monotonic()
                if left <= 0:
                    return False
                self.changed.wait(min(left, 0.2))
        return True


def run_case(case, packed, out, timeout):
    qmp_path = out / f"{case['name']}.qmp"
    qmp_path.unlink(missing_ok=True)
    devices = [
        "-device", f"{case['xhci']},id=xhci",
        "-device", "usb-hub,bus=xhci.0,port=1,id=hub",
        "-device", "usb-kbd,bus=xhci.0,port=1.1",
        "-device", f"{case['pointer']},bus=xhci.0,port=1.2",
    ]
    command = ["qemu-system-aarch64", "-machine", "virt,gic-version=3", "-accel", "tcg",
               "-cpu", "cortex-a76", "-smp", "1", "-m", "256M", "-display", "none",
               "-monitor", "none", "-serial", "stdio", "-no-reboot",
               "-qmp", f"unix:{qmp_path},server=on,wait=off", *devices, "-kernel", str(packed)]
    started = time.monotonic()
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    serial = Serial(process)
    result = {"case": case["name"], "command": command, "passed": False}
    try:
        qmp = Qmp(qmp_path, 10)
        bound = serial.wait("bound to usb-hid", timeout)
        menu = serial.wait(case["menu_marker"], timeout)
        result.update(hid_bound=bound, menu=menu)
        if bound and menu:
            if case["pointer"] == "usb-tablet":
                qmp.pointer([{"type": "abs", "data": {"axis": "x", "value": 16000}},
                             {"type": "abs", "data": {"axis": "y", "value": 16000}}])
            else:
                qmp.pointer([{"type": "rel", "data": {"axis": "x", "value": 20}},
                             {"type": "rel", "data": {"axis": "y", "value": -10}}])
            time.sleep(0.3)
            qmp.key("down")
            time.sleep(0.5)
            qmp.key("ret")
        result["passed"] = serial.wait("PAYLOAD_B_PASS", timeout)
    finally:
        try:
            process.wait(timeout=10 if result["passed"] else 1)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    log = "".join(serial.text)
    (out / f"{case['name']}.log").write_text(log)
    refused = "refusing payload handoff" in log or "quiesce failed" in log
    result.update(seconds=round(time.monotonic() - started, 2), handoff_refused=refused,
                  passed=result["passed"] and not refused)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=Path("build/usb-input-qemu"))
    parser.add_argument("--packer", type=Path, help="MultiBootKernelPatcher checkout "
                        "(default: ../MultiBootKernelPatcher next to the kShimC checkout)")
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--lvgl", action="store_true", help="also run the LVGL menu case")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if not drivers_present(root):
        print("SKIP: kernel/drivers/pci and kernel/drivers/usb/host (pci and xhci branches) "
              "are not in this tree")
        return SKIP
    packer_root = (args.packer or root.parent / "MultiBootKernelPatcher").resolve()
    packer = build_packer(packer_root, out)
    build_payloads(out)
    serial_runtime = build_runtime(root, "configs/qemu_io_defconfig", "", out / "serial", args.jobs)
    cases = [
        {"name": "serial-qemu-xhci", "xhci": "qemu-xhci", "pointer": "usb-mouse",
         "runtime": serial_runtime, "menu_marker": "Select kernel"},
        {"name": "serial-nec-xhci", "xhci": "nec-usb-xhci", "pointer": "usb-tablet",
         "runtime": serial_runtime, "menu_marker": "Select kernel"},
    ]
    if args.lvgl:
        lvgl_runtime = build_runtime(root, "configs/qemu_manifest_ui_defconfig",
                                     "CONFIG_KSHIM_TOUCH=y\n", out / "lvgl", args.jobs)
        cases.append({"name": "lvgl-qemu-xhci", "xhci": "qemu-xhci", "pointer": "usb-tablet",
                      "runtime": lvgl_runtime, "menu_marker": "UI:"})
    results = []
    for case in cases:
        packed = pack(packer, case["runtime"], out, case["name"])
        result = run_case(case, packed, out, args.timeout)
        result["packed_sha256"] = hashlib.sha256(packed.read_bytes()).hexdigest()
        results.append(result)
        print(f"{case['name']}: {'PASS' if result['passed'] else 'FAIL'}", flush=True)
    report = {
        "qemu": subprocess.check_output(["qemu-system-aarch64", "--version"], text=True).splitlines()[0],
        "tests": results,
        "passed": all(r["passed"] for r in results),
        "hardware_tested": False,
    }
    (out / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"{sum(r['passed'] for r in results)}/{len(results)} passed")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
