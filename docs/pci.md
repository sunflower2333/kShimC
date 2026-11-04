# PCI: core, generic ECAM host and Qualcomm PCIe host

This document covers `kernel/drivers/pci/` (owner: pci), which implements
`kernel/include/kshim/pci.h`. See `docs/io-stack.md` for the I/O stack
contracts this builds on. Nothing here has run on hardware. QEMU and host
register models are the only evidence so far.

| File | Role |
| --- | --- |
| `core.c` | Host registry, configuration access and write journal, enumeration, BAR and bridge-window assignment, INTx, driver binding, handoff |
| `dt.c` | `bus-range`, `linux,pci-domain`, `ranges` windows, `dma-coherent`, `interrupt-map` decoding, address translation through ancestor `ranges` |
| `alloc.c` | Aperture allocator (busy spans, first fit, natural alignment) |
| `map.c` | `kshim_pci_map_resources()`, with each host mapped and rolled back on its own |
| `ecam.c` | `pci-host-ecam-generic` (QEMU virt gpex), `CONFIG_KSHIM_PCI_ECAM` |
| `qcom.c` | Qualcomm DesignWare root complex, `CONFIG_KSHIM_PCI_QCOM` |
| `qcom_pcielib.c` | CrDK PcieLib `PcieIoOps` adapter and target selection |
| `qcom_sm8650.c` | kShimC SM8650 pcie1 `PcieTargetContext` |
| `selftest.c` | `CONFIG_KSHIM_PCI_SELFTEST`: a probe-only xHCI check for `tools/test_qemu_pci.py` |

Kconfig: `KSHIM_PCI`, `KSHIM_PCI_ECAM`, `KSHIM_PCI_QCOM` (which needs
`KSHIM_REGULATOR` and selects `KSHIM_GPIO` and `KSHIM_CLK`), and
`KSHIM_PCI_SELFTEST` (added here, default n). `configs/qemu_pci_defconfig`
is `qemu_manifest_serial` plus PCI, ECAM and the self-test.

## Boot

1. `kshim_pci_map_resources()` runs from `kshim_io_map_resources()` with
   DT access only. For every available host node it maps the configuration
   space and the `ranges` windows as device RW. For ECAM that is `reg[0]`,
   limited to `bus-range` × 1 MiB. For Qualcomm it is `parf`, `dbi`,
   `elbi`, `atu`, `config`, an optional `mhi` and the QMP PHY. Each window
   is capped at 1 GiB, because QEMU describes a 512 GiB 64-bit window. A
   host that fails to map is rolled back on its own and skipped later.
2. `kshim_pci_init()` runs from `kshim_io_init()`, after the USB core has
   registered the xHCI PCI driver. It does the following:
   1. Register the hosts (provider probes).
   2. For each host, power the DT endpoints: every node below the host node
      with a `*-supply` gets `kshim_regulator_bulk_enable()`, and with a
      `pinctrl-0` it gets `kshim_pinctrl_apply("default")`. A cold
      Qualcomm host asserts PERST# before this step.
   3. `ops->start`.
   4. Depth-first scan.
   5. BAR sizing and assignment.
   6. INTx resolution.
   7. Driver binding, after all hosts are up.
3. Drivers call `kshim_pci_enable_device()`, `kshim_pci_bar_map()` and
   `kshim_pci_request_irq()`.

### Configuration access and the journal

- Every `kshim_pci_write_config*()` and every write the core makes itself
  (bus numbers, BARs, windows, COMMAND, MSI, INTx disable) is journaled. The
  first write to a dword records the inherited dword and a byte mask of what
  kShimC wrote.
- At quiesce the journal is replayed newest first, and only the written
  bytes are put back, at their natural width. A 16-bit COMMAND write is
  therefore undone with a 16-bit write, so W1C status bits next to it, or
  the bridge secondary status, are never acknowledged.
- Children are restored before the bridge bus numbers that route to them.
- `kshim_pci_write_config_raw32()` and the BAR sizing pattern are not
  journaled. The sizing pattern is undone immediately, and decoding is off
  while it is applied.
- An equal value is never rewritten (inherit first).

### Enumeration

- Inherited bridge bus numbers are kept when they are ordered
  (`secondary >= next free`), inside the parent's range, and have the
  primary bus right. Otherwise the next free bus is assigned, with the
  parent's limit as a temporary subordinate (Linux-style).
