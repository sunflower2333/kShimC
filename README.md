# kShimC

kShimC is one implementation of the generic Shim runtime protocol used by
MultiBootKernelPatcher.
The packer enters the attached runtime with BL. kShimC preserves bootloader
arguments and derives the Linux base in `x4`, Manifest address in `x5`, and
size in `x6`. It validates the Manifest and presents its executable entries
through LVGL when a supported continuous splash is verified, with DT-selected
GENI or PL011 serial selection when no usable splash exists.
The default Manifest runtime remains a fixed 4 MiB slot.

The runtime uses picolibc 1.8.11 and LVGL 9.2.2. Hardware libraries come from
`lib/CrDK`, pinned at `b6c3335136b7f6b28271917de11e8f11982975fc`.
`ports/crdk/prepare.py` verifies and applies the recorded patches into each
build directory. It never modifies the submodule or a mu_aloha workspace.
See [the CrDK port contract and provenance](ports/crdk/README.md).
Completed checks and remaining evidence limits are in the
[migration validation report](docs/crdk-migration-validation.md).

## Build

Install an AArch64 GCC toolchain, CMake, Ninja, Python 3.12+ and kconfiglib
(`genconfig` on PATH). Initialize the dependencies and choose a profile:

```sh
git submodule update --init --recursive
python3 tools/build_runtime.py --config configs/hdk8250_defconfig --out build/manifest
```

This produces `build/manifest/kShimC.bin`, verifies the fixed 4 MiB Manifest
slot and rejects undefined symbols. The same command accepts the Andromeda,
HDK8150 and HDK8450 profiles. No firmware payload or mu_aloha path is needed
for a Manifest runtime. CMake evaluates Kconfig at configure time and selects
CrDK and UI sources by capability; `cmake --build` regenerates after config
changes. The firmware uses the boot DT to obtain device addresses.

## Pack multiple kernels

Build the parent `MultiBootKernelPatcher` repository, then create an INI file:

```ini
[Pack]
Shim=../../kShimC/build/manifest/kShimC.bin
Output=build/FakeKernel
Default=Image-A
Timeout=5000

[Image-A]
Name=Android Linux
Type=Linux
Path=Image
BaseImage=true
Align=21

[Image-B]
Name=Recovery Linux
Type=Linux
Path=/path/to/recovery-Image
BaseImage=false
Align=21

[Image-UEFI]
Name=UEFI
Type=FreeExec
Path=/path/to/UEFI.fd
BaseImage=false
CopyTo=0x9FC00000
CopySizeMax=0x00300000

[Manifest]
Type=Manifest
```

Run the packer with that configuration:

```sh
../../build/MultiBootKernelPatcher ../../Config/Shim.Sample.cfg
```

Additional images are optional. Paths are resolved relative to the
configuration file. Exactly one image must set `BaseImage=true`; `Default`
references an image section. An entry without `CopyTo` is validated as an
ARM64 Linux Image when read; `Align` is optional and its `text_offset`
placement constraint is always applied. An entry with `CopyTo` must also set
`CopySizeMax`. The packer rejects the entry
if its file exceeds that limit; otherwise it is copied to the given address
and booted from there with the original boot arguments. Copy entries default
to 4-byte packed-image alignment, so UEFI does not need an explicit `Align`.
Align is a base-2 alignment exponent from 2 through 21, and assumes the
packed Base Image is loaded at a 2 MiB-aligned address. `Timeout=0` waits for
serial input indefinitely.
The resulting layout follows the generic Shim protocol:

```text
[Base Image slot][KShim 4 MiB slot][aligned images ...][manifest]
```

The Linux slot is at least the ARM64 Image header's effective `image_size`.
The first instruction becomes a BL to the attached runtime; the original
Linux branch is preserved as the second instruction. Header offsets `0x20`, `0x28`, and `0x30` hold the manifest
offset, manifest size, and base image slot size. Additional Linux Images honor
their ARM64 `text_offset` relative to a 2 MiB-aligned base. Copy entries use
their exact file size and KShim synchronizes the data and instruction caches
after copying. The variable-length manifest is appended at the end of the
packed image. At boot, enter a displayed number over PL011 and press Enter;
Escape or the timeout selects the default.

## QEMU

```sh
qemu-system-aarch64 \
	-machine virt,gic-version=3 \
	-cpu cortex-a57 -smp 1 -m 1024M \
	-nographic -no-reboot \
	-kernel build/FakeKernel \
	-append 'console=ttyAMA0,115200 earlycon=pl011,mmio32,0x09000000'
```


## Board menus and input

`configs/` contains capability profiles. The DT splash reservation and stable
SDE source/mixer registers supply the framebuffer address, dimensions, stride
and format. Supported layouts are linear RGB565, 32-bit RGB/BGR and verified
shared-framebuffer split scans. An absent or ambiguous splash falls back to
serial selection. kShimC does not initialize DSI hardware. Volume Up moves up,
Volume Down moves down, and Power confirms. Touch selects a card; the separate
Boot button confirms.

The menu has one flat, dark style (`src/ui/lvgl_port.c`, `src/ui/backdrop.c`):
a borderless indigo acrylic panel (0x161c3e at 50%) centred over an animated
background - a diagonal indigo gradient with an azure-to-lilac wash and four
slowly drifting soft colour blobs, rendered at a bounded texture size and
stretched. Rows are flat plates without borders or shadows; the focused row is
the accent blue 0x2f6bf0 with white text, its label sliding in with an
overshoot. The Boot button is a solid light plate with dark text.

