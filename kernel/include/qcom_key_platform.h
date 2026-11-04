/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

/* Board code may override this weak hook with its SPMI transport. */
int kshim_qcom_spmi_read32(void *Context, uint8_t Sid, uint32_t Address,
                           uint32_t *Value);

/* One-microsecond PMIC Arbiter polling delay; board code may override it. */
void kshim_qcom_spmi_delay_us(uint32_t Microseconds);

/* Decode an SPMI token or return the CrDK TLMM input bit at an IO address. */
uint32_t kshim_qcom_key_read32(void *Context, uint64_t Address);

/* Status-preserving form used by QcomKeysInitEx(). */
int kshim_qcom_key_read32_status(
    void *Context, uint64_t Address, uint32_t *Value);

#include <Library/spmi.h>
void kshim_qcom_spmi_bind(SpmiDeviceContext **contexts, unsigned count);

/* SPMI interrupt cascade (CONFIG_QCOM_IRQCHIP, kernel/drivers/irqchip/
 * spmi_pmic.c): the initialized controller context, and reference-counted
 * write grants for exactly the channel and PIC words of one claimed
 * peripheral. Without a grant SpmiLib cannot touch a peripheral's IRQ
 * configuration through this transport. */
SpmiDeviceContext *kshim_qcom_spmi_context(unsigned controller);
int kshim_qcom_spmi_grant(unsigned controller, uint8_t sid, uint8_t peripheral);
void kshim_qcom_spmi_revoke(unsigned controller, uint8_t sid, uint8_t peripheral);

/* Restricted PON adapter used only by the power sequencer. */
int kshim_qcom_power_read8(uint8_t controller,uint8_t bus,uint8_t sid,uint16_t address,uint8_t *value);
int kshim_qcom_power_write8(uint8_t controller,uint8_t bus,uint8_t sid,uint16_t address,uint8_t value);

/* PMIC GPIO byte transport (kernel/drivers/gpio, CONFIG_QCOM_SPMI): reads
 * any byte; writes only GPIO configuration registers 0x40-0x46/0x4a of a
 * peripheral whose TYPE reads as GPIO. 0 or -1. */
int kshim_qcom_pmic_gpio_read8(uint8_t controller, uint8_t sid, uint16_t address, uint8_t *value);
int kshim_qcom_pmic_gpio_write8(uint8_t controller, uint8_t sid, uint16_t address, uint8_t value);
