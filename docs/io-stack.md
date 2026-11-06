# I/O stack: QUP, I2C/SPI, GPIO/regulators, PCI, USB, HID, touch

This document defines the shared interfaces that let seven implementers build
kShimC's I/O stack in parallel. It also covers the boot and handoff order, the
per-board data paths, the test plan and the licensing rules. The interfaces
are the headers in `kernel/include/kshim/`. Each header's comment is the
detailed contract, and this document says how the headers fit together.

Status: the interfaces, the core orchestration (`kernel/core/io.c`) and
every driver below are implemented and merged on `integrate/io-stack`
(QUP/GENI, I2C, SPI, UART, GPIO/pinctrl, clocks, regulators, PCI with the
ECAM and Qualcomm hosts, xHCI with the Renesas download, the USB core, hub
and HID, input injection, the touch framework with FTM4, FTM5, GT911 and
GT9916). With every new symbol off (the default) the tree builds and behaves
as before. The host register-model tests and the QEMU suites pass; nothing
here has run on hardware. Sections 1-3 and 6-7 were the parallel-build
contract; the per-subsystem docs (`docs/qup.md`, `sm8650.md`, `pci.md`,
`usb.md`, `usb-hid.md`, `touch.md`, `touch-goodix.md`) describe what was
built.

## 1. Layers

```
user/ (boot menu, LVGL)              include/kshim/input.h (unchanged API, +BACK, +HOVER)
-------------------------------------------------------------------------------------------
input core   kernel/core/input.c     kshim_key_source_poll / kshim_input_touch_poll
             kernel/core/input_inject.c   <- kshim/input_inject.h (keys + pointer queue, pumps)
touch        kernel/drivers/input/touch/  <- kshim/touch_driver.h (FTM4, FTM5, GT911, GT9916)
HID          kernel/drivers/hid/          <- kshim/hid.h (parser, usage mapping)
USB class    kernel/drivers/usb/class/    <- kshim/usb_class.h (hub, HID transport)
USB core     kernel/drivers/usb/core/     <- kshim/usb.h (device model, URBs, enumeration)
HCD          kernel/drivers/usb/host/     <- kshim/usb_hcd.h (xHCI, Renesas firmware), kshim/dma.h
PCI          kernel/drivers/pci/          <- kshim/pci.h (core, ECAM host, Qualcomm host)
I2C / SPI    kernel/drivers/i2c, spi/     <- kshim/i2c.h, kshim/spi.h
QUP / UART   kernel/drivers/qup/          <- kshim/qup.h, kshim/uart.h (console sits on it)
platform     kernel/drivers/gpio, clk, regulator/, kernel/soc/
                                          <- kshim/gpio.h, pinctrl.h, clk.h, regulator.h
interrupts   kshim/irq.h (GICv3), qcom_irq.h (TLMM/PDC/SPMI cascades)   [existing]
storage      kshim/firmware.h (Renesas firmware from Android partitions) [existing]
CrDK         GeniSeLib FIFO core, ClockLib clock_poll, CmdDB/RpmhLib polling API,
             PcieLib (+ target data), AndroidBindingsLib tables       [unmodified, via ports/crdk]
```

The core orchestration in `kernel/core/io.c` (`kshim/io.h`) is the only code
that calls into every subsystem. Each call is guarded by that subsystem's
Kconfig symbol.

## 2. Conventions (all I/O headers)

- **Return codes.** Every function returns 0 on success or a positive code.
  Codes 1-8 are `ERROR_*` (`error_no.h`), 64-69 belong to storage
  (`kshim/storage.h`) and 96-106 are `KSHIM_IO_ERR_*` (`kshim/io.h`: NACK,
  NO_MEMORY, NO_DEVICE, STALL, SHORT, CANCELLED, OVERFLOW, PROTOCOL, EXISTS,
  QUIESCED, PENDING). CrDK `CR_BUS_*` results are converted with
  `kshim_io_err_from_bus()`. Existing negative-errno code, such as
  `st_ftm5.c` and the CrDK GENI core, is converted where it meets these APIs.
  `kshim_io_strerror()` names any code.
- **Thread context.** This means the boot CPU with IRQs unmasked, outside any
  handler. The following run only in thread context: bus transfers,
  synchronous USB requests, PCI enumeration, clock/regulator/GPIO changes,
  and every init and quiesce call. Only functions documented as IRQ-safe may
  run in handlers: `kshim_completion_complete`, `kshim_input_inject_*`
  producers, `kshim_usb_hcd_port_changed`, `kshim_gpio_set/get` (TLMM) and
  `kshim_uart_write`. `kshim_io_on_boot_cpu()` lets pumps skip other CPUs.
- **Deferred interrupt model.** A handler only acknowledges or masks its
  source and records the pending work (`irq_pending`, a completion). The
  interrupt ends `kshim_wait_event()`/WFI, so the event loop or a
  `kshim_completion_wait()` runs the work right away in thread context.
  This keeps bus transfers out of IRQ context and needs no locks between
  handlers and drivers. It differs from the existing FTM5 IRQ path, which
  drains the FIFO in the TLMM handler; the touch framework replaces that
  path.
- **Polling fallback.** Every driver works without its interrupt: when the
  interrupt does not resolve from the DT, when a cascade seals it (TLMM or
  SPMI storm), or when a level stays asserted for 100 ms with no delivery.
  Touch with an edge trigger, which leaves no level behind, also falls back
  when a periodic handle pass finds events before any interrupt was
  delivered (docs/touch.md).
  `kshim_completion_wait(c, timeout_us, poll, ctx)` covers both cases: it
  calls `poll` before every check and sleeps in WFI for at most 1 ms between
  checks while interrupts are live, or busy-waits in 10 µs steps otherwise.
- **Inherit first.** The previous bootloader (XBL/ABL/UEFI) has already
  brought most hardware up. A running clock, an enabled rail, a muxed pad, a
  live PCIe link, an SE with firmware loaded or an xHCI with Renesas
  firmware is used as it is. A cold init is only the fallback. A value that
  is already correct is not written again.
- **Snapshot and restore.** Every register kShimC changes outside its own
  memory is snapshot before the first write and restored before handoff,
  newest first. Each subsystem has a single restore entry point (section 4).
  Quiesce entry points are idempotent and must succeed before handoff.
- **Memory.** Memory is identity-mapped. Device windows are added in
  `kshim_io_map_resources()` before the MMU is built. On the serial-menu
  path the MMU may never be enabled, in which case all accesses are
  Device-nGnRnE and uncached. Code must therefore work both ways:
  `-mstrict-align` is in effect, and DMA ownership changes use
  `kshim_dma_sync_*`.
- **SMMU.** kShimC never programs the SMMU. PCIe endpoint DMA on SM8650
  (`iommu-map` stream IDs 0x1480/0x1481 for pcie1) relies on the stream
  being in the bypass/identity state the previous bootloader left, as UFS
  already does. This is a risk for the Pocket S2 that must be checked on
  hardware.
