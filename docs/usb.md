# USB host: core, xHCI and the Renesas firmware loader

Owner: xhci. Interfaces: `kshim/usb.h`, `kshim/usb_hcd.h`, `kshim/dma.h`
(see `docs/io-stack.md` for how they fit the I/O stack). Nothing here has
run on hardware; QEMU and host register models are the evidence so far.

## Files

| Path | Content |
| --- | --- |
| `kernel/core/dma.c` | DMA pool (`kshim/dma.h`) |
| `kernel/drivers/usb/core/core.c` | HCD registry, URBs, bouncing, given-back list, sync helpers, class registry, poll/settle, handoff |
| `kernel/drivers/usb/core/enumerate.c` | Enumeration, configuration choice, class binding, root-port state machine, disconnect |
| `kernel/drivers/usb/core/desc.c` | Descriptor iteration, speed names |
| `kernel/drivers/usb/host/xhci.c` | Controller lifecycle, rings, commands, event ring, IRQ ack |
| `kernel/drivers/usb/host/xhci_ops.c` | `struct kshim_usb_hcd_ops`: ports, slots/contexts, TDs, transfer events, cancel |
| `kernel/drivers/usb/host/xhci_pci.c` | PCI driver (class 0x0c0330, 1912:0014/0015, 1033:0194), Renesas preparation |
| `kernel/drivers/usb/host/renesas_fw.c` | uPD720201/uPD720202 firmware download over config space |
| `kernel/drivers/usb/core/selftest.c` | Test only: HID/BOT self-test class driver (`KSHIM_USB_SELFTEST`) |

## Execution model

- The xHCI interrupt handler reads USBSTS/IMAN, clears EINT and IP
  (keeping IE) and sets `hcd->irq_pending`. Nothing else runs in IRQ
  context. Event Handler Busy stays set until thread context consumed the
  events and wrote ERDP, which holds off the next interrupt (xHCI 4.17.2).
  An IP that survives its acknowledge (an INTx level that will not drop)
  100 times in a row makes the handler clear INTE/IE; the controller then
  runs on the polling path and thread context logs it.
- `kshim_usb_poll_events()` runs each HCD's `ops->poll` (the event ring) in
  thread context. HCDs hand finished URBs to `kshim_usb_hcd_giveback()`,
  which only queues them; the core then copies bounced IN data back,
  completes `urb->done` and runs `urb->complete`. Callbacks are never
  nested inside an HCD op or another callback, so a command wait inside an
  op (which processes the event ring directly) never runs a callback.
- Synchronous helpers wait with `kshim_completion_wait()` pumping
  `kshim_usb_poll_events()`: WFI between checks while interrupts are live,
  so the interrupt ends the wait at once; the same pump is the polling
  fallback when the interrupt is missing, sealed or never delivered.
  A synchronous call from a callback returns `ERROR_BUSY`.
- `kshim_usb_cancel_urb()` runs the CANCELLED callback before it returns
  (unless called from a callback), so a class driver may free its state
  right after cancelling in `disconnect`.
- `kshim_usb_poll()` adds root-port handling and class `service` hooks and
  is registered as an input-source pump when `CONFIG_KSHIM_INPUT_INJECT`.
  `kshim_usb_settle()` polls for at least 250 ms after a controller
  registered (attach signalling plus the 100 ms debounce), then until no
  root port is pending or debouncing and no class driver asked for more
  time with `kshim_usb_settle_extend()`.

## xHCI (xHCI 1.2 specification)

Start (`xhci_start`, 4.2):
1. Capability registers, CNR clear.
2. Takeover: a controller the previous firmware left running (R/S=1 or
   not HCHalted, e.g. UEFI XhciDxe on the Pocket S2) is stopped first
   (R/S=0, wait HCHalted) because its rings are not ours.
3. BIOS handoff (7.1): set OS Owned, wait up to 100 ms for BIOS Owned to
   clear, else take it (no SMM on these platforms); disable the SMI enables
   and clear latched SMI events. Both registers are snapshot first.
