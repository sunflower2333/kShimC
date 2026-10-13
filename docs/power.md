# Reboot and shutdown controls

This implementation adds presentation controls to all 29 selectable UI themes.
It does not restore the removed theme or change the payload-selection contract.
The UI requests a typed operation; only the platform loop touches hardware after
quiescing touch, LVGL and secondary workers. Source-based compatibility and host
simulation are **not physical-device validation**.

## Operations and discovery

| Operation | Legacy Qualcomm PON type | Preparation |
| --- | --- | --- |
| Warm restart | 1 | Clear normal dump / emergency-download requests and stale reboot reason. |
| Hard restart | 7 | Same, but disabled when the DT requires warm resets. |
| Power off | 4 | Clear download requests, configure shutdown, deassert PS_HOLD. |
| Enter EDL | 1 (warm) | Disable the primary PON watchdog and request emergency download before reset. |

PON identity comes from DT bus/controller/SID/peripheral tuples, followed by
read-only subtype/revision probes. Nothing is written while capabilities are
being discovered. Up to four explicitly described system/secondary PON devices
are supported. Their reset-enable bit is cleared, a conservative 500 microsecond
settling delay is observed, and the reset-type nibble is programmed before the
enable bit is restored. Only Gen1 subtypes 1/2 with revision zero put that enable
bit in the type register; Gen2 and subtype 3 use the separate enable register.
Unrelated bits are preserved. The optional stored reason is cleared only when
the downstream DT requests it, respecting Gen2 and legacy bit positions.

Addresses are not guessed: PS_HOLD, optional IMEM download/EDL/restart cookies,
and the optional `tcsr-boot-misc-detect` resource come from enabled DT nodes and
are checked for alignment, lengths, duplicate identity and overlapping regions.
The repository's andromeda and HDK8150 fixtures use the named TCSR resource rather
than the optional EDL IMEM node. They have dedicated parser regression coverage.

Qualcomm SMCCC convention and command availability are probed. Interrupted calls
retain the A6 continuation cookie and are bounded. Download ABI selection follows
the DT contract: downstream uses `(mode, 0)` with emergency mode 1, while mainline
uses the normal-download selector `(0x10, value)`. They are not interchangeable.
EDL is exposed only for the supported downstream contract. When the dedicated
boot SCM command is absent, the downstream TCSR path uses SCM secure I/O write,
**not raw TCSR MMIO**. A known secure cookie that cannot be cleared disables the
PON path rather than silently resetting into a stale download state. IMEM-only
emergency download is available only with its explicit DT resource.

After successful preparation, the optional SDI and PMIC-arbiter firmware calls
are used and PS_HOLD is deasserted through firmware, with a DT-described direct
PS_HOLD fallback if the firmware call returns. This is the irreversible boundary:
there are no further SPMI accesses or attempted UI resumption after it. Preparation
failures attempt to restore saved PON settings and clear an emergency request;
cleanup failure is reported separately. A returned reset/shutdown call is always
an error. The platform stops instead of booting a payload after a failed request.

## Deliberate limitations

Unknown and Gen3 PBS/HLOS PON layouts are not written as though they were Gen1/2.
Board-specific RESIN/TWM sequences are not guessed. Reset-type overrides and
PS_HOLD-disable flags restrict the corresponding advertised operation. There is
no silent conversion of EDL or WARM into HARD. A generic PSCI `SYSTEM_OFF` fallback
is offered when a PSCI conduit exists; generic `SYSTEM_RESET` is not labelled as
an exact Qualcomm warm/hard reset. Firmware policy may still reject any advertised
call; capability probing establishes an available interface, not authorization
or successful behavior on every board.

EDL is emergency USB download, not the crash-dump/minidump mode. A computer may
be needed to recover from it, and firmware can require an authorized loader.
These controls do not flash storage, unlock a bootloader or bypass that policy.

## UI and lifecycle

