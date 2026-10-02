# CrDK / DT migration validation

Validation date: 2026-09-26. This is an uncommitted worktree change.
The original dirty tree was backed up under `build/crdk-migration-backup`.
No commits, pushes, flashing or changes to the mu_aloha workspace were made.
All 29 recorded reference file hashes still match that workspace.

The complete CrDK submodule is clean at
`b6c3335136b7f6b28271917de11e8f11982975fc`. Its two reproducible patches are:

- `0001-cranepkg-driver-fixes.patch`: `43d33752f133c4baa99683591032c5acaabf88b91939fe2217cd1713f2aed668`
- `0002-freestanding-resources.patch`: `3bfd7dd8de56acacf4600b2d56734d7c068f278a8a8200cc8387d41145253050`

Implementation, ownership rules, source pins and supported resource bindings
are documented in [ports/crdk/README.md](../ports/crdk/README.md). Runtime
sources are organized under `src/core`, `src/dt`, `src/drivers`, `src/ui`
and `src/arch/arm64`, with common public headers under `src/include`.
Old scattered CrDK copies and unused MMU/FDT/GIC/OSKAL copies were removed.

## Completed checks

| Layer | Result | Evidence under `build/` |
| --- | --- | --- |
| Host, current worktree | 14/14 ASan/UBSan tests passed, including actual DT fixtures, keys/SPMI, I2C/SPI, FTM4/5, LVGL, UART recovery, splash and MMU boundaries | `crdk-host-test.log` |
| Independent source snapshot | Andromeda Manifest build and 14/14 ASan/UBSan tests passed; all 241 materialized CrDK files matched the verified patch output | `crdk-clean-snapshot/`, `crdk-clean-host-test.log` |
| Generated hardware tables | SM8150/SM8250 tables reproduced byte-for-byte from hash-checked Android sources | `crdk-table-reproduction.log` |
| CrDK native tests | GENI FIFO/GPI, Clock, RPMh and MU I2C/SPI suites passed from materialized source | `crdk-final-upstream/validation.json` and suite logs |
| AArch64 UEFI syntax | 10 shared-core/adapter/target translation units passed | `crdk-final-upstream/uefi-syntax.log` |
| Manifest firmware builds | Andromeda, HDK8150, HDK8250 and HDK8450: exactly 4 MiB, no undefined symbols | `crdk-manifest-{board}/validation.json` |
| Standalone packaging | Andromeda, HDK8150 and HDK8450: existing board-matched UEFI bytes preserved, boot image round-trip passed, no undefined symbols | `crdk-final-{board}/manifest.json` |
| Standalone boot emulator | Four overlapping/disjoint placements per board, argument/BSS/stack/relocation checks, both UEFI entries and scratch-overlap refusal passed | `crdk-boot-{board}.log` |
| QEMU Standalone SMP | EL1/HVC and EL2/SMC at 1, 2, 4, 8 CPUs: all eight cases passed, with LVGL work/flush/shutdown evidence | `crdk-qemu-smp/validation.json` |
| QEMU Manifest serial | Six cases each on the serial profile and the HDK8250 profile without splash: default/explicit Linux, copied FreeExec, separate payload DT, truncated DTB entry and corrupt DTB structure passed | `crdk-manifest-regression/validation.json`, `crdk-manifest-no-splash/validation.json` |
| QEMU Manifest UI | Both default entries passed with four CPUs; payload verified varied framebuffer, MMU off and secondary CPU_OFF | `crdk-manifest-ui/validation.json` |
| QEMU fail-stop | Injected LVGL worker shutdown failure correctly stopped handoff | `crdk-qemu-failstop/validation.json` |
| Andromeda DT bundle | Nine packaging/negative tests passed, including fresh replay of all 32 base/DTBO combinations | `crdk-andromeda-dt.log` |

The consolidated machine-readable record is
[`build/crdk-migration-validation.json`](../build/crdk-migration-validation.json).
Runtime builds from the independent snapshot have no mu_aloha or CrDK edit-tree
compiler inputs. The CrDK submodule was cloned without object alternates or
hard links. Source materialization is reproducible; timestamped build banners
mean binary identity across different build times is not asserted.

Toolchains: AArch64 GCC 13.3.0, Clang 18.1.3, QEMU 8.2.2, dtc 1.7.0,
Unicorn 2.1.4. CrDK native and syntax tests used explicitly supplied EDK2
headers from the read-only reference workspace. Ordinary runtime builds do
not require EDK2 or that workspace.

## Boundaries and observed external issue

No physical board execution occurred. GENI/SPMI electrical access, IRQ/pin
ownership granted by firmware, touch input, actual SDE scanout, PSCI permission
and hardware payload handoff remain unverified. AArch64 UEFI syntax checks
are not a complete UEFI firmware or WDK build. The polling Clock/RPMh/UART
APIs coexist with existing CrDK interrupt APIs and assume exclusive ownership
on the boot CPU; they do not support concurrent interrupt clients of the same
controller. Stock OnePlus HDK's empty touch selector remains disabled.

The sibling packer currently writes `baseImage->deviceTreeBlobEntry` for a
secondary image in `src/pack.c`, where that image's own index is required.
Its source was preserved. The six-case DT fixture uses a common payload DT
for the executable entries, so it verifies runtime-vs-payload DT separation
and bounds without claiming correct packing of distinct per-image DTBs.
This packer issue is separate from kShimC's runtime DT lookup.

HDK8250 has a Manifest build only; no substitute MTP/QRD UEFI was packaged.
The source-only Kona fixture and host MMIO models are explicitly not hardware
captures. Unsupported or absent splash layouts fall back to serial selection.
