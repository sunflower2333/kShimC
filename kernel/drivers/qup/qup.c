/* SPDX-License-Identifier: MIT */
/*
 * QUPv3 / GENI serial-engine core (kshim/qup.h, docs/qup.md): the registry
 * of wrappers and SE windows from the runtime DT, per-window ownership, and
 * the clock/pin bring-up of an engine for one protocol.
 *
 * Register facts (GENI SE common block; Linux include/linux/soc/qcom/
 * geni-se.h offsets, mirrored by CrDK Include/Library/cr_geni_regs.h):
 *   GENI_STATUS        +0x40  M_GENI_CMD_ACTIVE bit 0, S_GENI_CMD_ACTIVE bit 12
 *   GENI_FW_REVISION_RO +0x68 PROTOCOL [15:8]: 1 SPI, 2 UART, 3 I2C
 * Clock policy: CrDK Library/ClockLib/clock_poll.c (CrClockPollPrepare):
 * vote the wrapper AHB clocks first, then read GENI_STATUS through them and
 * never retune an engine whose M or S sequencer is active; put the engine
 * clock on CXO 19.2 MHz undivided (verified DFS entry, else the RCG).
 */
#include <kshim/qup.h>
#include <kshim/clk.h>
#include <kshim/io.h>
#include <kshim/pinctrl.h>
#include <kshim/uart.h>
#include <Library/cr_geni.h>
#include <config.h>
#include <dt.h>
#include <stdio.h>
#include <string.h>

#include <kshim_console.h>
#include <qcom_dt.h>
#if CONFIG_QCOM_IRQCHIP
#include <qcom_irq.h>
#endif

#define GENI_STATUS 0x40U
#define GENI_STATUS_M_ACTIVE (1U << 0)
#define GENI_STATUS_S_ACTIVE (1U << 12)
#define GENI_FW_REVISION_RO 0x68U
#define CXO_HZ 19200000U

struct se_state {
    struct kshim_clk *se, *m_ahb, *s_ahb;
    bool se_on, m_on, s_on;     /* kShimC holds an enable on each */
    enum kshim_qup_protocol protocol;
    uint32_t source_hz;
    uint8_t dfs_index;
    unsigned sequence;          /* acquire order, for newest-first release */
};

static struct kshim_qup_wrapper Wrappers[KSHIM_QUP_MAX_WRAPPERS];
static struct kshim_qup_se Ses[KSHIM_QUP_MAX_SE];
static struct se_state State[KSHIM_QUP_MAX_SE];
static unsigned WrapperCount, SeCount, Sequence;
static const void *Fdt;
static const struct CrIo *Io;
/* Windows mapped before the MMU was built (by base); empty while no
 * mapping pass ran (host tests, serial path without resources). */
static uint64_t Mapped[KSHIM_QUP_MAX_SE];
static unsigned MappedCount;
static bool MapPass;

void kshim_qup_set_io(const struct CrIo *io) { Io = io; }

const struct CrIo *kshim_qup_io(void)
{
    if (!Io) Io = CrIoDefault();
    return CrIoValid(Io) ? Io : NULL;
}

const void *kshim_qup_fdt(void) { return Fdt; }

static uint32_t rd(const struct kshim_qup_se *se, uint32_t offset)
{
    const struct CrIo *io = kshim_qup_io();
    return io->read32(io->context, (uintptr_t)se->base + offset);
}

unsigned kshim_qup_se_count(void) { return SeCount; }

struct kshim_qup_se *kshim_qup_se_get(unsigned index)
{
    return index < SeCount ? &Ses[index] : NULL;
}

struct kshim_qup_se *kshim_qup_se_by_node(int node)
{
    if (node < 0) return NULL;
    for (unsigned i = 0; i < SeCount; ++i)
        for (unsigned k = 0; k < 3; ++k)
            if (Ses[i].nodes[k] == node) return &Ses[i];
    return NULL;
}

struct kshim_qup_se *kshim_qup_se_by_base(uint64_t base)
{
    for (unsigned i = 0; i < SeCount; ++i)
        if (Ses[i].base == base) return &Ses[i];
    return NULL;
}

static bool names_window(const struct kshim_qup_se *se, int node)
{
    for (unsigned k = 0; k < 3; ++k)
        if (se->nodes[k] == node) return true;
    return false;
}

