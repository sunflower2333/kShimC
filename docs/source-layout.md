# Source layout: kernel and userspace

kShimC is one binary that runs at a single privilege level. The tree is still
split into a **kernel** (low-level runtime and hardware) and a **userspace**
(the boot-menu application and its policy). This is a source-layout and API
boundary, not EL0 privilege separation. The aim is that the UI can change
without touching drivers, and that new kernel subsystems do not pull in UI code.

## Layout

```
include/kshim/        Public kernel API: the only kernel headers userspace may include
kernel/
  Kconfig             Architecture, drivers, input, SMP, display resources, handoff, debug
  arch/               arm64 (startup, MMU, exceptions, GICv3, timer, PSCI, SMP) and riscv
  core/               main (Manifest parsing, image handoff), platform services, power,
                      UEFI payload handoff, input and display services
  dt/                 Device-tree parsing (console, keys, touch, splash, SPMI, power)
  drivers/            console, pl011, Qualcomm keys, touch, ST FTM5, power sequencing
  lib/                picoport (picolibc system-call port)
  include/            Kernel-internal headers (arch, DT, drivers, MMU, SMP, ...)
user/
  Kconfig             LVGL menu, menu styles, menu entry names
  bootmenu/           Boot-entry selection: LVGL loop, serial fallback, default main loop
  ui/                 LVGL menu port, designs/themes, power menu, fonts, backdrop,
                      framebuffer writer (presentation)
  lvgl/               LVGL OS layer (threads/mutexes on kernel SMP workers) and its
                      QEMU self-test stage
  include/            Userspace headers (lvgl_port.h, framebuffer.h, lv_conf.h, ...)
```

`ports/` (CrDK and LVGL build adapters), `lib/` and `third_party/` are unchanged.

### Storage and firmware subsystems

- `kernel/block/`: blkdev registry, RAM disk and GPT partition scanning.
- `kernel/drivers/block/`: block device drivers (UFS, SDHCI).
- `kernel/fs/`: read-only filesystems over CrDK FirmwareLoaderLib, plus liblp
  `super` logical partitions.
- `kernel/firmware/`: the firmware loader and Android boot-slot capture.

Their headers (`blkdev.h`, `block.h`, `storage.h`, `fs.h`, `firmware.h`) are
kernel-internal and live in `kernel/include/kshim/`, because they depend on
`error_no.h`. Promote a header to `include/kshim/` only once userspace needs it
and it no longer pulls in kernel-internal headers. Build glue is in
`cmake/storage.cmake`; see `docs/firmware-loader.md`.

## Boundary API (`include/kshim/`)

| Header | Provides |
| --- | --- |
| `display.h` | `kshim_fb_format_t`, `kshim_framebuffer_config_t`, `kshim_fb_format_bpp()`, `kshim_display_get()` (scanout described by DT or the QEMU window), `kshim_display_scratch()` (spare RAM for fonts and textures) |
| `input.h` | Abstract key source (`kshim_key_source_t`, `kshim_key_source_poll()`, `kshim_input_keys_init()`, `kshim_input_keys()`), touch event types and the touch service (`kshim_input_touch_start()` / `_poll()`) |
| `power.h` | `kshim_power_action_t`, capability bits, `kshim_platform_power_init()` / `_execute()` |
| `boot.h` | `kshim_menu_action_t` and `kshim_menu_activate()` (the board/UEFI handoff hook) |
| `platform.h` | `kshim_platform_accelerate()` (start PSCI workers and enable the MMU), `kshim_platform_prepare_handoff()` |
| `smp.h` | Worker offload: CPU counts, `kshim_smp_submit()` / `_join()`, status codes, time |
| `time.h` | `delay()` (microseconds) and `get_current_time()` (milliseconds) |
| `console.h` | `kshim_console_write()` and `kshim_console_try_read()` |
| `user.h` | **Upcalls**: entry points userspace implements and the kernel calls (`kshim_user_select()`, `kshim_user_main()`, `kshim_user_prepare_handoff()`, `kshim_user_smp_selftest()`) |

The generated `config.h` (Kconfig) is shared by both sides. libc (picolibc) is
available to both sides. LVGL itself (`third_party/lvgl`) is a userspace library
and is built with `user/include`, because `lv_conf.h` and `kshim_lvgl_os.h`
configure it.

## Rules

1. Userspace (`user/`) includes only `<kshim/...>`, its own `user/include` and
   `user/ui` headers, LVGL, libc and `config.h`. It never includes anything from
   `kernel/include`. The build enforces this: kernel and userspace compile as
   separate CMake object libraries, and only the kernel one gets `kernel/include`
   on its include path.
2. The kernel never includes userspace headers or LVGL. It reaches userspace only
   through the upcalls declared in `kshim/user.h`.
3. When userspace needs something that lives in a driver, add a thin accessor
   to `include/kshim/` and implement it in the kernel (see `input.h`). Do not
   move driver structs into the public headers.
4. Kernel internals may include a public header to share a type (for example
   `kernel/include/touch.h` includes `<kshim/input.h>` for touch events), so the
   types are defined once.
5. Host tests in `tests/` may include both sides, because they unit-test internals.

## Notes on the split

- **Boot-entry selection** used to live in `core/platform.c`. The menu loop,
  the serial fallback and the default two-entry loop are now in
  `user/bootmenu/bootmenu.c`. The kernel keeps the hardware parts: building
  Qualcomm keys from DT, FTM5/QUP touch bring-up, quiescing them, SMP/MMU
  bring-up and handoff. `kernel/core/main.c` still parses the Manifest. It calls
  `kshim_user_select()` to pick an entry.
- **LVGL OS glue** (`user/lvgl/lvgl_os.c`) depends only on LVGL and the public
  SMP offload API, so it is userspace, like a threads library on top of
  syscalls. The LVGL stage of the QEMU SMP self-test is next to it
  (`user/lvgl/lvgl_selftest.c`). The kernel self-test (`kernel/arch/arm64/smp_selftest.c`)
  calls it through `kshim_user_smp_selftest()`.
- **Framebuffer**: the kernel describes the scanout (`kshim_framebuffer_config_t`,
  from DT, SDE or Kconfig). The pixel writer and blitter (`user/ui/framebuffer.c`)
  are presentation code and live in userspace.
- **Handoff**: `kshim_platform_prepare_handoff()` quiesces touch, calls
  `kshim_user_prepare_handoff()` (LVGL teardown and LVGL worker shutdown), then
  parks the SMP workers or the MMU. The order is the same as before the split.