- Below a PCIe root port or downstream port only device 0 is scanned.
  Type 0 configuration ignores the device number, so other slots would
  alias.
- A function's DT node is the child of the host node (root bus) or of its
  bridge's node whose `reg` devfn matches. The Pocket S2
  `usb-controller@0` is found this way.

### Assignment (inherit first)

- Apertures come from the host windows. The first I/O window is the I/O
  aperture, the first non-prefetchable 32-bit window is MEM, and the other
  64-bit or prefetchable window is MEM64.
- A BAR is kept when it is non-zero, naturally aligned, inside an aperture
  of its kind and not overlapping another kept range. Its CPU address
  follows from the window translation.
- A bridge is kept when each open window lies inside the parent's
  apertures and its whole subtree fits in it. Kept children are reserved,
  and new ones are allocated inside. A bridge window that touches no parent
  aperture claims nothing kShimC uses and is ignored. One example is the
  reset value 0/0, which decodes `[0, 1 MiB)`.
- A bridge that cannot be kept is reassigned with its whole subtree:
  1. Its I/O and memory windows are sized bottom up (4 KiB and 1 MiB
     granules, sorted by alignment).
  2. Those windows are allocated in the parent.
  3. Its prefetchable window is closed.
  4. COMMAND gets MEM/IO decoding.
- New memory BARs are placed in the 32-bit window first, so 64-bit and
  prefetchable BARs are placed below 4 GiB too. MEM64 is the fallback for
  64-bit BARs on the root bus, or prefetchable BARs below a kept
  prefetchable window. I/O space below 0x1000 is never handed out.
- Endpoints are not enabled by the core. `kshim_pci_enable_device()` sets
  MEM, IO and optionally MASTER on the function and every upstream bridge.

### Interrupts

- INTx: the pin is swizzled through each bridge
  (`pin = ((pin - 1 + slot) % 4) + 1`). The root-bus unit address and pin
  are then matched against the host's `interrupt-map` with
  `interrupt-map-mask`. The parent must be `arm,gic-v3`, with 3- or 4-cell
  specifiers. One GIC handler per INTID is shared by every function on that
  line, and handlers must tolerate spurious calls. Requesting clears
  COMMAND.INTX_DISABLE, and freeing sets it. Both changes are journaled.
- MSI is only offered through `ops->msi_setup`. Neither host implements it:
  - QEMU's `msi-map` points at the GICv3 ITS, which kShimC does not drive.
  - The DesignWare internal MSI controller is not implemented yet.
  - Drivers therefore get INTx, or ERROR_NOT_SUPPORTED, and then poll.

### Handoff (`kshim_pci_quiesce`, step 3a after USB)

1. Bound drivers' `quiesce`, newest first. A failure is sticky: handoff is
   refused.
2. IRQs are freed.
3. DMA fence: COMMAND.MASTER is cleared on every function (raw write).
4. The journal is replayed. Then each function's bus mastering is set back
   to exactly what it was at entry. A function whose driver failed to
   quiesce keeps bus mastering off. The net effect: the payload finds the
   configuration space as the previous bootloader left it, with any DMA
   engine kShimC drove stopped by its driver.
5. Hosts: `ops->quiesce`. A host that fails is retried on the next call.
6. Endpoint power: `kshim_regulator_bulk_disable()` for every node enabled
   at step 2. This runs after PERST# is asserted, so each rail drops back to
   its inherited state.

After the first call every API returns `KSHIM_IO_ERR_QUIESCED`. Repeated
calls are idempotent.

## Generic ECAM host

`pci-host-ecam-generic` maps the configuration space at
`reg[0] + (bus - bus_start) << 20 | devfn << 12 | reg` (PCIe Base 7.2.2).
Absent functions read all-ones. A `reg` shorter than `bus-range` limits
`bus_end`. Start and quiesce are no-ops: QEMU needs no link, and the
journal restores everything the core wrote.

## Qualcomm PCIe host

Compatibles:

- mainline `qcom,pcie-sm8650`, `-sm8550`, `-sm8450-pcie0/1`, `-sm8350`,
  `-sm8250`, `-sm8150`;
- Android `qcom,pci-msm`.

The DT regions use either reg-name family: `parf`, `dbi`/`dm_core`, `elbi`,
`atu`/`iatu`, `config`/`conf`, and optional `mhi`. The PHY comes from
`phys` (mainline) or a `phy` region (Android). PERST# comes from
`perst-gpios`, `perst-gpio`, or a root-port `reset-gpios`.

