/* SPDX-License-Identifier: MIT */
#include <kshim_console.h>
#include <config.h>
#include <dt.h>
#include <pl011_uart.h>
#include <Library/cr_debug_uart_poll.h>
#include <kshim/irq.h>
#if CONFIG_KSHIM_CONSOLE_USB_ACM
#include <kshim_mmu.h>
#include <qcom_dt.h>
#include <usb_acm.h>
#include <string.h>
#endif
static struct kshim_console_resource console;
static struct CrDebugUartPoll uart;
static bool initialized;
#if CONFIG_KSHIM_CONSOLE_RX_IRQ
static const void *ConsoleFdt;
#endif

#if CONFIG_KSHIM_CONSOLE_USB_ACM
/*
 * One lock for both backends, so output from the SMP workers does not
 * interleave inside a write and only one CPU drives the USB controller.
 * A ticket lock keeps the boot CPU's 5 ms input polling from starving a
 * worker. Before the MMU is on only the boot CPU runs and exclusives on
 * Device memory are not guaranteed, so plain accesses are used then.
 * Re-entry on the owning CPU (an exception printing from inside the
 * console) is not blocked: it writes the UART only.
 */
static struct { uint32_t next, serving, owner; } lock;

static uint32_t cpu_tag(void)
{
#if defined(__aarch64__)
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(((mpidr >> 8) & 0xFF000000U) | (mpidr & 0xFFFFFFU)) + 1U;
#else
    /* Host tests: one tag per thread stands in for the CPU. */
    static _Thread_local uint32_t tag;
    static uint32_t tags;
    if (!tag) tag = __atomic_add_fetch(&tags, 1, __ATOMIC_RELAXED);
    return tag;
#endif
}

static bool shared_memory(void)
{
#if defined(__aarch64__) && !defined(KSHIM_HOST_TEST)
    return kshim_mmu_is_enabled() != 0;
#else
    return true;
#endif
}

