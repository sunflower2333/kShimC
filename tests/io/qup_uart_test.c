/* SPDX-License-Identifier: MIT */
/*
 * UART layer over a GENI UART register model and the fake GIC/PDC, on the
 * andromeda DT: the console's early MMIO instance (polled TX/RX, RX
 * interrupt queue with the newest bytes kept on overflow, a lost latch,
 * stop and inherited restore), a DT instance through its QUP serial engine
 * (claim, clocks kept at the inherited rate, console engine reserved), and
 * PL011.
 */
#include <kshim/uart.h>
#include <kshim/qup.h>
#include <kshim/io.h>
#include <qcom_dt.h>
#include <Library/cr_geni_regs.h>

#include "qup_fakes.h"
#include "../irq_fake.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- PL011 fake ---------------------------------------------------------- */
static uint64_t Pl011Base;
static char Pl011Out[64], Pl011In;
static unsigned Pl011OutLen;
void pl011_init(uint64_t base) { Pl011Base = base; }
void pl011_write(uint64_t base, char *s, int n)
{
    assert(base == Pl011Base && n >= 0 && Pl011OutLen + (unsigned)n <= sizeof(Pl011Out));
    memcpy(Pl011Out + Pl011OutLen, s, (size_t)n);
    Pl011OutLen += (unsigned)n;
}
int pl011_try_read(uint64_t base, char *c)
{
    assert(base == Pl011Base);
    if (!Pl011In) return 0;
    *c = Pl011In;
    Pl011In = 0;
    return 1;
}

/* ---- GENI UART model (DebugUartLib's register use) ------------------------ */
static uintptr_t Se;
static uint32_t RxWords[64], RxLast;
static unsigned RxCount, TxBytes;
static char TxText[64];

static uint32_t uart_read(uintptr_t a)
{
    switch (a - Se) {
    case 0x68: return 2U << 8;                   /* UART firmware */
    case 0xe24: case 0xe28: return 16U << 16;    /* FIFO depths */
    case 0x804: return RxCount | (RxCount == 1 && RxLast ? (1U << 31) | (RxLast << 28) : 0);
    case 0x780: {
        assert(RxCount);
        uint32_t w = RxWords[0];
        memmove(RxWords, RxWords + 1, --RxCount * 4U);
        if (!RxCount) RxLast = 0;
        return w;
    }
    default: return fake_peek(a);
    }
}
static void uart_write(uintptr_t a, uint32_t v)
{
    switch (a - Se) {
    case 0x648: fake_poke(Se + 0x640, fake_peek(Se + 0x640) & ~v); return;  /* S_IRQ_CLEAR */
    case 0x618: fake_poke(Se + 0x610, fake_peek(Se + 0x610) & ~v); return;  /* M_IRQ_CLEAR */
    case 0x630: fake_poke(Se + 0x40, fake_peek(Se + 0x40) | (1U << 12)); return; /* RX on */
    case 0x634:                                                                /* RX abort */
        fake_poke(Se + 0x40, fake_peek(Se + 0x40) & ~(1U << 12));
        fake_poke(Se + 0x640, fake_peek(Se + 0x640) | (1U << 5));
        return;
    case 0x600: TxBytes = 0; return;
    case 0x700:
        for (unsigned b = 0; b < 4 && TxBytes < fake_peek(Se + 0x270); ++b)
            if (TxBytes < sizeof(TxText)) TxText[TxBytes++] = (char)(v >> (8 * b));
        if (TxBytes >= fake_peek(Se + 0x270)) fake_poke(Se + 0x610, fake_peek(Se + 0x610) | 1U);
        return;
    default: fake_poke(a, v);
    }
}
static void receive(const char *text, size_t n)
{
    for (size_t i = 0; i < n; i += 4) {
        uint32_t w = 0;
        for (size_t j = 0; j < 4 && i + j < n; ++j) w |= (uint32_t)(uint8_t)text[i + j] << (8 * j);
        assert(RxCount < 64);
        RxWords[RxCount++] = w;
    }
    RxLast = n % 4 ? (uint32_t)(n % 4) : 0;
    fake_poke(Se + 0x640, fake_peek(Se + 0x640) | (1U << 26) | (RxLast ? 1U << 27 : 0));
}
static bool uart_line(void)
{
    return (fake_peek(Se + 0xe1c) & 8U) &&
           (fake_peek(Se + 0x640) & fake_peek(Se + 0x644) & ((1U << 26) | (1U << 27)));
}
static void uart_model(uintptr_t base)
{
    Se = base;
    RxCount = RxLast = TxBytes = 0;
    fake_mmio_region(base, 0x4000, uart_read, uart_write);
    /* The boot chain left the M (TX) interrupt enabled; DebugUartLib masks it. */
    fake_poke(Se + 0xe1c, 4U);
    fake_poke(Se + 0x614, 1U);
}