At viewports of at least 240 by 160 pixels, Restart and Power off are
integrated into each theme's existing boot chrome; no physical-screen bottom
strip is reserved. Smaller framebuffers do not install the optional controls.
Unsupported actions are disabled. `menu_power_profile.h` declares each family's
placement and mode-menu structure; `power-ui-complete.md` records all 29 presets.
`LVGL_DEFAULT` retains native widget state styles. Restart opens Warm / Hard /
EDL choices; each operation requires its own confirmation. Initial modal focus
is always Cancel. Touch cancellation never confirms, and the modal has its own
keyboard group so physical keys cannot activate a boot entry behind it. Closing
the modal restores prior focus. Clover retains its direct boot-entry activation
and separate small system-tool row without a redundant Boot button.

Any interaction cancels automatic boot, including opening a power menu. This is
checked both in the UI countdown and in the platform timeout. Event callbacks
only enqueue intentions; object teardown and power operations do not run inside
an event callback. Existing boot entries remain separate from power actions.

## Validation and previews

`power_test` simulates PMIC registers, secure firmware, MMIO and time. It exercises
all four action types, multiple PMICs, per-stage faults, rollback, conservative
capability gates, reason preservation, the TCSR fallback and PSCI-off behavior.
`spmi_test` tests actual driver write adapters and denied transactions without
real hardware. `power_dt_test` validates synthetic malformed trees and the two
large actual board fixtures. `power_menu_test` covers confirmation, Cancel,
physical-key event simulation, real pointer-cancel events, 49-entry rebuilds,
fixed toolbar geometry, supported-mode masks and autoboot cancellation.

CI runs the full host matrix with sanitizers, the non-FLAT ARM64 matrix, and
additional HDK8150 / HDK8250 / headless builds. HDK8150's unchanged defconfig
requires an external UEFI FD. Its CI compile/link test supplies a deliberately
non-bootable AArch64 branch-to-self fixture instead of distributing a firmware
payload. This verifies the UEFI entry path links, not that an FD boots; the test
binary is neither executed nor published. HDK8250 and headless builds do not
need that fixture. Rendering is separate from hardware:
`power_preview_test` explicitly simulates all four capabilities to exercise every
menu, and never executes a reset. It emits two resolutions and four states per
theme. The ordinary boot frames (without the optional toolbar) retain their
existing pixel comparison against the prior renderer.

```sh
python3 tools/build_power_review.py --input collected --output power-review
```

The builder validates original PNG bytes, dimensions, complete frame matrices,
checkout stamps and CTest logs. The offline HTML is a frame viewer, not a working
reboot interface. Do not relabel old images with a new commit or treat a test
fixture's enabled capabilities as detection on a physical device.

## Primary references

Register/ABI facts were checked against the following implementations. This
project's callback sequencer is independently written under MIT, not copied
kernel driver code. No Linux repository is modified.

- Linux v6.12, PON types and reset-enable sequencing:
  https://github.com/torvalds/linux/blob/v6.12/drivers/input/misc/pm8941-pwrkey.c
- Qualcomm downstream PON generations, delay, secondary devices and DT flags:
  https://github.com/LineageOS/android_kernel_qcom_sm8250/blob/a9d9833cb5121c979b503d612b50595f1b5a28a7/drivers/input/misc/qpnp-power-on.c
- Qualcomm downstream EDL cookies, TCSR fallback, reset preparation and PS_HOLD:
  https://github.com/LineageOS/android_kernel_qcom_sm8250/blob/a9d9833cb5121c979b503d612b50595f1b5a28a7/drivers/power/reset/msm-poweroff.c
- Linux v6.12, mainline download ABI and SMCCC convention/continuation handling:
  https://github.com/torvalds/linux/blob/v6.12/drivers/firmware/qcom/qcom_scm.c
  https://github.com/torvalds/linux/blob/v6.12/drivers/firmware/qcom/qcom_scm-smc.c