- **Interconnect/power domains.** No ICC/BCM votes are issued and the CX
  rail is not voted. QUP and PCIe interconnect state is assumed inherited.
  Cold paths that need a vote report ERROR_NOT_SUPPORTED instead of
  guessing.

## 3. Interfaces

The headers are the contract. This section gives a summary and the points
where subsystems meet.

### 3.1 QUP / GENI (`kshim/qup.h`, owner qup)
- Discovery is pure DT and supports both families. Mainline uses
  `qcom,geni-se-qup` wrappers, whose SE children are `qcom,geni-i2c`, `-spi`,
  `-uart` or `-debug-uart`. Android uses `qcom,qupv3-geni-se` wrappers, with
  SEs `qcom,i2c-geni`, `qcom,spi-geni` and `qcom,msm-geni-serial-hs/-console`
  that point back through `qcom,wrapper-core`. One SE window can have
  several nodes; on SM8650, i2c4 and spi4 share 0xa90000.
- Ownership is per window: `kshim_qup_se_claim(se, owner, node, name)`
  returns ERROR_BUSY when another owner holds it. `kshim_qup_init()` claims
  the console's SE for `KSHIM_QUP_OWNER_CONSOLE`.
- `kshim_qup_se_acquire(se, protocol, &source_hz, &dfs_index)` brings up
  clocks and pins. A running SE is inherited. Otherwise the wrapper AHB
  clocks are voted and the SE clock is set to CXO 19.2 MHz undivided, using
  a verified DFS slot when DFS is enabled; this is CrDK clock_poll's policy.
  The loaded firmware protocol must match (GENI_FW_REVISION_RO +0x68 bits
  15:8; 1 = SPI, 2 = UART, 3 = I2C). kShimC never loads SE firmware. Pinctrl
  "default" is applied. The output feeds `CrGeniConfig` for
  `CrGeniInit`.
- The SE IRQ handler, requested with `kshim_qup_se_irq_request`, only
  masks and acknowledges the source and completes. The FIFO is driven in
  thread context. One way to do this is to run CrDK's `CrGeniI2cTransfer` or
  `CrGeniSpiTransfer` with a `CrIo.delay_us` that sleeps through
  `kshim_completion_wait`/`kshim_wait_event`, so the SE interrupt ends each
  wait early. CrDK stays unmodified.
- `kshim_qup_quiesce_all()` releases every acquired SE except the console's.

### 3.2 I2C (`kshim/i2c.h`) and SPI (`kshim/spi.h`), owner qup
- Adapters and controllers register with the core. Consumers call
  `kshim_i2c_adapter_get(fdt, node)` or `kshim_spi_controller_get(fdt,
  node)`, which probe the GENI controller on demand, so only buses with a
  user are touched.
- I2C messages (`addr`, flags `RD`/`TEN`/`STOP`, `len`, `buf`) are joined by
  repeated START, and the last message ends with STOP. Use
  `kshim_i2c_write_read` for register reads. NACK is reported as
  `KSHIM_IO_ERR_NACK`.
- SPI devices are DT children with "reg" (chip select), "spi-max-frequency",
  "spi-cpha" and "spi-cpol". A message is a list of full-duplex segments
  under one CS assertion, with an optional `delay_us` hold per segment
  (CrGeniSpiTransfer semantics). GENI supports 8-bit words and modes 0-3.
- Transfers are thread context only. A re-entrant call on a busy bus gets
  ERROR_BUSY. Quiesce runs CrGeniQuiesce and then
  `kshim_qup_se_release`.

### 3.3 UART (`kshim/uart.h`, owner qup)
The public console API and `kshim_console.h` are unchanged. `console.c` keeps
one `struct kshim_uart`. That instance is opened from the MMIO window
(`kshim_uart_open_mmio`) before the MMU and QUP discovery exist, and is
quiesced last by `kshim_console_quiesce()`. TX stays polled under
`kshim_irq_save`. RX may use the interrupt (`kshim_uart_rx_irq_start`),
whose handler queues bytes and never takes the console ticket lock. The GENI
RX interrupt code (`geni_rx_irq.c`) moves behind this API.

### 3.4 GPIO, pinctrl, clocks (`kshim/gpio.h`, `pinctrl.h`, `clk.h`), owner sm8650
- `kshim_gpio_from_dt(fdt, node, "reset-gpios", 0, &g)` is followed by
  `kshim_gpio_request(&g, "owner")`, which snapshots CFG/IO, then
  `direction_output/input`, `set/get` (logical values with active-low
  applied) and `free`. Last reference restores. TLMM facts are CFG +0
  (pull 1:0, func 5:2, drive 8:6, OE 9) and IO +4 (IN 0, OUT 1). The pin
  address comes from CrAndroidPin on SM8150/SM8250 and from base + pin ×
  0x1000 on SM8450/SM8650. PMIC GPIOs go through SPMI (PmicGpioLib
  register facts) and have no interrupts here.
- Interrupts on TLMM pins still use `kshim_gpio_irq_request(pin, trigger,
  handler, ctx)` from `qcom_irq.h`, with the pin from `kshim_gpio_tlmm_pin()`.
  The SoC table in `kernel/drivers/irqchip/tlmm.c` needs
  `qcom,sm8650-tlmm` (210 pins), which the sm8650 owner adds.
- `kshim_pinctrl_apply(fdt, node, "default")` handles `pins`, `function`
  (mux index from the SoC function table), `drive-strength`, `bias-*`,
  `output-high/low` and `input-enable`. Each touched pin is snapshot through
  the GPIO registry. PMIC GPIO states return ERROR_NOT_SUPPORTED and are
  left as inherited. One example: the Pocket S2 `gamepad_pwr_en` is GPIO28
  `output-high` on the uPD720201 node.
- Clocks: `kshim_clk_get(fdt, consumer, "se", &clk)` and
  `kshim_clk_get_by_name("gcc", "gcc_pcie_1_aux_clk", &clk)` (for CrDK
  PcieTarget data) give refcounted branch, vote, GDSC and BCR leaves. An
  inherited running branch is not written. `kshim_clk_set_rate_cxo()` is
  the only rate change. SoC tables: CrAndroidClock (SM8150/SM8250 Android
  IDs) and a kShimC SM8650 table keyed by `dt-bindings/clock/qcom,sm8650-gcc.h`
  IDs and Linux names. Its offsets are hardware facts with provenance, for
  example `CrClockSourceGenerated.h` style.

### 3.5 Regulators (`kshim/regulator.h`, owner sm8650)
- `kshim_regulator_get(fdt, consumer, "avdd", &r)` resolves `avdd-supply`.
  Supported providers are `regulator-fixed` with a GPIO, RPMh VRM/XOB in the
  Android form (`qcom,resource-name`) and the mainline form
  (`qcom,pmXXXX-rpmh-regulators` plus `qcom,pmic-id` plus ldoN/smpsN/bobN,
  giving "ldob14"), and always-on fixed rails without a GPIO.
