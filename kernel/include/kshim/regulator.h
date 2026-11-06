/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Regulators resolved from DT "<name>-supply" phandles (CONFIG_KSHIM_REGULATOR).
 * Owner: sm8650 (kernel/drivers/regulator/). See docs/io-stack.md.
 *
 * Providers:
 *  - "regulator-fixed" with "gpio"/"gpios" (+ "enable-active-high",
 *    "startup-delay-us", "off-on-delay-us", "vin-supply", pinctrl-0). Without
 *    a GPIO it is an always-on rail (vph_pwr). Enable/disable drive the GPIO
 *    through kshim/gpio.h; the inherited level is the restore target.
 *  - RPMh VRM/XOB through CrDK CmdDBLib + RpmhLib polling API:
 *    Android "qcom,rpmh-vrm-regulator"/"qcom,rpmh-xob-regulator" parents with
 *    "qcom,resource-name" (for example "ldob14"), and mainline
 *    "qcom,pmXXXX-rpmh-regulators" with "qcom,pmic-id" and child node names
 *    ldoN/smpsN/bobN (resource "ldo" + pmic-id + N). The RSC is the apps DRV
 *    ("qcom,rpmh-rsc" / Android "qcom,tcs-drv"), CMD DB the "qcom,cmd-db"
 *    reserved memory. Votes are voltage (mV, clamped to the node's
 *    regulator-min/max-microvolt), enable, and mode only when the DT names
 *    one (regulator-initial-mode) or a consumer's load hint needs one
 *    (kshim_regulator_set_load()).
 *  - Anything else: ERROR_NOT_SUPPORTED from kshim_regulator_get().
 *
 * Reference counting: one object per provider node, shared by all
 * consumers. The first enable enables the vin-supply parent, programs the
 * rail only if it is observed off (a fixed GPIO already at its active level
 * is inherited-on and not written), then waits the startup delay. The last
 * disable returns the rail to its inherited state: an inherited-on rail
 * stays on.
 *
 * Handoff (kshim_regulator_restore_all): fixed rails get their inherited GPIO
 * level back (newest first). RPMh: the apps-DRV vote that kShimC raised is
 * restored to the inherited vote when CrDK's own-DRV observation
 * (RpmhRailObserveLib) captured one; otherwise it is kept, which is CrDK's
 * documented policy (an inherited vote cannot be read back reliably and
 * dropping it could cut a rail the previous stage relied on), and logged.
 */
#include <stdbool.h>
#include <stdint.h>

#include "error_no.h"

struct kshim_regulator;

int kshim_regulator_init(const void *fdt);
struct kshim_resources;
/* Map the RSC DRV window, CMD DB (normal NC, read-only) and fixed-regulator
 * GPIO pages for every regulator referenced by an enabled consumer. */
int kshim_regulator_map_resources(struct kshim_resources *resources);

/* Resolve "<supply>-supply" of `consumer` ("avdd" -> "avdd-supply";
 * supply names are matched case-sensitively, so GT911's "AVDD28" works).
 * ERROR_NOT_FOUND when the property is absent (optional supply). No MMIO. */
int kshim_regulator_get(const void *fdt, int consumer, const char *supply,
                        struct kshim_regulator **regulator);
/* The provider node itself (for a "regulator-fixed" referenced elsewhere). */
int kshim_regulator_get_node(const void *fdt, int provider, struct kshim_regulator **regulator);

/* Thread context. 0 when the rail is on (and settled). */
int kshim_regulator_enable(struct kshim_regulator *regulator);
int kshim_regulator_disable(struct kshim_regulator *regulator);
/* RPMh only (fixed rails accept a range containing their voltage). Takes
 * effect immediately when enabled, else at the next enable. */
int kshim_regulator_set_voltage(struct kshim_regulator *regulator,
                                uint32_t min_uv, uint32_t max_uv);
/* Load hint in microamps, as Linux regulator_set_load(): the rail's
 * requirement is the largest load any consumer asked for (a lower request
 * never lowers it while it is held). RPMh LDO/SMPS/BOB rails turn it into a
 * PMIC5 hardware mode:
 *  - Android VRM nodes: qcom,regulator-type "pmic5-*" with
 *    qcom,supported-modes and qcom,mode-threshold-currents: the highest
 *    listed mode whose threshold <= the load (kona ldoa13: LPM below
 *    10000 uA, HPM from 10000 uA);
 *  - mainline PMIC5 LDOs with regulator-allow-set-load: HPM (7) from
 *    10000 uA (qcom-rpmh-regulator hpm_min_load_uA), else the initial mode.
 * The mode is voted with the voltage and enable votes (at the next enable,
 * or at once on an enabled rail) and goes back to the observed request at
 * release/handoff like them. A load of 0, or a rail without these facts,
 * votes no mode. Fixed rails and XOBs ignore it (0). */
int kshim_regulator_set_load(struct kshim_regulator *regulator, uint32_t load_ua);
/* The PMIC5 hardware mode the next vote uses, or -1 for none. */
int kshim_regulator_mode(const struct kshim_regulator *regulator);
bool kshim_regulator_is_enabled(const struct kshim_regulator *regulator);
/* "regulator-name", or the node name. Never NULL. */
const char *kshim_regulator_name(const struct kshim_regulator *regulator);

/* Enable/disable every "*-supply" of `node` in property order (used by the
 * touch framework and PCI endpoint power). Partially enabled supplies are
 * disabled again on failure. */
int kshim_regulator_bulk_enable(const void *fdt, int node);
int kshim_regulator_bulk_disable(const void *fdt, int node);

/* Handoff (kshim_io_quiesce): see the policy above. Idempotent. */
int kshim_regulator_restore_all(void);