4. Renesas only: if an earlier owner left a 64-bit address (non-zero
   upper half of DCBAAP, ERSTBA or ERDP), clear DCBAAP, CRCR and every
   interrupter's ERSTBA/ERDP while halted; these parts keep upper halves
   across a reset.
5. HCRST, CNR clear; PAGESIZE; DCBAA (MaxSlotsEn = min(MaxSlots, 32));
   scratchpad array and pages; command ring (64 TRBs); one event-ring
   segment (256 TRBs) and its ERST; interrupter 0 (IMOD 40 us, IE when an
   interrupt was granted); R/S=1; port power when PPC.

Rings: one segment each with a Link TRB (Toggle Cycle). A TD's first TRB is
written with the inverted cycle bit and handed over last (4.11.1). Data is
split at 64 KiB boundaries with TD Size; IN TRBs carry ISP; the last TRB of
a TD carries IOC; a TD continuing across the Link TRB sets its chain bit.
Free space is computed from the software dequeue and enqueue positions.

Transfers: control = Setup (IDT, TRT) / Data (DIR) / Status (opposite
direction, IOC) stage TDs; bulk and interrupt = Normal TRBs, optionally a
zero-length TD (`KSHIM_USB_URB_ZERO_PACKET`). Short packet: control waits
for the Status stage event; other TDs end there (4.10.1.1); a possible
later event for the same TD is ignored. STALL, babble, transaction and TRB
errors halt the endpoint: the next submission (EP0) or
`kshim_usb_clear_halt()` runs Reset Endpoint and Set TR Dequeue to the next
queued TD. Cancel: Stop Endpoint unless already stopped/halted; the
controller's current TD is skipped with Set TR Dequeue, a later TD is
turned into No Op TRBs.

Devices: Enable Slot; Address Device (BSR=0) with route string, speed, root
port and TT hub slot/port for LS/FS behind a HS hub; Evaluate Context for a
new EP0 max packet size; Configure Endpoint for the selected configuration
(isochronous endpoints are not enabled); hub fields (Hub, ports, TTT, MTT)
through Configure Endpoint with only the slot context; Disable Slot before
a device's buffers are released.

Ports: PORTSC writes keep the RW bits (PP, PIC, wake enables) and write
zero to every RW1C/RW1S bit not meant. USB2 ports reset with PR; USB3 ports
that trained to U0 are used as is, otherwise warm reset. Port protocol
comes from the Supported Protocol capabilities.

Stop (`xhci_stop`, handoff): INTE/IE off, R/S=0, wait HCHalted (a failure
refuses the handoff and keeps the DMA memory), give back every URB, HCRST
(clears DCBAAP, CRCR, CONFIG, the interrupter), restore the legacy-support
semaphores and SMI enables, free the DMA memory. Idempotent. A downloaded
Renesas image stays loaded. The PCI core then frees the interrupt and
replays its config journal (COMMAND, BARs).

## Renesas uPD720201 / uPD720202

Facts and sequence: `kernel/drivers/usb/host/renesas_fw.h` (Renesas user's
manual R19UH0078EJ: registers 0x6C, 0xF4, 0xF6, 0xF8, 0xFC and the
download procedure). Written independently of CrDK `RenesasXhciCrLib`
(GPL-2.0-only) and Linux.

Decision at probe, before memory decode or MMIO:
- firmware result "success" (with or without the lock): already running,
  used as is (the expected case under UEFI XhciDxe);
- external ROM present with a successful ROM load: used as is;
- lock without success, a stale FW Download Enable, or an error result:
  refused (needs a reset kShimC does not issue);
- otherwise download `CONFIG_KSHIM_USB_XHCI_RENESAS_FW`
  ("renesas_usb_fw.mem") from the Android partitions with
  `kshim_firmware_request()` (default locations, `super/vendor{slot}/
  firmware` among them) into 64 KiB of DMA-pool scratch returned right
  after the download. `CONFIG_KSHIM_USB_XHCI_RENESAS_FW_EMBED` may name an
  image to link in at build time (default empty): it is the loader's
  builtin fallback after confirmed absence, or the only source without
  the loader. Never commit the image; check its redistribution terms.