static bool console_lock(bool wait)
{
    uint32_t me = cpu_tag();
    if (__atomic_load_n(&lock.owner, __ATOMIC_RELAXED) == me) return false;
    if (!shared_memory()) {
        if (lock.owner) return false;
        lock.owner = me;
        return true;
    }
    uint32_t ticket = __atomic_load_n(&lock.next, __ATOMIC_RELAXED);
    if (!wait) {
        if (ticket != __atomic_load_n(&lock.serving, __ATOMIC_ACQUIRE) ||
            !__atomic_compare_exchange_n(&lock.next, &ticket, ticket + 1, false,
                                         __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return false;
    } else {
        ticket = __atomic_fetch_add(&lock.next, 1, __ATOMIC_ACQUIRE);
        while (__atomic_load_n(&lock.serving, __ATOMIC_ACQUIRE) != ticket) {
#if defined(__aarch64__)
            __asm__ volatile("yield" ::: "memory");
#endif
        }
    }
    __atomic_store_n(&lock.owner, me, __ATOMIC_RELAXED);
    return true;
}

static void console_unlock(void)
{
    if (!shared_memory()) {
        lock.owner = 0;
        return;
    }
    __atomic_store_n(&lock.owner, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&lock.serving, lock.serving + 1, __ATOMIC_RELEASE);
}

static void acm_init(const void *fdt)
{
    struct kshim_usb_acm_resource resource;
    if (!fdt || kshim_dt_usb_acm(fdt, &resource)) return;
    (void)kshim_usb_acm_start(&resource, kshim_usb_acm_io());
}

int kshim_console_map_resources(struct kshim_resources *r)
{
    const struct kshim_usb_acm_resource *acm = kshim_usb_acm_resource();
    if (!acm) return 0;
    kshim_mmu_region_t saved[KSHIM_RESOURCE_MAX];
    size_t count = r->mapping_count;
    memcpy(saved, r->mappings, count * sizeof(*saved));
    if (!kshim_resource_map(r, acm->base, acm->size, KSHIM_MMU_DEVICE, KSHIM_MMU_READ | KSHIM_MMU_WRITE))
        return 0;
    memcpy(r->mappings, saved, count * sizeof(*saved));
    r->mapping_count = count;
    /* Unmapped once the MMU is on: stop it while it is still reachable. */
    if (console_lock(true)) {
        kshim_usb_acm_stop();
        console_unlock();
    }
    return -1;
}

void kshim_console_service(void)
{
    if (!kshim_usb_acm_active() || !console_lock(false)) return;
    kshim_usb_acm_poll();
    console_unlock();
}
#endif

int kshim_console_init(const void *fdt)
{
    if (initialized) return 0;
    int status = -1;
    if (fdt && !dt_validate(fdt, fdt_totalsize(fdt)) && !kshim_console_probe(fdt, &console)) {
#if CONFIG_KSHIM_CONSOLE_RX_IRQ
        ConsoleFdt = fdt;
#endif
        if (console.geni) {
            const struct CrIo *io = CrIoDefault();
            if (io) {
                uart = (struct CrDebugUartPoll){.io = *io, .base = console.base, .size = console.size};
                status = CrDebugUartPollInit(&uart) ? -1 : 0;
            }
        } else {
            pl011_init(console.base);
            status = 0;
        }
    }
    initialized = status == 0;
#if CONFIG_KSHIM_CONSOLE_USB_ACM
    if (fdt && !dt_validate(fdt, fdt_totalsize(fdt))) acm_init(fdt);
    if (kshim_usb_acm_active()) status = 0;
#endif
    return status;
}

/* A GENI transfer is a multi-register sequence; an interrupt handler that
 * prints must not start another one in the middle (printf from handlers). */
static void uart_write(const char *buffer, uint32_t length)
{
    if (!initialized) return;
    unsigned long flags = kshim_irq_save();
    if (console.geni) (void)CrDebugUartPollWrite(&uart, buffer, length);
    else pl011_write(console.base, (char *)buffer, (int)length);
    kshim_irq_restore(flags);
}

static int uart_try_read(char *character)
{
    if (!initialized) return 0;
#if CONFIG_KSHIM_CONSOLE_RX_IRQ
    /* RX interrupt (kernel/drivers/geni_rx_irq.c) when it is live. */
    int received;
    if (console.geni && !kshim_geni_rx_irq_read(&uart, ConsoleFdt, console.base, character, &received))
        return received;
#endif
    unsigned long flags = kshim_irq_save();
    int status = console.geni ? CrDebugUartPollRead(&uart, (uint8_t *)character) > 0 :
                                pl011_try_read(console.base, character);
    kshim_irq_restore(flags);
    return status;
}

void kshim_console_write(const char *buffer, uint32_t length)
{
    if (!buffer || !length) return;
#if CONFIG_KSHIM_CONSOLE_USB_ACM
    bool locked = console_lock(true);
    uart_write(buffer, length);
    if (locked) {
        kshim_usb_acm_write(buffer, length);
        console_unlock();
    }
#else
    uart_write(buffer, length);
#endif
}

int kshim_console_try_read(char *character)
{
    if (!character) return 0;
#if CONFIG_KSHIM_CONSOLE_USB_ACM
    if (!console_lock(true)) return 0;
    int got = uart_try_read(character);
    if (!got) got = kshim_usb_acm_read(character);
    else kshim_usb_acm_poll();
    console_unlock();
    return got;
#else
    return uart_try_read(character);
#endif
}

uint64_t kshim_console_base(void) { return initialized ? console.base : 0; }
size_t kshim_console_size(void) { return initialized ? console.size : 0; }
int kshim_console_quiesce(void)
{
#if CONFIG_KSHIM_CONSOLE_USB_ACM
    /* Stop DMA and drop the pull-up so the payload's dwc3 driver starts clean. */
    bool locked = console_lock(true);
    kshim_usb_acm_stop();
    if (locked) console_unlock();
#endif
#if CONFIG_KSHIM_CONSOLE_RX_IRQ
    if (kshim_console_rx_irq_stop()) return -1;
#endif
    int status = console.geni ? CrDebugUartPollQuiesce(&uart) : 0;
    if (!status) initialized = false;
    return status;
}