- Enable/disable are refcounted with one object per provider node. The
  parent `vin-supply` is enabled first, the startup delay is honoured, and an
  inherited-on rail is never written.
  `kshim_regulator_bulk_enable(fdt, node)` enables every `*-supply` of a
  node.
- Handoff: fixed rails return to their inherited GPIO level. RPMh votes go
  back to the inherited vote when CrDK's own-DRV observation captured it.
  Otherwise they are kept and logged, which is CrDK's documented policy.

### 3.6 PCI (`kshim/pci.h`, owner pci)
- Hosts: `pci-host-ecam-generic` (QEMU gpex) and Qualcomm DWC
  (`qcom,pcie-sm8650` and others). The Qualcomm host inherits a live link
  (LTSSM L0) and owns only an iATU CFG window, which it snapshots and
  restores. If no link is live, it cold-initialises the port through CrDK
  PcieLib with a `PcieIoOps` adapter onto kshim_clk, regulator and gpio and
  per-SoC `PcieTargetContext` data, and later shuts the port down with
  `PcieShutdownPort`. The SM8650 pcie1 target data is a kShimC file written
  from DT and QMP facts, `kernel/drivers/pci/qcom_sm8650.c` (owned by pci,
  next to the adapter `qcom_pcielib.c`); CrDK is not edited. Its PIPE and
  PHY-AUX muxes and the 100 MHz rchng rate go through `kshim_clk_set_parent()`
  and `kshim_clk_set_rate()` (docs/pci.md).
- `kshim_pci_init()` runs these steps for each host:
  1. Parse the DT: bus-range, linux,pci-domain, ranges (windows),
     dma-coherent and interrupt-map.
  2. Power the endpoints described in DT under the root ports, with every
     `*-supply` and pinctrl "default". For the Pocket S2 uPD720201 these are
     the three regulator-fixed GPIOs plus `gamepad_pwr_en`. This happens
     while PERST is asserted.
  3. Call `ops->start`.
  4. Scan the buses and configure bridges.
  5. Size BARs and assign them from the windows. Valid inherited
     assignments are kept.
  6. Resolve INTx: swizzle through the bridges, then apply the host
     `interrupt-map` to a GIC INTID. The 3- or 4-cell GIC specifier is
     decoded by the core.
  7. Bind drivers: the first matching driver in registration order, matched
     on vendor/device (`KSHIM_PCI_ANY_ID`) and class/mask.
- Config writes are journaled: the first write per dword records the
  inherited value, and quiesce replays the journal newest first.
  `kshim_pci_write_config_raw32` is for side-effect registers such as the
  Renesas firmware download registers 0xf4-0xfc.
- IRQs: `kshim_pci_request_irq(dev, INTX|MSI, handler, ctx, &intid)`. MSI
  only applies when the host offers it; this is the DWC internal MSI on
  SPI "msi0", because the GICv3 ITS is not driven. INTx lines are shared by
  the core. ERROR_NOT_SUPPORTED means the driver must poll.
- `kshim_pci_quiesce()` quiesces the drivers newest first, frees IRQs,
  replays the journal and calls the hosts' `ops->quiesce`.

### 3.7 USB (`kshim/usb.h`, `usb_hcd.h`, `dma.h`, owner xhci; class drivers usbhid)
- Device model: `kshim_usb_device` holds the topology (parent, port,
  root_port, tier, route string, TT hub slot/port), descriptors, the active
  configuration (heap copy), `ep0` and up to 8 interfaces with up to 8
  endpoints each. Packed chapter-9 structs and descriptor iterators are
  provided.
- Transfers: an `kshim_usb_urb` carries device, endpoint, setup, buffer,
  length, flags (SHORT_NOT_OK, ZERO_PACKET), complete and context. Control,
  bulk and interrupt are supported; isochronous is not. Completion callbacks
  run in thread context from `kshim_usb_poll_events()`, never nested and
  never waiting. They may resubmit. The synchronous helpers
  `kshim_usb_control`, `bulk`, `interrupt`, `get_descriptor` and
  `clear_halt` wait with `kshim_completion_wait` while pumping events. The
  core bounces unsuitable buffers into `kshim_dma_alloc` memory.
- Pumps: `kshim_usb_poll()` is `poll_events` plus root and hub port handling
  plus class `service` hooks, and it is registered as an input-source
  service. `kshim_usb_settle(1500 ms)` is called from `kshim_io_init()` for
  the initial enumeration.
- HCD ops: root port status, clear_change, reset and disable; device
  alloc, address, update_ep0, configure, hub_configure, endpoint_reset and
  free; submit, cancel and poll; quiesce. The HCD IRQ handler only acks and
  sets `hcd->irq_pending`.
- Enumeration: `kshim_usb_enumerate(hcd, parent, port, speed, &dev)`
  allocates a slot, sets the address, reads the descriptors, chooses the
  first configuration with a supported interface (else 1), configures the
  endpoints and binds class drivers per interface.
  `kshim_usb_disconnect()` unbinds children first. The external hub driver
  uses the same two calls; that is its hub-class hook, together with
  `ops->hub_configure`.
- Class drivers: `kshim_usb_class_register()`, with a match on interface
  class/subclass/protocol (-1 = any) plus vendor/product (0 = any), and
  `probe`, `disconnect` and `service` hooks.
- xHCI on PCI (`kshim_xhci_pci_register` from `kshim_usb_init`) matches
  class 0x0c0330 and 1912:0014/0015. The Renesas path
  (`CONFIG_KSHIM_USB_XHCI_RENESAS`) uses firmware that is already running
  or was loaded from an external ROM. Otherwise it takes the image linked in
  with `CONFIG_KSHIM_USB_XHCI_RENESAS_FW_EMBED` when one is configured, or
  loads `renesas_usb_fw.mem` with `kshim_firmware_request()` (the vendor
  firmware directories, then `super/odm` and `vendor_dlkm`, then `persist`
  and `efisp`; docs/usb.md), and downloads it through config space using the
  raw writes. A controller left running by UEFI (XhciDxe) is taken over:
  halt, HCRST, then the interrupt is requested, then new rings and run. Quiesce
  halts and resets. The downloaded firmware stays for the payload only when
  the link and the controller's rails were inherited (section 5).
- DMA (`kshim/dma.h`): aligned, boundary-respecting allocations below an
  address limit, plus sync-for-device/cpu. The pool is cacheable heap with
  maintenance, which is also correct while the MMU is off.

### 3.8 HID (`kshim/hid.h`, `usb_class.h`, owner usbhid)
- `kshim_hid_parse()` flattens a report descriptor into items: one per
  variable element, one per array. Each item records its usage range, the
  enclosing application, bit offset and size, count, report ID and type,
  flags and logical range. Limits: 192 items, 32 reports, Push/Pop depth 4,
  collection depth 8. `kshim_hid_find()` and `kshim_hid_get_value()` read
  values.
