# UFS and SDHCI block storage

`CONFIG_BLK_UFS` and `CONFIG_BLK_SDHCI` (both default `n`) register polled,
read-only block devices through `kernel/include/kshim/blkdev.h`: one
`ufs0:lu<N>` per enabled UFS logical unit and one `sdhc<N>` per SD/eMMC
card (`<N>` is the DT `sdhcN`/`mmcN` alias). `CONFIG_BLK_UFS_WRITE` and
`CONFIG_BLK_SDHCI_WRITE` expose writes and are also `n` by default.

## Layout

| File | Role |
| --- | --- |
| `kernel/drivers/block/ufshci.c` | UFSHCI register core, UTRD/UCD/PRDT, NOP/query UPIUs, SCSI READ(10/16), READ CAPACITY(10/16), LU scan |
| `kernel/drivers/block/sdmmc.c` | SD (v1/v2/SDHC/SDXC) and eMMC identification, CSD/EXT_CSD capacity, CMD17/18 (CMD24/25) over CrDK `CR_SDHC` |
| `kernel/drivers/block/ufs_qcom.c`, `sdhci_msm.c` | Qualcomm power/clock bring-up and platform hooks |
| `kernel/drivers/block/qcom_res.c` | CrDK target tables, ClockLib, polled RPMh rail votes |
| `kernel/drivers/block/probe.c`, `kernel/dt/storage.c` | DT discovery, MMU windows, probe and handoff quiesce |
| `kernel/drivers/block/crdk_shim/` | `Uefi.h` and `Library/FdtLib.h` shims for the CrDK sources only |
| `kernel/drivers/block/block.cmake`, `Kconfig` | build fragment and options |

Everything is polled. No interrupt is enabled, every wait is bounded, and
`kshim_storage_quiesce()` stops the UFS transfer lists and the SD card clock
before payload handoff. Both options depend on `CONFIG_BLOCK`: the drivers
register whole disks with the blkdev core (`kernel/block/`), and
`kshim_storage_init()` then scans their GPT partitions.

## CrDK reuse

Compiled unmodified from the prepared CrDK tree (`CRDK_ROOT`):

- `Library/UfsPowerFdtCrLib/UfsPowerFdt.c`, `Library/UfsPowerPolicyCrLib/UfsPowerPolicy.c` (MIT): UFS supply facts from the runtime DT, then the board-qualified rail votes.
- `Target/Sm8250` and `Target/Sm8450` `CrUfsTarget.c`/`CrSdTarget.c`, plus `Target/Sm8250/CrClockTarget.c` (MIT). The `CrTarget*` entry points are renamed per SoC with compile definitions, so the runtime DT can select the SoC.
- `Library/SdhciMsmLib/SdhciMsm.c` is the SDHCI register core. Its header says "MIT" but has no SPDX tag. CrDK documents its Qualcomm register facts as taken from Android `sdhci-msm.c`; the code itself is CrDK's own.
- `Library/ClockLib` (`clock_config.c`, `clock_votes.c`, `fixed_pll.c`, plus `clock.c`/`gdsc.c` when the port does not already build them), only with `CONFIG_BLK_QCOM_CLOCKS`.
- The port's polled `CrCmdDbLookup`/`CrRpmhPollWrite` API votes the rails.

Not used: `Driver/UfsHcCrDxe/UfsHcCrDxe.c` (no license tag; EDK2 protocol
glue around MU UfsPassThruDxe; its PHY/vendor sequencing follows Linux
ufs-qcom and phy-qcom-qmp-ufs) and `Driver/SdhciCrDxe/MmcHost.c` (`MmcHost`
protocol adapter, "MIT" without SPDX). The UFSHCI engine and the card layer
were written from JESD223/JESD220, the SD Physical Layer Simplified
Specification and JESD84.

## UFS init path

The selected path is logged as `ufs0: UFSHCI x.y ready via <path> path`.