### Inherit or cold start

Before any MMIO, the host's GDSC (`power-domains`, when it has one) and its
`cfg`, `bus_master` and `bus_slave` clocks (or Android `*_cfg_ahb_clk`,
`*_mstr_axi_clk` and `*_slv_axi_clk`) are checked with `kshim_clk_is_on()`.
PARF needs the AHB clock, and DBI and the config space go through the AXI
ports. An access with these clocks gated stalls the NoC. If the clock
framework cannot tell, the host is left alone.

- **Live link (inherited).** PARF LTSSM is enabled and the root port's
  Link Status has DLL Link Active set. kShimC changes nothing except iATU
  outbound regions:
  - one region for configuration cycles: the bootloader's own CFG region
    whose base is the `config` window, or else a free, implemented region;
  - one free region for each memory window that no enabled region
    translates. A window that cannot be translated is dropped, and the I/O
    window is only used when the bootloader translated it.

  The root port is reached through DBI. Device 0 of the secondary bus uses
  CFG0, and deeper buses use CFG1. The BDF goes in the target as
  `bus[31:24] dev[23:19] fn[18:16]`, as in CrDK `PcieConfigAccess()`.
  Downstream cycles are only issued while DLL Link Active is set.
  **Handoff:** the owned iATU regions are restored to their snapshot, newest
  first. The link, PERST#, clocks, GDSCs, rails and PARF are left as found.
- **Cold start.** The link is down, or the clocks are off. This needs
  target data for the SoC and controller; selection goes by compatible and
  PARF base, and DBI/ATU/config/PHY must match the DT.
  1. PERST# is asserted at registration.
  2. The core powers the DT endpoints.
  3. `ops->start` applies the controller's pinctrl default and runs
     `PcieLibInit` once, then `PcieInitializePort`.
  4. Configuration access goes through `PcieConfigAccess`.

  **Handoff:** `PcieShutdownPort` disables LTSSM, asserts PERST#, and
  releases every clock, BCR, GDSC and rail the adapter took, in reverse.
  The PERST# pin request is then dropped, so `kshim_gpio_restore_all()`
  returns the pad to its configuration at entry, as it does for the
  pinctrl and rail GPIOs. On a cold port that configuration had the
  endpoint unpowered or held in reset by the pad's pull-down.

### PcieIoOps adapter (`qcom_pcielib.c`)

| PcieLib callback | kShimC service |
| --- | --- |
| Read/Write 8/16/32 | `pci_mmio_*` (identity mapped) |
| DelayUs, GetTimeUs | `delay()`, `kshim_smp_time_us()` |
| SetClock | DT first: `kshim_clk_get(controller or PHY node, clock-names)`. Fallback: `kshim_clk_get_by_name(Controller, Id)`. A 19.2 MHz request on an off branch first calls `kshim_clk_set_rate_cxo()`. PHY-output and always-on CXO entries are no-ops |
| SetReset | `kshim_clk_get_reset(node, reset-names)`, fallback by name, then `kshim_clk_reset()` |
| SetPowerDomain | the controller and PHY `power-domains` (fallback: target GDSC names), enabled controller first and disabled PHY first |
| SetSupply | `kshim_regulator_get(PHY node, then controller node, Name)`. A supply missing from the DT is left as found and logged |
| SetGpio | PERST#: `kshim_gpio` from the DT (else the target pin). WAKE# and CLKREQ#: covered by pinctrl default. Board `enable` lines: the target pin |
| SetInterconnect, SetIommu | logged no-ops: kShimC votes no ICC path and does not program the SMMU (assumed inherited, docs/io-stack.md) |
| SetClockParent, SetPipeClockParent | unsupported: `kshim_clk` has no mux API |
| SetEndpointPower, SetEndpointStage | success: the core powers endpoints from the DT. NTN3 switch boards are unsupported |

PcieLib is built unmodified (`ports/crdk/io/pci.cmake`, `CRDK_PCIE`) with
`CrOsLog=KshimPcieCrLog`. That function prints EDK2 `%a` strings as `%s` and
ends each line. The CrDK `Target/Sm8450` and `Target/Sm8250`
`CrPcieTarget.c` are built unmodified too, with `CrTargetGetPcieContext`
renamed per SoC.

### SM8650 pcie1 target data (`qcom_sm8650.c`)