- `kshim_hid_input_*` maps reports to injected input. Keys are reported
  with Linux codes. Key events are edges, except that a gamepad direction
  held down repeats after 500 ms and then every 150 ms (`PRESS` with
  `pressed = 2`, run from the class `service` hook); keyboards, buttons and
  wheels never repeat (docs/usb-hid.md).

  | Source | UP | DOWN | SELECT | BACK | Pointer |
  | --- | --- | --- | --- | --- | --- |
  | Keyboard | Up arrow | Down arrow | Enter, Keypad Enter, Space | Escape, Backspace | |
  | Gamepad / joystick | Hat N/NE/NW, D-pad 0x90, Y < -50 % | Hat S/SE/SW, D-pad 0x91, Y > +50 % (release under 25 %) | Button 1 | Button 2 | |
  | Consumer control | Volume Increment | Volume Decrement | | | |
  | Mouse | Wheel + | Wheel − | | | relative X/Y, Button 1 = press |
  | Tablet / digitizer | | | | | absolute X/Y, Button 1 or Tip Switch = press |

- The USB HID transport: GET_DESCRIPTOR(Report), parse and map. Boot
  keyboards and mice whose descriptor does not parse or map fall back to
  SET_PROTOCOL(boot). SET_IDLE(0) is sent and a STALL ignored. Reports come
  from an interrupt-IN URB that is resubmitted from its callback.
- Hub class: the hub descriptor (0x29, or 0x2a for SuperSpeed),
  `ops->hub_configure`, port power with a wait of bPwrOn2PwrGood × 2 ms,
  and a status-change URB whose callback only records the change. Port
  handling runs in the `service` hook: GET_STATUS, CLEAR_FEATURE(C_*),
  PORT_RESET, a 100 ms debounce, then enumerate or disconnect. The tier
  limit is 6.

### 3.9 Input injection (`kshim/input_inject.h`, owner usbhid)
- Producers call `kshim_input_inject_key(&kshim_key_event_t)` and
  `kshim_input_inject_pointer(&{ABSOLUTE|RELATIVE, x, y, range, pressed})`.
  Both are IRQ-safe and drop events when the queue is full.
- Sources call `kshim_input_source_register({name, caps, service, ctx})`,
  where `service` is a pump such as `kshim_usb_poll`.
  `kshim_input_inject_has(caps)` reports which capabilities are available.
- Consumer hooks are already in the tree; consumers keep their API:
  - `kshim_key_source_poll()` (kernel/drivers/keys.c) runs
    `kshim_input_service()` and then returns injected keys before the
    platform keys.
  - `kshim_input_touch_poll()` (kernel/core/input.c) delivers injected
    pointer events to the emit callback, scaled to the surface given to
    `kshim_input_touch_start()`. `kshim_input_touch_start` now records
    `emit` and the surface even when no controller exists.
  - `kshim_input_keys_init()` succeeds without platform keys when a
    key-capable source is registered. This is what makes QEMU work.
  - `kshim_input_quiesce()` calls `kshim_input_inject_quiesce()`, which drops
    queued events and refuses new ones.
- Public header changes (append-only): `KSHIM_KEY_ACTION_BACK` (4) and
  `KSHIM_TOUCH_EVENT_HOVER` (4), carrying the pointer position without a
  press. Injected pointer events use contact_id 15
  (`KSHIM_INPUT_POINTER_CONTACT`). Existing user/ code ignores both new
  values. The usbhid owner maps BACK to LV_KEY_ESC in
  `user/ui/lvgl_port.c`, and may draw a cursor on HOVER.

### 3.10 Touch framework (`kshim/touch_driver.h`, owner touch-st; drivers touch-st and touch-goodix)
- Drivers are `const struct kshim_touch_driver` objects, listed in match
  order in `kernel/drivers/input/touch/registry.c`: FTM5, FTM4, GT911,
  GT9916, each under its own `#if`. A driver provides compatibles, bus_mask,
  the reset convention (`KSHIM_TOUCH_RESET_ASSERTED`, or
  `KSHIM_TOUCH_RESET_LEVEL` for goodix,gt9xx, where the logical value is the
  RSTB level), the GPIO property overrides (Android `fts,reset-gpio`), its
  supply names in order with optional per-supply loads (handed to
  `kshim_regulator_set_load()`), the power and reset delays, priv_size, and
  the hooks `match` (no I/O), `power_on` (optional override), `probe`,
  `handle`, `pending` and `quiesce`.
- The framework resolves the bus client (I2C address or SPI device), the
  reset and IRQ GPIOs, the TLMM interrupt, the supplies and the
  `touchscreen-*` properties. It runs the common power sequence: reset
  asserted, supplies on, delay, reset released, delay. It then probes,
  takes the interrupt through `kshim_gpio_irq_request` (the handler masks
  and marks pending, and `kshim_input_touch_poll` runs `handle`, then
  unmasks), and falls back to polling. The stuck-line check re-reads the
  pending flag, so an interrupt taken during the check is not mistaken for
  a sealed line, and the IRQ GPIO polarity follows the trigger `probe`
  leaves. It applies swap/invert/scale through the existing tracker
  (`touch.h`, `kshim_touch_report`).
- FTM4 and FTM5 share `st,fts`. Their `match` hooks tell them apart by
  property family: FTM5 has `fts,irq-gpio`, FTM4 has `st,irq-gpio`. The
  Android panel selection and `qcom,i2c-touch-active` checks of
  `kernel/dt/touch.c` move into the FTM `match` hooks.
- GT911 overrides `power_on`: it drives the IRQ GPIO during reset to select
  address 0x5d or 0x14. The touch-goodix owner verifies the RFC's
  active-high `reset-gpios` against the GT911 sequence.
- Stop and quiesce are idempotent and are called from
  `kshim_input_quiesce()` (step 1) and `kshim_io_quiesce()`.
- Migration (done): the legacy FTM5 path is retired. `kernel/dt/touch.c`
  keeps only the Android board selection (`kshim_touch_dt_selected`) and is
  built with the ST drivers, like `st_ftm5.c`. `CONFIG_KSHIM_TOUCH` alone is
  the pointer service (injected HID pointers); the injection hooks in
  `kshim_input_touch_start/poll` are kept.

## 4. Boot and handoff order

Boot (`kshim_main`):
1. Console (`kshim_console_init`).
2. `kshim_resources_init`, which calls `kshim_io_map_resources` in the order
   GPIO, clk, regulator, QUP, PCI. Each subsystem is rolled back on its own
   failure. The GIC is mapped last.
3. `kshim_irq_init`.
4. Storage probe and `kshim_storage_init`.
5. `kshim_io_init(fdt)`: gpio, clk, regulator, qup, usb_init (core, xHCI
   PCI driver, input pump), usb_hub_register, usb_hid_register, pci_init
   (hosts, enumerate, bind, which starts xHCI), usb_settle(1500 ms).