The MIX 3 5G profile is `andromeda`: ST FTM5 (not FocalTech), GENI I2C at
0x49, 1080 x 2340. Its bus, clocks, GPIOs, CmdDB and RPMh setup are independent
of HDK8150. The protocol also has an SPI transport, but no SPI touch variant
of this phone has been established. This boot driver does not flash touch
firmware. Source revisions and register provenance are recorded under
`ports/crdk` and `configs/andromeda-provenance.json`.
The FTM5 driver sources fetched from GitHub are pinned with hashes in
`third_party/st-ftm5/UPSTREAM.json`. IRQ/RESET GPIO122/54 use the vendor's
8 mA pull-up configuration; the power sequence enables GPIO37, waits 4 ms,
enables the fixed 1.8 V RPMh rail, then pulses reset for 10 ms.

Both touch drivers deliver every FIFO contact transition to LVGL, so a tap
whose press and release arrive in one read still selects a card or confirms
the Boot button. A controller reset, bus error or shutdown cancels the contact
without generating a click. FTM5 also handles a moving finger whose ENTER
was missed and treats hovering as released. The serial console prints the
board/controller name and initialization or input error code for bringup.

Before payload handoff, the runtime stops touch scanning, restores claimed bus
resources, joins drawing work, confirms secondary CPU_OFF using PSCI, and
cleans writable runtime-owned WB mappings before disabling its MMU. It then
quiesces GENI UART and restores inherited settings. Reserved/no-map memory is
never mapped as general RAM; explicit framebuffer/CmdDB consumers use NC mappings. A failed shutdown prevents handoff.
Unavailable PSCI falls back to rendering on the boot CPU.

## Validation

```sh
cmake -S tests -B build/host -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/host -j8
ctest --test-dir build/host --output-on-failure
python3 tools/test_qemu_smp.py
python3 tools/test_crdk_upstream.py --edk2 /path/to/edk2
python3 tests/manifest_qemu.py --runtime build/manifest/kShimC.bin
python3 tests/manifest_ui_qemu.py --build build/qemu-manifest-ui
python3 tools/test_qemu_lvgl_failstop.py --build build/qemu-lvgl-failstop
```

The host suite uses ASan and UBSan. QEMU executes the real AArch64 PSCI path
under EL1/HVC and EL2/SMC at 1, 2, 4 and 8 CPUs. The Manifest regression uses
the sibling packer, then boots its default Linux image, another Linux image,
and a copied FreeExec image. It also verifies a separate payload DT and
rejects a DTB exceeding its Manifest entry length or with corrupt structure. `manifest_ui_qemu.py` additionally runs the real
LVGL framebuffer menu with four PSCI CPUs and verifies that the selected
payload observes a varied framebuffer, disabled MMU, and CPU_OFF on all
secondary CPUs. The fail-stop check injects a draw-worker join failure and
verifies that no freed worker is reused. These checks are not hardware
validation of the phone's touch bus, display, or production firmware's PSCI
permissions.

## Build board test images

The standalone tests reserve 40 MiB in the board's Kernel region and embed
one board-matched UEFI FD. Both `UEFI A` and `UEFI B` boot that same FD.
Each build checks
the embedded FD against its saved build report, target device, DEBUG setting,
GENI serial library and nonzero debug PCDs.

```sh
python3 tools/build_hdk8150.py --aloha /path/to/mu_aloha_platforms --out build/hdk8150 \
  --uefi-boot build/uefi-hdk8150/source-uefi-boot.img \
  --uefi-build-dir build/uefi-hdk8150 --require-debug-uart
python3 tools/build_hdk8450.py --aloha /path/to/mu_aloha_platforms --out build/hdk8450 \
  --uefi-boot build/uefi-hdk8450/source-uefi-boot.img \
  --uefi-build-dir build/uefi-hdk8450 --require-debug-uart
python3 tools/build_andromeda_dt.py
python3 tools/build_andromeda.py --aloha /path/to/mu_aloha_platforms --out build/andromeda \
  --uefi-boot build/uefi-andromeda/corrected/source-uefi-boot.img \
  --uefi-build-dir build/uefi-andromeda --require-debug-uart \
  --dtb-bundle build/andromeda-dt/android-andromeda-dtbs.bin
```

The saved Andromeda FD is verified DEBUG+UART, but its original source boot
image carried an incorrect 8450-sized BootShim header. `corrected/` holds a
wrapper rebuilt for 0x9fc00000 / 3 MiB, using the byte-identical FD; its
`provenance.json` records both images. The original snapshot is preserved.
The four Andromeda base trees retain the phone's existing DTBO selection;
32 base/overlay combinations are replayed before packaging. No DTBO is flashed.

HDK8250 bindings and hardware tables use the pinned OnePlus
`oneplus/SM8250_Q_10.0` source at `cad09e061ef6cf689a4a1e54d27562e0f042236a`.
Stock Kona HDK selects HDMI and clears `qcom,i2c-touch-active`. The inherited
ST node therefore stays disabled until the active panel and touch selection
match. With a supported panel selection, FTM4 uses DT GPIO/supply/clock
resources, chip-ID validation and the DT X/Y flips. FTM5 is selected by its
property family and independently verified chip ID.

```sh
python3 tools/build_hdk8250.py --out build/hdk8250
```

This produces a 4 MiB Manifest runtime without bundling UEFI. Display layout
is discovered at boot. The standalone packaging tools above require an
explicit `--aloha` only to read board memory reservations and validate the
user-supplied UEFI package. They do not rebuild or edit that workspace.

Standalone copy-in validates that its scratch page does not overlap the
incoming payload, and rejects an unsafe loader placement before writing.
`tests/boot_emulation.py` checks overlapping and disjoint copies, original
arguments, stack, BSS, relocations, both UEFI choices and that refusal path.