static void world(void)
{
    fake_reset();
    fake_clk_reset();
    kshim_qup_set_io(&FakeIo);
    kshim_uart_set_io(&FakeIo);
}

static void test_console_instance(const void *f)
{
    static struct kshim_uart uart;
    const uint32_t intid = KSHIM_IRQ_SPI(357);
    uint8_t c;
    world();
    uart_model(0xa90000);
    fake_gic_line(intid, uart_line);
    assert(!kshim_uart_open_mmio(&uart, KSHIM_UART_GENI, 0xa90000, 0x4000));
    assert(uart.open && uart.kind == KSHIM_UART_GENI && !uart.se);
    assert(kshim_uart_open_mmio(&uart, KSHIM_UART_GENI, 0xa90000, 0x4000) == ERROR_BUSY);
    assert(fake_peek(Se + 0xe1c) == 0 && fake_peek(Se + 0x614) == 0);

    /* Polled TX and RX before the GIC. */
    assert(!kshim_uart_write(&uart, "hello", 5) && TxBytes == 5 && !memcmp(TxText, "hello", 5));
    assert(!FakeIrqMasked);
    receive("x", 1);
    assert(kshim_uart_read_byte(&uart, &c) == 1 && c == 'x');
    assert(kshim_uart_read_byte(&uart, &c) == 0);
    fake_poke(Se + 0x640, 0);
    FakeIrqReady = false;
    assert(kshim_uart_rx_irq_start(&uart, f, -1) == ERROR_NOT_READY);

    /* RX interrupt: only GENI_S and the two RX status bits are set. */
    FakeIrqReady = true;
    assert(!kshim_uart_rx_irq_start(&uart, f, -1) && uart.rx_irq && uart.intid == intid);
    assert(!kshim_uart_rx_irq_start(&uart, f, -1));
    assert(FakeGic[intid].requested && FakeGic[intid].trigger == KSHIM_IRQ_TYPE_LEVEL_HIGH);
    assert(fake_peek(Se + 0xe1c) == 8U && fake_peek(Se + 0x644) == ((1U << 26) | (1U << 27)));
    assert(fake_peek(Se + 0x614) == 0); /* TX interrupt untouched */
    receive("ok\r", 3);
    assert(uart_line() && fake_gic_run() == 1 && !RxCount && !uart_line());
    for (const char *p = "ok\r"; *p; ++p) assert(kshim_uart_read_byte(&uart, &c) == 1 && c == (uint8_t)*p);
    assert(kshim_uart_read_byte(&uart, &c) == 0);

    /* Overflow: the queue keeps the newest KSHIM_UART_RX_QUEUE bytes. The
     * 16-word FIFO takes 64 bytes per interrupt. */
    char burst[300];
    for (unsigned i = 0; i < 300; ++i) burst[i] = (char)i;
    for (unsigned off = 0; off < 300; off += 64) {
        receive(burst + off, 300 - off < 64 ? 300 - off : 64);
        assert(fake_gic_run() == 1);
    }
    assert(uart.rx_dropped == 300 - KSHIM_UART_RX_QUEUE);
    for (unsigned i = 300 - KSHIM_UART_RX_QUEUE; i < 300; ++i)
        assert(kshim_uart_read_byte(&uart, &c) == 1 && c == (uint8_t)i);
    assert(kshim_uart_read_byte(&uart, &c) == 0);

    /* A byte whose latch was lost is read by the next poll. */
    receive("z", 1);
    fake_poke(Se + 0x640, 0);
    assert(fake_gic_run() == 0);
    assert(kshim_uart_read_byte(&uart, &c) == 1 && c == 'z');
    /* TX stays polled while RX is interrupt-driven. */
    assert(!kshim_uart_write(&uart, "hi", 2) && !memcmp(TxText, "hi", 2));

    /* Stop: kShimC's bits back, SPI freed; queued bytes still read. */
    receive("q", 1);
    assert(fake_gic_run() == 1);
    assert(!kshim_uart_rx_irq_stop(&uart) && !uart.rx_irq && !FakeGic[intid].requested);
    assert(fake_peek(Se + 0xe1c) == 0 && fake_peek(Se + 0x644) == 0);
    assert(!kshim_uart_rx_irq_stop(&uart));
    assert(kshim_uart_read_byte(&uart, &c) == 1 && c == 'q');
    assert(kshim_uart_read_byte(&uart, &c) == 0);

    /* Quiesce: DebugUartLib's snapshot (the inherited enables) is back. */
    assert(!kshim_uart_rx_irq_start(&uart, f, -1));
    assert(!kshim_uart_quiesce(&uart) && !uart.open && !FakeGic[intid].requested);
    assert(fake_peek(Se + 0xe1c) == 4U && fake_peek(Se + 0x614) == 1U);
    assert(kshim_uart_write(&uart, "x", 1) == ERROR_NOT_READY);
    assert(!kshim_uart_quiesce(&uart) && !FakeIrqMasked);
    printf("console instance: ok\n");
}