CrDK has no SM8650 PCIe target. kShimC's table follows the CrDK Sm8450
layout:

- Addresses, interrupts, clocks, resets, GDSCs, the iommu-map and pins come
  from Linux `sm8650.dtsi` (BSD-3-Clause), and the PHY supplies from
  `sm8650-ayaneo-pocket-s2.dts`.
- `pinctrl-sm8650.c` `PINGROUP(98, pcie1_clk_req_n)` gives CLKREQ# mux 1.
- The QMP v6.20 gen4x2 PHY has 119 rows, as listed by
  `sm8650_qmp_gen4x2_pciephy_cfg`: the sm8550 serdes, tx, pcs, pcs_misc and
  ln_shrd tables and the sm8650 rx table, in `qmp_pcie_init_registers()`
  order. Register offsets were resolved from the Linux QMP register headers
  and are named per row.
- The layout is `qmp_pcie_offsets_v6_20`. The status bit is
  PHYSTATUS_4_20 (bit 7), and power-down is `SW_PWRDN | REFCLK_DRV_DSBL`.
- CrDK has no lane-shared PHY block. As in CrDK `Target/Sm8550`, the
  ln_shrd rows are lane-0 TX rows at absolute offset `0xe00 + reg`.
- The no-CSR PHY reset is CrDK's PIPE reset. The PHY GDSC is the PHY power
  domain. `MaxLinkSpeed` 0 keeps the Gen4 capability.
  `ParfSysControlSetMask` 0 keeps PARF_SYS_CTRL, as Linux does.

## Tests

- `pci_core_test`: an ECAM register model (`tests/io/pci_model.h`, fixture
  `tests/fixtures/io/pci-ecam.dts`). It covers:
  - map windows and caps;
  - multifunction, a conventional bridge, and a PCIe port with an aliasing
    slot;
  - 32/64-bit, prefetchable and I/O BARs;
  - bridge windows;
  - swizzle and interrupt-map;
  - a shared INTx line;
  - driver order, probe fallback and late binding;
  - a byte-exact restore with a W1C bit kept, and idempotent quiesce.
- `pci_inherit_test`: kept BARs, bus numbers and windows. Overlapping and
  out-of-window BARs are reassigned, a bridge that cannot hold its subtree
  is reassigned, and conflicting bus numbers are renumbered. Bus mastering
  ends as found.
- `pci_qcom_test_{1,2,3}`: a DWC/PARF/iATU/QMP model with fake
  clk/regulator/gpio/pinctrl (fixture `tests/fixtures/io/pci-sm8650.dts`):
  - 1: inherited link: one iATU region is owned, nothing else is touched,
    and the restore is exact.
  - 2: inherited without a memory region: a free region is programmed and
    restored.
  - 3: cold start through CrDK PcieLib with the SM8650 target. It checks
    PERST# / rail / GDSC / clock / BCR / QMP / PIPE ordering, the PHY
    rows, link up and enumeration. At handoff, PERST# is asserted before
    endpoint power drops and every reference is released.
- `tools/test_qemu_pci.py --runtime <qemu_pci kShimC.bin> --packer <MultiBootKernelPatcher>`:
  1. It boots QEMU virt with gpex, qemu-xhci, a PCIe root port with
     nec-usb-xhci, a PCI bridge with qemu-xhci behind it, and a
     multifunction pair.
  2. It checks enumeration, BAR placement, bus numbers, xHCI CAPLENGTH and
     HCIVERSION through BAR0, and INTx INTIDs.
  3. It compares the GIC state and the configuration space of every
     function, as seen by a payload, with a direct boot, at EL1 and EL2.
     It also passes with the LVGL runtime, where the MMU is on at handoff.

## Not done or not verified

- Nothing has run on hardware. The Pocket S2 inherited path depends on:
  - the UEFI-left link, which is assumed;
  - the SMMU stream for the uPD720201 being in bypass;
  - the sm8650 clock tables answering `kshim_clk_is_on()` for the pcie1
    clocks.
- The cold path depends on:
  - the PCIe NoC/ICC and SMMU state left by the bootloader;
  - `gcc_pcie_1_pipe_clk_src` being switched to the PHY by the clock
    driver. kshim_clk has no mux API, and Linux treats it as a regmap PHY
    mux.

  The CrDK SM8450 pcie1 target's AUX mux makes its cold start fail cleanly
  as unsupported.
- MSI (DesignWare iMSI) is not implemented. INTx is used instead.