static const char *owner_label(enum kshim_qup_owner owner)
{
    switch (owner) {
    case KSHIM_QUP_OWNER_CONSOLE: return "console";
    case KSHIM_QUP_OWNER_I2C: return "i2c";
    case KSHIM_QUP_OWNER_SPI: return "spi";
    case KSHIM_QUP_OWNER_UART: return "uart";
    default: return "none";
    }
}

int kshim_qup_se_claim(struct kshim_qup_se *se, enum kshim_qup_owner owner,
                       int node, const char *owner_name)
{
    if (!se || owner == KSHIM_QUP_OWNER_NONE || !names_window(se, node)) return ERROR_INVALID_PARAMETER;
    if (se->owner == KSHIM_QUP_OWNER_NONE) {
        se->owner = owner;
        se->owner_node = node;
        se->owner_name = owner_name ? owner_name : owner_label(owner);
        return 0;
    }
    if (se->owner == owner && se->owner_node == node) return 0;
    return ERROR_BUSY;
}

int kshim_qup_se_unclaim(struct kshim_qup_se *se)
{
    if (!se) return ERROR_INVALID_PARAMETER;
    if (se->acquired || se->intid) return ERROR_BUSY;
    se->owner = KSHIM_QUP_OWNER_NONE;
    se->owner_node = -1;
    se->owner_name = NULL;
    return 0;
}

int kshim_qup_init(const void *fdt)
{
    if (!fdt) return ERROR_INVALID_PARAMETER;
    if (Fdt == fdt && SeCount) return 0;
    for (unsigned i = 0; i < SeCount; ++i)
        if (Ses[i].acquired || Ses[i].intid ||
            (Ses[i].owner != KSHIM_QUP_OWNER_NONE && Ses[i].owner != KSHIM_QUP_OWNER_CONSOLE))
            return ERROR_BUSY;
    memset(State, 0, sizeof(State));
    Fdt = fdt;
    int status = kshim_dt_qup_discover(fdt, Wrappers, KSHIM_QUP_MAX_WRAPPERS, &WrapperCount,
                                       Ses, KSHIM_QUP_MAX_SE, &SeCount);
    if (status == KSHIM_IO_ERR_OVERFLOW) printf("QUP: registry full, some engines ignored\n");
    else if (status) { SeCount = WrapperCount = 0; Fdt = NULL; return status; }
    /* The console's engine runs from before kShimC; nobody else may take it. */
    struct kshim_qup_se *console = kshim_qup_se_by_base(kshim_console_base());
    if (console) {
        int node = console->node;
        for (unsigned k = 0; k < 3; ++k) {
            int n = console->nodes[k];
            if (n >= 0 && dt_available(fdt, n) && kshim_dt_qup_protocol(fdt, n) == KSHIM_QUP_PROTOCOL_UART) {
                node = n;
                break;
            }
        }
        (void)kshim_qup_se_claim(console, KSHIM_QUP_OWNER_CONSOLE, node, "console");
    }
    printf("QUP: %u wrappers, %u serial engines%s\n", WrapperCount, SeCount,
           console ? ", console engine reserved" : "");
    return 0;
}

int kshim_qup_map_resources(struct kshim_resources *resources)
{
    const void *fdt = resources ? resources->fdt : NULL;
    /* Discovery into scratch tables: the registry is built later by
     * kshim_qup_init(), after the MMU. */
    static struct kshim_qup_wrapper wrappers[KSHIM_QUP_MAX_WRAPPERS];
    static struct kshim_qup_se ses[KSHIM_QUP_MAX_SE];
    unsigned wrapper_count, se_count;
    if (!resources || !fdt) return ERROR_INVALID_PARAMETER;
    MappedCount = 0;
    MapPass = true;
    int status = kshim_dt_qup_discover(fdt, wrappers, KSHIM_QUP_MAX_WRAPPERS, &wrapper_count,
                                       ses, KSHIM_QUP_MAX_SE, &se_count);
    if (status && status != KSHIM_IO_ERR_OVERFLOW) return status;
    for (unsigned i = 0; i < se_count; ++i) {
        if (!kshim_dt_qup_window_used(fdt, &ses[i])) continue;
        if (kshim_resource_map(resources, ses[i].base, ses[i].size, KSHIM_MMU_DEVICE,
                               KSHIM_MMU_READ | KSHIM_MMU_WRITE)) {
            MappedCount = 0;
            return ERROR_NOT_SUPPORTED;
        }
        Mapped[MappedCount++] = ses[i].base;
    }
    return 0;
}

/* ---- Bring-up ------------------------------------------------------------ */

static bool window_mapped(uint64_t base)
{
    for (unsigned m = 0; m < MappedCount; ++m)
        if (Mapped[m] == base) return true;
    return false;
}