static void test_dt_instance(const void *f)
{
    static struct kshim_uart uart, other;
    world();
    FakeConsoleBase = 0xa90000;
    assert(!kshim_qup_init(f));
    uart_model(0xc8c000);
    struct kshim_qup_se *se = kshim_qup_se_by_base(0xc8c000), *console = kshim_qup_se_by_base(0xa90000);
    assert(se && console && console->owner == KSHIM_QUP_OWNER_CONSOLE);
    assert(kshim_uart_open_dt(&uart, f, se->node, 115200) == ERROR_NOT_SUPPORTED);
    assert(kshim_uart_open_dt(&other, f, console->node, 0) == ERROR_BUSY);
    /* The HS UART: claimed, clocks enabled, the rate left alone. */
    fake_poke(Se + 0x40, 1U << 12); /* its RX command is running */
    assert(!kshim_uart_open_dt(&uart, f, se->node, 0));
    assert(uart.se == se && se->owner == KSHIM_QUP_OWNER_UART && se->acquired);
    struct kshim_clk *clk = fake_clk_of(f, se->node, "se-clk");
    assert(clk->on && clk->ours && !clk->rate_calls);
    assert(!kshim_uart_write(&uart, "at", 2));
    /* interrupts-extended <&intc 0 0x249 0>, <&tlmm 55 0>: the GIC entry. */
    assert(!kshim_uart_rx_irq_start(&uart, f, se->node) && uart.intid == KSHIM_IRQ_SPI(0x249));
    assert(!kshim_uart_quiesce(&uart));
    assert(!se->acquired && se->owner == KSHIM_QUP_OWNER_NONE && !fake_clk_enabled_by_us());
    assert(!FakeGic[KSHIM_IRQ_SPI(0x249)].requested);
    printf("DT instance: ok\n");
}

static void test_pl011(const void *f)
{
    static struct kshim_uart uart;
    uint8_t c;
    world();
    assert(kshim_uart_open_mmio(&uart, KSHIM_UART_PL011, 0x9000000, 0x80) == ERROR_INVALID_PARAMETER);
    memset(&uart, 0, sizeof(uart));
    assert(!kshim_uart_open_mmio(&uart, KSHIM_UART_PL011, 0x9000000, 0x1000) && Pl011Base == 0x9000000);
    assert(!kshim_uart_write(&uart, "pl", 2) && Pl011OutLen == 2 && !memcmp(Pl011Out, "pl", 2));
    Pl011In = 'k';
    assert(kshim_uart_read_byte(&uart, &c) == 1 && c == 'k' && kshim_uart_read_byte(&uart, &c) == 0);
    assert(kshim_uart_rx_irq_start(&uart, f, -1) == ERROR_NOT_SUPPORTED);
    assert(!kshim_uart_quiesce(&uart) && !uart.open);
    printf("pl011: ok\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    void *andromeda = qup_load_fixture(FIXTURES, "andromeda-fdt.dtb");
    test_console_instance(andromeda);
    test_dt_instance(andromeda);
    test_pl011(andromeda);
    free(andromeda);
    puts("qup_uart_test: ok");
    return 0;
}