1. **Power.** For SM8250 with `CONFIG_BLK_QCOM_CLOCKS`: rails come from `CrUfsPowerFdtParse` + `CrUfsPowerResolve`. SM8250 requires DT qualification, so a tree that does not qualify leaves the rails inherited. Then the GDSC, the seven non-lane clocks and the three lane symbol clocks are acquired via ClockLib at the target rates. Otherwise (SM8150, SM8450, or clocks off) the HCI is touched only if its GDSC register reports PWR_ON; the GDSC address comes from DT `vdd-hba-supply` or the CrDK policy.
2. **Inherited (preferred).** Taken over without a reset when all of these hold: HCE=1, HCS has DP, UTRLRDY and UCRDY, both doorbells are idle, no fatal IS bit, and (if DME_GET works) a non-zero connected lane count. The driver then stops the lists, installs its own UTRL, restarts, and must get a NOP IN back. If the NOP fails, the takeover is rejected.
3. **Full init (fallback, `CONFIG_BLK_UFS_FULL_INIT=y`).** HCE 0→1, DME_LINKSTARTUP (up to 4 tries), lists, NOP OUT, then fDeviceInit set and polled. This runs on the PHY as XBL/ABL left it.

kShimC never calibrates the QMP PHY. The only sequencing for CrDK's PHY
tables is in the untagged UfsHcCrDxe.c. If the lane clocks do not run
(PHY not locked), a cold start is reported unsupported. HS gear changes,
interconnect votes, SMMU programming and the device reset GPIO are not done.
DMA uses identity addresses with cache clean/invalidate, which assumes the
UFS SMMU stream is in the bypass/identity state the boot chain left.

## SDHCI

The SDCC2 path needs `CONFIG_BLK_QCOM_CLOCKS` (selected automatically): the
card clock is the GCC APPS clock, because the v5 core bypasses the SDHCI
divider. It also needs the CrDK SD target (SM8250). Rails are VDD/VDD-IO via
RPMh. Power requests are serviced by polling `CrSdhcPowerCapture`/`Complete`
from the core's `SyncPower` hook. Card detect is the DT `cd-gpios` TLMM
input. Pads and interconnect votes stay as inherited. Only default speed
(≤ 25 MHz) and PIO are used.

## Size

The Manifest runtime slot is `CONFIG_KSHIM_RUNTIME_SLOT_SIZE`: 4 MiB by default, 8 MiB
for the full-I/O profiles (andromeda, hdk8250, qrd8650, ayaneo_pocket_s2). The
table below predates that change and was measured against 4 MiB. On hdk8250, `BLK_UFS` adds
about 56 KiB and fits. `BLK_QCOM_CLOCKS` adds the SM8250 GCC graph
(37 KiB of data plus about 24 KiB of code). Results with `tools/build_runtime.py`:

| Profile | off | `BLK_UFS` | `+BLK_QCOM_CLOCKS` | `+BLK_SDHCI` |
| --- | --- | --- | --- | --- |
| andromeda, hdk8150, hdk8250, hdk8450 | pass | pass | over by 26–60 KiB | over by 43–70 KiB |
| qemu_manifest_serial, qemu_manifest_ui, qemu_smp | pass | pass | pass | pass |

Fitting SD on the device profiles needs slot space from elsewhere (UI,
touch), or a smaller clock owner than CrDK's full SM8250 GCC graph.

## Tests

- `tests/ufshci_test.c`: register/UPIU model. Covers full init, inherited takeover, busy or dead-link rejection, READ(10/16), bounce buffering, OCS/sense errors and quiesce.
- `tests/sdmmc_test.c`: SDHCI model over the real CrDK core with SDHC, SDSC (byte addressing) and eMMC cards.
- `tests/qcom_storage_test.c`: DT discovery plus CrDK UFS power parse/resolve on `fixtures/kona-storage.dts` and the real HDK8150 DT.
- `tests/storage_qemu.py`: bare-metal QEMU run of `ufshci.c` and `sdmmc.c` + `SdhciMsm.c` against PCI `ufs`/`ufs-lu` and `sdhci-pci`/`sd-card`. It checks full-init followed by inherited takeover, and patterned reads.

None of this has run on Qualcomm hardware.