static struct se_state *state_of(const struct kshim_qup_se *se)
{
    return se && se >= Ses && se < Ses + SeCount ? &State[se - Ses] : NULL;
}

static int get_clock(int node, const char *const *names, struct kshim_clk **clk)
{
    int status = ERROR_NOT_FOUND;
    for (; *names; ++names) {
        status = kshim_clk_get(Fdt, node, *names, clk);
        if (status != ERROR_NOT_FOUND) return status;
    }
    return status;
}

static int resolve_clocks(struct kshim_qup_se *se, struct se_state *st)
{
    static const char *const se_names[] = {"se", "se-clk", NULL};
    static const char *const m_names[] = {"m-ahb", NULL};
    static const char *const s_names[] = {"s-ahb", NULL};
    int status;
    if (!st->se && (status = get_clock(se->owner_node, se_names, &st->se))) return status;
    if (st->m_ahb && st->s_ahb) return 0;
    /* Mainline: the wrapper carries the AHB clocks; Android: the engine. */
    int holder = se->owner_node;
    if (se->wrapper && fdt_getprop(Fdt, se->wrapper->node, "clock-names", NULL) &&
        dt_string_index(Fdt, se->wrapper->node, "clock-names", "m-ahb") >= 0)
        holder = se->wrapper->node;
    if ((status = get_clock(holder, m_names, &st->m_ahb)) ||
        (status = get_clock(holder, s_names, &st->s_ahb)))
        return status;
    if (se->wrapper && holder == se->wrapper->node) {
        Wrappers[se->wrapper->index].m_ahb = st->m_ahb;
        Wrappers[se->wrapper->index].s_ahb = st->s_ahb;
    }
    return 0;
}

static void drop_clocks(struct se_state *st)
{
    if (st->se_on && !kshim_clk_disable(st->se)) st->se_on = false;
    if (st->s_on && !kshim_clk_disable(st->s_ahb)) st->s_on = false;
    if (st->m_on && !kshim_clk_disable(st->m_ahb)) st->m_on = false;
}

int kshim_qup_se_acquire(struct kshim_qup_se *se, enum kshim_qup_protocol protocol,
                         uint32_t *source_hz, uint8_t *dfs_index)
{
    struct se_state *st = state_of(se);
    bool keep_rate = !source_hz || !dfs_index;
    if (!st || protocol == KSHIM_QUP_PROTOCOL_NONE || se->owner == KSHIM_QUP_OWNER_NONE ||
        se->owner == KSHIM_QUP_OWNER_CONSOLE || se->owner_node < 0)
        return ERROR_INVALID_PARAMETER;
    if (keep_rate && protocol != KSHIM_QUP_PROTOCOL_UART) return ERROR_INVALID_PARAMETER;
    if (!kshim_qup_io()) return ERROR_NOT_READY;
    if (se->acquired) {
        if (st->protocol != protocol) return ERROR_BUSY;
        if (!keep_rate) { *source_hz = st->source_hz; *dfs_index = st->dfs_index; }
        return 0;
    }
    if (MapPass && !window_mapped(se->base)) {
        printf("QUP: engine %#llx is not mapped\n", (unsigned long long)se->base);
        return ERROR_NOT_SUPPORTED;
    }
    int status = resolve_clocks(se, st);
    if (status) return status;
    bool inherited = kshim_clk_is_on(st->m_ahb) && kshim_clk_is_on(st->s_ahb) &&
                     kshim_clk_is_on(st->se);
    /* The AHB window first: every engine register sits behind it. */
    if ((status = kshim_clk_enable(st->m_ahb))) goto fail;
    st->m_on = true;
    if ((status = kshim_clk_enable(st->s_ahb))) goto fail;
    st->s_on = true;
    uint32_t geni_status = rd(se, GENI_STATUS);
    uint8_t dfs = 0;
    if (!keep_rate) {
        if (geni_status & (GENI_STATUS_M_ACTIVE | GENI_STATUS_S_ACTIVE)) {
            status = ERROR_BUSY; /* never retune or take over an active engine */
            goto fail;
        }
        if ((status = kshim_clk_set_rate_cxo(st->se, &dfs))) goto fail;
    }
    if ((status = kshim_clk_enable(st->se))) goto fail;
    st->se_on = true;
    uint32_t loaded = (rd(se, GENI_FW_REVISION_RO) >> 8) & 0xffU;
    if (loaded != (uint32_t)protocol) {
        printf("QUP: engine %#llx runs protocol %u, %u requested\n",
               (unsigned long long)se->base, (unsigned)loaded, (unsigned)protocol);
        status = ERROR_NOT_SUPPORTED;
        goto fail;
    }
    status = kshim_pinctrl_apply(Fdt, se->owner_node, "default");
    if (status == ERROR_NOT_FOUND) status = 0;
    if (status) {
        /* An inherited engine already has its pads muxed. */
        printf("QUP: engine %#llx pinctrl: %s\n", (unsigned long long)se->base, kshim_io_strerror(status));
        if (!inherited) goto fail;
        status = 0;
    }
    st->protocol = protocol;
    st->source_hz = keep_rate ? 0 : CXO_HZ;
    st->dfs_index = dfs;
    st->sequence = ++Sequence;
    se->acquired = true;
    se->inherited_active = inherited;
    if (!keep_rate) { *source_hz = CXO_HZ; *dfs_index = dfs; }
    return 0;
fail:
    drop_clocks(st);
    return status;
}