All 0xF4-0xFC writes use `kshim_pci_write_config_raw32()`: they are never
journaled or replayed. The 0xF4 dword write keeps the ROM half (0xF6) at
zero, its idle state, which is checked before the download starts.

## DMA pool

Static, page-aligned array in the runtime image (`CONFIG_KSHIM_DMA_POOL_SIZE`,
default 256 KiB), 64-byte units (cache lines), first-fit bitmap with
alignment, no-crossing boundary and address limit. It holds the xHCI
DCBAA, scratchpads, rings, contexts, per-slot bookkeeping, transfer bounce
buffers and the Renesas image during its download. About 8 KiB per
enumerated device. Buffers are identity mapped (VA = PA = bus address).
`kshim_dma_sync_for_device()` cleans, `kshim_dma_sync_for_cpu()` cleans and
invalidates; with the MMU off both reduce to barriers in effect.

## Kconfig

| Symbol | Meaning |
| --- | --- |
| `KSHIM_USB` | Core and DMA pool |
| `KSHIM_DMA_POOL_SIZE` | Pool bytes (hex) |
| `KSHIM_USB_XHCI` | xHCI on PCI (needs `KSHIM_PCI`) |
| `KSHIM_USB_XHCI_FORCE_POLL` | Never request the interrupt (bring-up/test) |
| `KSHIM_USB_XHCI_RENESAS` | Renesas firmware download |
| `KSHIM_USB_XHCI_RENESAS_FW` | Firmware file name |
| `KSHIM_USB_XHCI_RENESAS_FW_EMBED` | Optional build-time image path |
| `KSHIM_USB_SELFTEST`, `_MS` | Test only: HID/BOT self-test class driver |

## Additions to the architect's headers (additive)

- `kshim/usb_hcd.h`: `kshim_usb_hcd_giveback(urb, status, actual)`.
- `kshim/usb.h`: `kshim_usb_settle_extend(ms)`, `kshim_usb_speed_name()`;
  URB fields `done_next`, `bounce_size`, `in` (core-owned); `tt_hub_slot`
  is filled by the HCD in `device_alloc` (the core fills `tt_port`).
- `kshim/dma.h`: `kshim_dma_owns()`, `kshim_dma_in_use()`,
  `kshim_dma_pool_size()`.

## Tests

- Host (`tests/io/usb.cmake`): `usb_dma_test`, `usb_renesas_test`
  (register model of the download plus the PCI glue's decisions with a
  fake loader) and `usb_xhci_test` with a behavioural xHCI model
  (`tests/io/xhci_model.c`), run as the scenarios irq, poll, lostirq,
  takeover (with port power control), dead and stuck.
- QEMU: `tools/test_qemu_xhci.py` (qemu-xhci and nec-usb-xhci; usb-kbd
  and usb-storage; interrupt-driven and polled; EL1 and EL2) compares the
  xHCI function a payload finds after handoff with a direct boot. It runs
  on the real PCI core (`KSHIM_PCI_ECAM`, QEMU gpex); the test-only bus-0
  enumerator it used before the PCI merge was removed.

## Hardware risks (unverified)

- Pocket S2: SMMU stream 0x1481 of pcie1 must be in bypass for xHCI DMA;
  PCIe may be non-coherent (cache maintenance is done either way).
- uPD720201 MaxScratchpadBufs, PPC and BIOS semaphore state as left by
  UEFI XhciDxe are unknown; the handoff wait is bounded at 100 ms.
- The Renesas sequence is written from the manual's description; the
  0xF4 dword write (ROM half zero) and the handshake timing were only
  exercised against a model.
- `renesas_usb_fw.mem` may live on an EROFS vendor partition (GPL reader,
  opt-in) or be absent; then only an already-running image works.
