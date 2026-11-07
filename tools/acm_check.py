#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check kShimC's USB CDC-ACM console gadget on a Linux host.

Boot a debug image (CONFIG_KSHIM_CONSOLE_USB_ACM=y) with the board's USB
port cabled to this machine, then run:

    python3 tools/acm_check.py [--build-id ef00dd117c4f] [--send 1]

It waits for the gadget (0525:a4a7 "kShimC USB Serial"), opens its ttyACM
node, and reads what kShimC kept for the host (up to 16 KiB of earlier
output) plus anything new. It passes when the kShimC build line and the boot
menu prompt arrive over USB. --send types a menu choice followed by Enter
over USB, which checks the receive path (and starts that entry's handoff).
No third-party modules; only the standard library.
"""
import argparse
import glob
import os
import select
import sys
import termios
import time

VID, PID = "0525", "a4a7"


def find_tty():
    for dev in glob.glob("/sys/bus/usb/devices/*"):
        try:
            with open(os.path.join(dev, "idVendor")) as v, open(os.path.join(dev, "idProduct")) as p:
                if v.read().strip() != VID or p.read().strip() != PID:
                    continue
        except OSError:
            continue
        for tty in glob.glob(os.path.join(dev, "*", "tty", "tty*")):
            return dev, "/dev/" + os.path.basename(tty)
        return dev, None
    return None, None


def raw(fd):
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0                                 # iflag
    attrs[1] = 0                                 # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL | termios.HUPCL
    attrs[3] = 0                                 # lflag: no echo, no canonical mode
    attrs[6][termios.VMIN], attrs[6][termios.VTIME] = 0, 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def read_for(fd, seconds, sink):
    end = time.monotonic() + seconds
    while (left := end - time.monotonic()) > 0:
        ready, _, _ = select.select([fd], [], [], left)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError as error:
            print(f"\nread failed: {error}", file=sys.stderr)
            return False
        if chunk:
            sink.extend(chunk)
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--wait", type=float, default=60, help="seconds to wait for the gadget (default 60)")
    parser.add_argument("--read", type=float, default=5, help="seconds to collect output (default 5)")
    parser.add_argument("--build-id", help="expected build ID prefix, e.g. ef00dd117c4f")
    parser.add_argument("--send", help="menu input to type over USB, followed by Enter")
    args = parser.parse_args()

    print(f"waiting up to {args.wait:.0f} s for {VID}:{PID} ...")
    deadline = time.monotonic() + args.wait
    dev = tty = None
    while time.monotonic() < deadline:
        dev, tty = find_tty()
        if tty and os.path.exists(tty):
            break
        time.sleep(0.2)
    if not dev:
        print("FAIL: no 0525:a4a7 device enumerated (check the cable/port, `dmesg`)")
        return 1
    if not tty:
        print(f"FAIL: {os.path.basename(dev)} enumerated but no ttyACM bound (is cdc_acm loaded?)")
        return 1
    with open(os.path.join(dev, "product")) as product:
        print(f"found {os.path.basename(dev)} \"{product.read().strip()}\" -> {tty}")

    try:
        fd = os.open(tty, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except PermissionError:
        print(f"FAIL: no permission for {tty} (add yourself to 'dialout' or run with sudo)")
        return 1
    raw(fd)  # opening the port raises DTR: kShimC sends what it kept
    got = bytearray()
    ok = read_for(fd, args.read, got)
    text = got.decode("utf-8", "replace")
    banner = next((line for line in text.splitlines() if line.startswith("kShimC ") and ":" not in line[:8]), None)
    prompt = "Select kernel and press Enter:" in text
    print("\n---")
    print(f"received {len(got)} bytes over USB")
    print(f"build line: {banner or 'MISSING'}")
    print(f"menu prompt: {'yes' if prompt else 'MISSING'}")
    passed = ok and bool(banner) and prompt
    if banner and args.build_id and args.build_id not in banner:
        print(f"FAIL: build line does not carry {args.build_id}: a different image is running")
        passed = False
    if passed and args.send is not None:
        os.write(fd, args.send.encode() + b"\r")
        print(f"sent {args.send!r} + Enter over USB; output follows")
        echo = bytearray()
        read_for(fd, args.read, echo)
        print()
        if args.send.encode() not in echo:
            print(f"\nFAIL: kShimC did not echo {args.send!r}: USB input not received")
            passed = False
    os.close(fd)
    print("PASS" if passed else "FAIL")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