6. Menu (`kshim_user_main`). `kshim_input_keys_init` and
   `kshim_input_touch_start` start touch through the framework.

Handoff, `kshim_platform_quiesce()` (`kernel/core/platform.c`), which both
`prepare_boot()` and `kshim_platform_prepare_handoff()` run:
1. `kshim_input_quiesce()`: injection closed, touch stopped (IRQ freed,
   scanning stopped), key IRQs freed. Then the console RX IRQ stops.
2. `kshim_user_prepare_handoff()` (LVGL), then SMP worker shutdown.
3. 3a. `kshim_io_quiesce()`, in this order:
   1. USB devices and class drivers (`kshim_usb_quiesce`)
   2. USB host controllers (`kshim_usb_hcd_quiesce_all`: xHCI halt and
      HCRST)
   3. PCI (`kshim_pci_quiesce`: drivers, IRQs, config journal, hosts)
   4. touch (`kshim_touch_quiesce_all`)
   5. I2C, then SPI
   6. QUP SEs except the console
   7. clocks
   8. regulators
   9. GPIO and pinctrl snapshots

   3b. `kshim_irqchip_restore()` (TLMM, SPMI, PDC).
4. `prepare_boot` then runs `kshim_storage_quiesce()` and
   `kshim_irq_teardown()`, the MMU handoff and `kshim_console_quiesce()`,
   which quiesces the console UART last.

Every step is idempotent. A failure in step 3a returns -5 and the handoff is
refused. A payload must find no enabled interrupt, running DMA master,
changed pad, clock vote or rail that kShimC introduced. The documented
exception is an RPMh vote whose inherited value could not be observed.

## 5. Per-board data paths