int kshim_qup_se_release(struct kshim_qup_se *se)
{
    struct se_state *st = state_of(se);
    if (!st) return ERROR_INVALID_PARAMETER;
    if (!se->acquired) return 0;
    drop_clocks(st);
    if (st->se_on || st->s_on || st->m_on) return ERROR_DEVICE_ERROR;
    se->acquired = false;
    se->inherited_active = false;
    st->protocol = KSHIM_QUP_PROTOCOL_NONE;
    return 0;
}

int kshim_qup_se_irq_request(struct kshim_qup_se *se, kshim_irq_handler_t handler,
                             void *context, uint32_t *intid)
{
    if (!state_of(se) || !handler || !intid || se->owner == KSHIM_QUP_OWNER_NONE || se->owner_node < 0)
        return ERROR_INVALID_PARAMETER;
    if (se->intid) return ERROR_BUSY;
    if (!kshim_irq_ready()) return ERROR_NOT_READY;
    uint32_t id = 0;
    int status;
#if CONFIG_QCOM_IRQCHIP
    /* GIC parent, or the PDC (Android: <0 spi 0> under the PDC root, a
     * pass-through). QUP serial-engine interrupts are level high. */
    status = kshim_qcom_irq_request(Fdt, se->owner_node, 0, KSHIM_IRQ_TYPE_LEVEL_HIGH,
                                    handler, context, &id);
#else
    unsigned trigger = 0;
    status = kshim_irq_from_fdt(Fdt, se->owner_node, 0, &id, &trigger);
    if (!status)
        status = kshim_irq_request(id, trigger ? trigger : KSHIM_IRQ_TYPE_LEVEL_HIGH, handler, context);
#endif
    if (status == ERROR_NOT_FOUND || status == ERROR_INVALID_PARAMETER) status = ERROR_NOT_SUPPORTED;
    if (status) return status;
    se->intid = id;
    *intid = id;
    return 0;
}

int kshim_qup_se_irq_free(struct kshim_qup_se *se)
{
    if (!state_of(se)) return ERROR_INVALID_PARAMETER;
    if (!se->intid) return 0;
#if CONFIG_QCOM_IRQCHIP
    int status = kshim_qcom_irq_free(se->intid);
#else
    int status = kshim_irq_free(se->intid);
#endif
    if (status) return status;
    se->intid = 0;
    return 0;
}

int kshim_qup_quiesce_all(void)
{
    /* DT UART instances restore DebugUartLib's snapshot and release their
     * engines themselves; I2C/SPI quiesced theirs before this step. */
    int result = kshim_uart_quiesce_all();
    bool tried[KSHIM_QUP_MAX_SE] = {false};
    /* Newest acquisition first. */
    for (;;) {
        struct kshim_qup_se *newest = NULL;
        for (unsigned i = 0; i < SeCount; ++i) {
            struct kshim_qup_se *se = &Ses[i];
            if (tried[i] || !se->acquired || se->owner == KSHIM_QUP_OWNER_CONSOLE) continue;
            if (!newest || State[i].sequence > State[newest - Ses].sequence) newest = se;
        }
        if (!newest) break;
        tried[newest - Ses] = true;
        int status = kshim_qup_se_irq_free(newest);
        if (!status) status = kshim_qup_se_release(newest);
        if (status) {
            printf("QUP: engine %#llx release failed (%d)\n", (unsigned long long)newest->base, status);
            if (!result) result = status;
        }
    }
    return result;
}
