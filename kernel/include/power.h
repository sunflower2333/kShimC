/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum {
    KSHIM_POWER_NONE = 0, KSHIM_POWER_OFF, KSHIM_POWER_WARM,
    KSHIM_POWER_HARD, KSHIM_POWER_EDL
} kshim_power_action_t;
#define KSHIM_POWER_BIT(a) (1U << (unsigned)(a))
#define KSHIM_POWER_ALL 0x1eU
#define KSHIM_POWER_MAX_PMICS 4U
/* Negative results never mean that a requested reset took place. */
enum { KSHIM_POWER_UNSUPPORTED=-1, KSHIM_POWER_IO=-2,
       KSHIM_POWER_RETURNED=-3, KSHIM_POWER_UNSAFE=-4 };
struct kshim_power_pmic { uint16_t base; uint8_t controller, bus, sid, revision, subtype, store_reason, legacy_reason; };
struct kshim_power_config {
    struct kshim_power_pmic pmic[KSHIM_POWER_MAX_PMICS];
    unsigned count;
    uintptr_t pshold, edl, dload, reason, tcsr;
    uint32_t capabilities, blocked;
    uint8_t pon_ready, scm_dload, scm_arbiter, scm_pshold, scm_sdi, psci_off, scm_io;
    uint8_t downstream, force_warm, unsupported_pon;
};
struct kshim_power_ops {
    void *context;
    int (*read8)(void *, const struct kshim_power_pmic *, uint16_t, uint8_t *);
    int (*write8)(void *, const struct kshim_power_pmic *, uint16_t, uint8_t);
    int (*write32)(void *, uintptr_t, uint32_t);
    void (*delay_us)(void *, uint32_t);
    int (*scm)(void *, unsigned, unsigned, unsigned, uint64_t, uint64_t, uint64_t *);
    int (*psci_off)(void *);
};
/* Probe is read-only. Execute may shut down hardware; caller must quiesce
 * touch/render workers first. Firmware returning is always an error. */
int kshim_power_probe(struct kshim_power_config *, const struct kshim_power_ops *);
int kshim_power_execute(struct kshim_power_config *, const struct kshim_power_ops *, kshim_power_action_t);
struct kshim_resources;
int kshim_dt_power(const void *, struct kshim_power_config *);
uint32_t kshim_platform_power_init(const void *);
int kshim_platform_power_execute(kshim_power_action_t);
int kshim_power_map_resources(struct kshim_resources *);