### AYANEO Pocket S2 / S2 Pro (SM8650, mainline `sm8650-ayaneo-pocket-s2.dts`)
- **Gamepad:**
  - `pcie1` (`qcom,pcie-sm8650`; PARF 0x1c08000, DBI 0x40000000, config
    0x40100000; MEM window 0x40300000+0x1fd00000; INTA-D on GIC SPI
    434/435/438/439; PERST TLMM 97 active-low; WAKE TLMM 99).
  - `pcie1_port0` (root port, bus 0).
  - `usb-controller@0` (`pci1912,0014`, bus 1 dev 0). Before link-up its
    supplies are enabled: `upd720201_avdd33` TLMM 123, `_vdd10` TLMM 122
    and `_vdd33` TLMM 121, all regulator-fixed and active-high. Its pinctrl
    `gamepad_pwr_en` (GPIO28 output-high) is applied.
  - PHY rails vreg_l3e_0p9, vreg_l3i_1p2 and vreg_l1i_0p88 are needed only
    for a cold init.
  - The Renesas firmware already runs when UEFI XhciDxe loaded it (the
    expected case). Otherwise it comes from an embedded image (opt-in), or
    `renesas_usb_fw.mem` in `super/vendor{slot}/firmware`, `super/odm` or
    `vendor_dlkm`, `persist` or `efisp` (docs/usb.md).
  - Expected consequence of the handoff contract: when kShimC cold-starts
    pcie1 (it enabled the TLMM 121/122/123 rails and released PERST#), the
    handoff asserts PERST# again and returns the rails to their inherited
    off state. The controller's RAM image is lost and the payload must
    download the firmware itself (it reads `RENESAS_FW_NEEDED`). Only an
    inherited link and inherited rails keep a downloaded image.
  - xHCI, then the USB HID gamepad on one of the controller's root ports
    (a hub, if present, is handled by the hub driver), then hid_input:
    hat/stick/buttons become UP/DOWN/SELECT/BACK.
  - Inherit-first applies: UEFI ships XhciDxe/XhciPciEmulationDxe, so the
    link and the controller are probably live at entry.
  - The SMMU stream 0x1481 must be in bypass (risk).
  - The vendor partition is usually EROFS, which needs the GPL
    `CONFIG_FS_EROFS` (off). The loader then logs it as unreadable and tries
    `super/odm`, `vendor_dlkm`, `persist` (ext4) and `efisp` (FAT); with no
    copy anywhere the probe fails with one log line and the gamepad stays
    off. Check the partition labels on hardware.
- **Touch:** from the RFC "add display nodes" patch. `&i2c4` (SE 0xa90000,
  wrapper qupv3_id_0, GCC_QUPV3_WRAP1_S4_CLK) at 400 kHz, then
  `touchscreen@5d` (`goodix,gt911`). IRQ is TLMM 162 falling edge, reset is
  TLMM 161, VDDIO and AVDD28 come from vreg_l14b_3p2, size 1440×2560,
  pinctrl ts_reset_default and ts_irq_default. The node is not in mainline,
  so the board fixture adds it (`tests/fixtures/io/sm8650-ayaneo-pocket-s2-rfc-touch.dts`).
  The GT911 uses the goodix,gt9xx reset convention (logical value = RSTB
  level, `KSHIM_TOUCH_RESET_LEVEL`): the handoff never pulses the reset of
  an inherited controller, and after a cold start the RSTB pad is released
  to its pull-down before the rail drops. The controller's configured INT
  mode replaces the DT trigger, and the IRQ GPIO polarity follows it.
- **Keys:** gpio-keys vol+ on pm8550_gpios 6 (active-low, debounce 15), the
  PON power key and pon_resin vol− (existing Qualcomm keys).

### Qualcomm QRD8650 (SM8650, mainline `sm8650-qrd.dts`)
- **Touch:** `&spi4` (SE 0xa90000, `qcom,geni-spi`, SPI 357), then
  `touchscreen@0` (`goodix,gt9916`, CS 0, mode 0, 1 MHz). IRQ is TLMM 162
  level-low, reset is TLMM 161 active-low, avdd from vreg_l14b_3p2, size
  1080×2400, pinctrl ts_irq and ts_reset. The GT9916 uses the Goodix Berlin
  SPI protocol, implemented from facts.
- The UEFI reference is LanaiCrPkg (qcom-qrd8650), with CrDK Target/Sm8650
  facts for GPIO, PDC and the console.

### Qualcomm HDK8250 (SM8250 Kona, Android DT)
- **Touch:** ST FTM4 (`st,fts` with the `st,*` property family) at I2C 0x49
  on qupv3_se13_i2c (SE 0xa94000, wrapper 1 engine 5, 400 kHz, GPIO 36/37
  mux qup13). IRQ is GPIO39 active-low and reset is GPIO38. The rails are
  RPMh LDOs (ldoa13 AVDD and others). The AVDD 20 mA load is passed to
  `kshim_regulator_set_load()`, which votes ldoa13 HPM (PMIC5 mode 7) from
  the node's `qcom,mode-threshold-currents`. Native size is 1440×2880,
  flipped on both axes. See `configs/hdk8250-touch-provenance.json`.
  Android kona DTs carry `qcom,pdc-ranges`. The FTM4 protocol is written
  from facts; the GPL integration was removed in 6527d06.

### Xiaomi Mi MIX 3 5G "andromeda" (SM8150, Android DT `andromeda-fdt.dtb`)
- **Touch:** ST FTM5 (`st,fts` with `fts,*`) at I2C 0x49 on qupv3_se17_i2c
  (SE 0xc80000, wrapper 2 engine 0, GPIO 55/56). IRQ is GPIO122 active-low
  and reset is GPIO54. Power is the AVDD gate GPIO37 (regulator-fixed,
  4 ms) plus RPMh XOB ldoc1 at 1.8 V. Native size is 1080×2340. The
  existing `st_ftm5.c` protocol core is adopted. See
  `configs/andromeda-provenance.json`.

### QEMU (end-to-end, `-M virt,gic-version=3`)
- **Input path:** `pci-host-ecam-generic` gpex (ECAM from DT reg, MMIO32
  window 0x10000000-0x3efeffff, INTx through interrupt-map to GIC SPI 3-6;
  the ITS is not used). Behind it sits `qemu-xhci` or `nec-usb-xhci`, then
  `usb-hub`, then `usb-kbd`, `usb-mouse` or `usb-tablet`. HID mapping and
  injection lead to the boot menu.
- **Serial menu:** qemu_manifest_serial-like, with `QCOM_KEYS=n`.
  `kshim_input_keys_init` succeeds because of the HID source, and arrow
  keys and Enter select an entry.
- **LVGL menu:** a usb-tablet's absolute pointer clicks Boot through
  `kshim_input_touch_poll`. This needs `CONFIG_KSHIM_TOUCH=y` as the
  pointer service.

## 6. Ownership

Each implementer works on its own branch from `feature/io-api` and edits
only its own files. Shared files received hook points in this commit, so
merges are additive.

| Implementer | Owns (create or edit freely) |
| --- | --- |
| **qup** | `kernel/drivers/qup/` (wrapper/SE core, `geni_i2c.c`, `geni_spi.c`, `geni_uart.c`), `kernel/dt/qup.c`, `kernel/drivers/i2c/`, `kernel/drivers/spi/`, `kernel/drivers/console.c`, `kernel/drivers/geni_rx_irq.c`, `kernel/include/kshim_console.h` (internal additions only), headers `kshim/qup.h`, `i2c.h`, `spi.h`, `uart.h`, `kernel/drivers/qup/Kconfig`, `cmake/io/qup.cmake`, `ports/crdk/io/qup.cmake`, `tests/io/qup.cmake`, tests `tests/io/qup_*` |
| **sm8650** | `kernel/drivers/gpio/` (consumer, pinctrl, Kconfig), `kernel/drivers/clk/`, `kernel/drivers/regulator/`, `kernel/soc/` (SM8650 pin, function, clock, mux, rate and RPMh tables, plus SM8150/SM8250 adapters over CrAndroidPin/CrAndroidClock), `kernel/dt/gpio.c` (add SM8650 to `kshim_dt_soc`/`kshim_dt_gpio`), the SoC table entry in `kernel/drivers/irqchip/tlmm.c` and `irqchip.h` (`qcom,sm8650-tlmm`), headers `kshim/gpio.h`, `pinctrl.h`, `clk.h`, `regulator.h`, `cmake/io/platform.cmake`, `ports/crdk/io/platform.cmake`, `tests/io/platform.cmake`, new `configs/qrd8650_defconfig`, `configs/ayaneo_pocket_s2_defconfig` (and their provenance JSON), `tests/fixtures/io/sm8650-*` (mainline DTB fixtures, including the Pocket S2 RFC touch node) |
| **pci** | `kernel/drivers/pci/` (core, ECAM host, Qualcomm host and its PcieLib adapter, the SM8650 pcie1 target data `qcom_sm8650.c`, DT parsing, journal), `kshim/pci.h`, `kernel/drivers/pci/Kconfig`, `cmake/io/pci.cmake`, `ports/crdk/io/pci.cmake`, `tests/io/pci.cmake`, `tools/test_qemu_pci.py`, optional `configs/qemu_pci_defconfig` |
| **xhci** | `kernel/drivers/usb/core/`, `kernel/drivers/usb/host/` (xHCI, xhci-pci, Renesas), `kernel/core/dma.c`, `kshim/usb.h`, `usb_hcd.h`, `dma.h`, `kernel/drivers/usb/Kconfig`, `cmake/io/usb.cmake`, `tests/io/usb.cmake`, `tools/test_qemu_xhci.py`, optional `configs/qemu_xhci_defconfig` |
| **usbhid** | `kernel/drivers/usb/class/` (hub, HID transport, Kconfig), `kernel/drivers/hid/`, `kernel/core/input_inject.c`, `kshim/usb_class.h`, `hid.h`, `input_inject.h`, `user/ui/lvgl_port.c` and `user/bootmenu/bootmenu.c` (BACK/HOVER handling only), `cmake/io/usbhid.cmake`, `tests/io/usbhid.cmake`, `tools/test_qemu_usb_input.py` (the end-to-end test), `configs/qemu_io_defconfig` |
| **touch-st** | `kernel/drivers/input/touch/core.c`, `registry.c`, FTM4 and FTM5 drivers, `Kconfig`, `kshim/touch_driver.h`, `kernel/include/touch.h`, `kernel/drivers/touch.c` (tracker), `kernel/include/st_ftm5.h`, `kernel/drivers/st_ftm5.c`, `kernel/dt/touch.c`, the touch part of `kernel/core/input.c` (everything under `CONFIG_KSHIM_TOUCH`, keeping the injection hooks), the `CONFIG_KSHIM_TOUCH` block of `kernel/dt/resources.c`, `kernel/drivers/input_irq.c` and `kernel/include/input_irq.h` (touch queue parts), the touch lines of `configs/andromeda_defconfig` and `configs/hdk8250_defconfig`, `cmake/io/touch.cmake`, `tests/io/touch.cmake`, the existing touch tests |
| **touch-goodix** | `kernel/drivers/input/touch/goodix_gt911.c`, `goodix_gt9916.c` (plus a shared `goodix_*.h` if needed), `Kconfig.goodix`, `cmake/io/touch_goodix.cmake`, `tests/io/touch_goodix.cmake` |

The following shared files are owned by the architect. Implementers must
not edit them; if a change is needed, ask for it.

- `kernel/core/io.c`, `kshim/io.h`
- `cmake/io.cmake`, `tests/io.cmake`, `tests/io/io_*`
- the `source` lines in `kernel/drivers/Kconfig` and `kernel/Kconfig`
- the single hook lines in `CMakeLists.txt`, `ports/crdk/CMakeLists.txt`,
  `tests/CMakeLists.txt`, `kernel/core/main.c`, `kernel/core/platform.c`,
  `kernel/dt/resources.c` and `kernel/drivers/keys.c`
- the injection hooks in `kernel/core/input.c`
- `include/kshim/input.h`
- this document

Interface changes go through the architect. Additive helper functions in
your own header are fine; record them in your branch's summary.

Cross-implementer dependencies, to be stubbed in host tests until the
dependency is merged:

- qup → sm8650 (clk, pinctrl)
- touch → qup (I2C/SPI), sm8650 (gpio, regulator)
- pci → sm8650 (Qualcomm host: clk, regulator, gpio, pinctrl)
- xhci → pci, firmware loader
- usbhid → xhci (USB core)

Merge order: sm8650, qup, pci, xhci, usbhid, touch-st, touch-goodix. Each
merge stays buildable because every call site is guarded by its own
Kconfig symbol.

## 7. Kconfig

All symbols default to `n`. Board defconfigs are left to the platform
implementer: sm8650 for the new SM8650 boards, and touch-st for the touch
lines of andromeda and hdk8250.

| Symbol | Depends / selects | File (owner) |
| --- | --- | --- |
| `KSHIM_GPIO` (hidden) | selected by REGULATOR, QUP, PCI_QCOM | gpio/Kconfig (sm8650) |
| `KSHIM_CLK` (hidden) | selected by QUP, PCI_QCOM | gpio/Kconfig (sm8650) |
| `KSHIM_REGULATOR` | selects GPIO | regulator/Kconfig (sm8650) |
| `KSHIM_QUP` | selects GPIO, CLK | qup/Kconfig (qup) |
| `KSHIM_I2C` (+ `KSHIM_I2C_GENI` = I2C && QUP) | — | qup/Kconfig (qup) |
| `KSHIM_SPI` (+ `KSHIM_SPI_GENI` = SPI && QUP) | — | qup/Kconfig (qup) |
| `KSHIM_PCI` | — | pci/Kconfig (pci) |
| `KSHIM_PCI_ECAM` | PCI | pci/Kconfig (pci) |
| `KSHIM_PCI_QCOM` | PCI && REGULATOR; selects GPIO, CLK | pci/Kconfig (pci) |
| `KSHIM_USB` | — | usb/Kconfig (xhci) |
| `KSHIM_USB_XHCI` | USB && PCI | usb/Kconfig (xhci) |
| `KSHIM_USB_XHCI_RENESAS` (+ string `_FW`, default `renesas_usb_fw.mem`) | USB_XHCI; uses FIRMWARE_LOADER when set | usb/Kconfig (xhci) |
| `KSHIM_USB_XHCI_RENESAS_FW_EMBED` (string, default empty) | USB_XHCI_RENESAS; a build-time path whose image is linked in and used before any partition (Renesas proprietary firmware: never committed, see section 9) | usb/Kconfig (xhci) |
| `KSHIM_USB_HUB` | USB | usb/class/Kconfig (usbhid) |
| `KSHIM_USB_HID` | USB; selects INPUT_INJECT | usb/class/Kconfig (usbhid) |
| `KSHIM_INPUT_INJECT` (hidden) | selected by USB_HID | usb/class/Kconfig (usbhid) |
| `KSHIM_TOUCH_FTM5`, `KSHIM_TOUCH_FTM4` | TOUCH && I2C && REGULATOR | input/touch/Kconfig (touch-st) |
| `KSHIM_TOUCH_GT911` | TOUCH && I2C && REGULATOR | input/touch/Kconfig.goodix (touch-goodix) |
| `KSHIM_TOUCH_GT9916` | TOUCH && SPI && REGULATOR | input/touch/Kconfig.goodix (touch-goodix) |
| `KSHIM_TOUCH_FRAMEWORK` (derived) | any TOUCH_<chip> | input/touch/Kconfig (touch-st) |

`KSHIM_TOUCH` (existing) is now "Touch and pointer input service". It keeps
the legacy FTM5 path while no `KSHIM_TOUCH_<chip>` is set.

Build hooks:

- `cmake/io.cmake` includes `cmake/io/<owner>.cmake`. These fragments
  append to `KERNEL_SOURCES` and may switch `CRDK_*` options ON, never OFF.
- `ports/crdk/CMakeLists.txt` includes `ports/crdk/io/{pci,platform,qup}.cmake`.
  These fragments append CrDK-relative paths to `CRDK_SOURCES` under their
  own `CRDK_*` option; `CRDK_PCIE` is declared there.
- `tests/io.cmake` includes `tests/io/<owner>.cmake`.

## 8. Test plan

Report exact counts and state plainly what was not run on hardware.

- **Host suite.** `cmake -S tests -B <dir> -G Ninja` with GCC. It had 36
  tests before; this commit adds `io_headers_test` (header contracts,
  descriptor sizes, code ranges, enum growth) and `io_core_test`
  (completion waits over a fake clock, no-op hooks), for 38 tests. Each
  owner adds register-model tests in the `tests/*_test.c` style in its
  `tests/io/<owner>.cmake`:
  - **qup:** a GENI SE model for SE discovery on the mainline and Android
    fixtures (sm8650-qrd, sm8650-pocket-s2, kona, andromeda), ownership
    conflicts (console, i2c4 vs spi4), the inherited vs cold clock path, a
    firmware protocol mismatch, I2C repeated start, NACK and timeout
    recovery, SPI full duplex and CS hold, UART RX IRQ queue, and restore
    on quiesce.
  - **sm8650:** a TLMM model for request, set and restore with refcounts,
    and pinctrl parsing of the Pocket S2 `gamepad_pwr_en` and touch states.
    Regulator-fixed chains (vin-supply, startup delay, inherited-on), RPMh
    name derivation (mainline "ldob14" vs Android `qcom,resource-name`) and
    votes over a fake RSC/CmdDB, restore order, and SM8650 clock table
    lookups.
  - **pci:** an ECAM model with bridge, multifunction and 64-bit BARs:
    enumeration, assignment from ranges, keeping inherited BARs, interrupt-map
    swizzle, the journal replay restoring exactly the inherited config, and
    the Qualcomm host inherit-vs-cold decision over a DBI/PARF/iATU model.
  - **xhci:** an xHCI register and ring model: takeover of a running
    controller, command/event rings, address device, control and interrupt
    transfers, port reset, IRQ vs poll, quiesce (halted and reset). Renesas:
    ROM present, firmware running and download paths.
  - **usbhid:** HID parser on real descriptors (boot keyboard, QEMU
    usb-tablet, an Xbox-style gamepad with hat and axes, a consumer
    control), mapping edges and hysteresis, the injection queue (overflow,
    quiesce), hub port state machine over a fake core, and the
    `kshim_key_source_poll` / `kshim_input_touch_poll` hook delivery.
  - **touch-st / touch-goodix:** framework selection (FTM4 vs FTM5
    match, panel selection), power sequence ordering, the IRQ to deferred
    handle flow, polling fallback, coordinate transform; per-chip protocol
    models (FTM4, FTM5 existing tests kept green, GT911 address select and
    report, GT9916 SPI framing and report).
  - **Cross-stack (board paths):** `touch_goodix_bus_test` runs the Pocket
    S2 GT911 (i2c4) and the QRD8650 GT9916 (spi4) from
    `kshim_input_touch_start/poll` through the touch framework, the I2C/SPI
    cores, the GENI controllers and engine with the CrDK FIFO core, the QUP
    core, the clock, GPIO, pinctrl, regulator and TLMM irqchip drivers and
    `kshim_io_init/quiesce`, with only the GIC, the GENI serial engine (the
    chips as bus devices on it) and the SM8650 TLMM/GCC/RPMh registers
    modelled (docs/touch-goodix.md, Tests).
- **Firmware matrix.** `tools/build_runtime.py --config configs/<x>_defconfig`
  for every defconfig. All 7 passed when this contract was written. New
  defconfigs joined the matrix: `qrd8650` and `ayaneo_pocket_s2` from
  sm8650, `qemu_io` from usbhid, plus `qemu_pci`, `qemu_xhci` and
  `qemu_usb`. Report only real link failures; the image size is not a
  concern. With the I/O stack, andromeda, hdk8250, qrd8650 and
  ayaneo_pocket_s2 outgrew 4 MiB, so their defconfigs set
  `CONFIG_KSHIM_RUNTIME_SLOT_SIZE=0x800000`. The slot is advertised in the
  ARM64 Image header (`image_size`), and MultiBootKernelPatcher takes the
  runtime size from the image, so the packer needs no change.
  `build_runtime.py` and the manifest tests check the image against the
  header's slot instead of a fixed 4 MiB.
- **QEMU.** `tools/test_qemu_irq.py`, `tools/test_qemu_smp.py`,
  `tests/handoff_qemu.py` and `tests/manifest_qemu.py --runtime
  <qemu_manifest_serial kShimC.bin>` stay green. From a worktree, run a
  private copy of `manifest_qemu.py` pointing at
  `/home/sunf/kShim/MultiBootKernelPatcher`. New scripts:
  - `tools/test_qemu_pci.py` (pci): gpex with `-device qemu-xhci` and a
    bridge. It checks enumeration and BARs in the log, then that the
    handoff state equals a direct boot (the `handoff_qemu.py` method,
    extended with the config-space dump of each function).
  - `tools/test_qemu_xhci.py` (xhci): `qemu-xhci` and `nec-usb-xhci`, each
    with `usb-kbd` on a root port. It checks enumeration and descriptors in
    the log, polled and IRQ modes (MSI off), and that the controller is
    halted at handoff.
  - `tools/test_qemu_usb_input.py` (usbhid), the end-to-end test. It runs
    gpex → {qemu-xhci, nec-usb-xhci} → `usb-hub` → `usb-kbd` + `usb-mouse`
    (+ `usb-tablet`) on `configs/qemu_io_defconfig` with the Manifest
    fixture of `manifest_qemu.py`. Keys are sent over QMP
    (`input-send-event` / `sendkey down`, `ret`). It asserts that the serial
    menu moves and boots `PAYLOAD_B_PASS`. The LVGL variant
    (`qemu_manifest_ui` + `usb-tablet`) clicks Boot. It also runs the
    handoff check.
- **Hardware.** Nothing has been tested on hardware. Before claiming
  support, each board needs a serial log showing: inherited vs cold paths
  taken, IRQ vs polled per driver, touch reports, gamepad keys, and a
  payload that boots and finds its devices (Linux: xhci-pci, touchscreen).

## 9. Licensing and provenance

- kShimC is MIT. Every new file starts with `/* SPDX-License-Identifier: MIT */`
  (or `#` in CMake and Kconfig).
- Never copy GPL code. This covers Linux drivers (`pcie-qcom.c`,
  `phy-qcom-qmp-pcie.c`, `xhci*.c`, `xhci-pci-renesas.c`, `hid-*.c`,
  `goodix*.c`, `st/fts*`, `pinctrl-msm.c`, `gcc-sm8650.c`, `qcom-geni-se.c`)
  and the GPL-2.0-only CrDK libraries `RenesasXhciCrLib`, `Dwc3CrLib`,
  `HdkUsbCrLib`, `UsbCrDxe`, `FocalTechTouchCrLib`, plus anything else
  tagged GPL. Check SPDX tags before reading any CrDK file you want to use.
- Hardware facts are fine to use: register addresses and fields, command
  opcodes, event IDs, PHY init register/value tables, DT values, protocol
  framing, and published spec or datasheet facts (xHCI 1.2, USB 2.0/3.2,
  HID 1.11 and Usage Tables, PCI/PCIe base and firmware specs, GT911
  programming guide, the Renesas uPD720201 firmware download sequence).
  Write the code yourself, and record where each fact came from in a short
  comment next to it or in the file header. Linux DTS files under
  `arch/arm64/boot/dts/qcom` are BSD-3-Clause, so their facts can be used.
  The FTM4 Android sources listed in `configs/hdk8250-touch-provenance.json`
  are GPL and are evidence only.
- CrDK is used unmodified through `ports/crdk`: new `CRDK_*` options and
  adapters are fine, but there are no new patches and no commits in
  `lib/CrDK`. A needed CrDK fix is reported as an open issue instead.
- `renesas_usb_fw.mem` is never committed. By default it is loaded at
  runtime from the device's partitions (or used as UEFI left it running).
  `CONFIG_KSHIM_USB_XHCI_RENESAS_FW_EMBED` can link a local copy in at build
  time; a runtime built that way contains Renesas' proprietary firmware and
  must not be redistributed unless Renesas' terms allow it.
- The committed DTB test fixtures are binary builds of BSD-3-Clause Linux
  DTS files; their notices and terms are in
  `tests/fixtures/io/LICENSE.BSD-3-Clause`, with a `.license` sidecar per
  blob.

## 10. Open risks

- The SMMU bypass for PCIe DMA on the Pocket S2 is not programmed by kShimC.
- The vendor partition filesystem on the Pocket S2 is probably EROFS, which
  is GPL opt-in and off; the Renesas firmware then has to come from UEFI,
  `persist`, `efisp` or an embedded build (section 5).
- QUP and PCIe interconnect (ICC/BCM) votes are assumed inherited. A cold
  QUP SE whose QUP core clock was gated by the BCM cannot be revived.
- The CrDK GENI FIFO core is synchronous. The IRQ-driven completion relies
  on a `CrIo.delay_us` that sleeps (section 3.1); if that is not enough, a
  kShimC FIFO engine written from GENI register facts would replace it.
- SM8650 pinctrl function names, GCC clock offsets and the pcie1 QMP tables
  are new kShimC data. Each table needs recorded provenance and a host
  check.
- The GT911 reset polarity in the RFC (`GPIO_ACTIVE_HIGH`) matches the
  goodix,gt9xx convention every mainline goodix,gt911 node uses (logical
  value = RSTB level); it is still unverified on the Pocket S2.
