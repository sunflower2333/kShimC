/* SPDX-License-Identifier: MIT */
#if !defined(KSHIM_HOST_TEST)
#include <config.h>
#endif
#include <qcom_key_platform.h>
#include <qcom_keys.h>
#include <stdint.h>
#include <string.h>
#include <Library/spmi.h>
#include <Library/pmic_gpio.h>
#include <Library/gpio.h>

#if defined(__aarch64__)
#include <timer.h>
#endif
#if CONFIG_QCOM_IRQCHIP
#include <kshim/irq.h>
#endif
__attribute__((weak)) void kshim_qcom_spmi_delay_us(uint32_t Microseconds)
{
#if defined(__aarch64__)
  (void)delay(Microseconds);
#else
  (void)Microseconds;
#endif
}

#if CONFIG_QCOM_SPMI
#define SPMI_PORTS 4
/* Interrupt cascade grants: the RW channel CMD/WDATA0/WDATA1, the observer
 * CMD and the PIC ACC_ENABLE/IRQ_CLEAR words of one claimed peripheral. */
#define SPMI_GRANT_WORDS 6U
#define SPMI_GRANTS 8U
struct spmi_grant { uint8_t sid, peripheral, references; uintptr_t words[SPMI_GRANT_WORDS]; };
struct spmi_port {
  SpmiDeviceContext *spmi; int fault; uintptr_t command, write_data;
#if CONFIG_QCOM_IRQCHIP
  struct spmi_grant grants[SPMI_GRANTS];
#endif
};
static struct spmi_port ports[SPMI_PORTS];
static unsigned port_count;
void kshim_qcom_spmi_bind(SpmiDeviceContext **contexts, unsigned count)
{
    memset(ports, 0, sizeof(ports));
    port_count = count <= SPMI_PORTS ? count : 0;
    for (unsigned i = 0; i < port_count; ++i) ports[i].spmi = contexts[i];
}

/* Arbiter transactions are not reentrant (CR_LOCK is a plain spinlock):
 * once the interrupt cascade dispatches from IRQ context, every
 * thread-context transaction runs with IRQs masked on this CPU. */
#if CONFIG_QCOM_IRQCHIP
#define SPMI_LOCK() unsigned long spmi_irq_flags = kshim_irq_save()
#define SPMI_UNLOCK() kshim_irq_restore(spmi_irq_flags)
#else
#define SPMI_LOCK() do { } while (0)
#define SPMI_UNLOCK() do { } while (0)
#endif

static int KshimSpmiContains(const SpmiMemoryRegion *Region,
                             uintptr_t Address, size_t Bytes)
{
  return Region->BaseAddress != 0 && Region->Size >= Bytes &&
         Region->BaseAddress <= UINTPTR_MAX - (Region->Size - 1U) &&
         Address >= Region->BaseAddress &&
         Address - Region->BaseAddress <= Region->Size - Bytes &&
         (Address & 3U) == 0;
}

static int KshimSpmiAddressValid(struct spmi_port *port, uintptr_t Address)
{
  for (unsigned Index = 0; Index < SPMI_MEMORY_REGION_TYPE_MAX; Index++)
    if (KshimSpmiContains(&port->spmi->Regions[Index], Address, sizeof(uint32_t)))
      return 1;
  port->fault = 1;
  return 0;
}

static void KshimSpmiBarrier(void)
{
#if defined(__aarch64__)
  __asm__ volatile("dsb sy" ::: "memory");
#else
  __asm__ volatile("" ::: "memory");
#endif
}

static UINT32 KshimSpmiRead(void *Context, UINTN Address)
{
  struct spmi_port *port = Context;
  if (!KshimSpmiAddressValid(port, Address))
    return 0;
  uint32_t Value = *(volatile uint32_t *)Address;
  KshimSpmiBarrier();
  return Value;
}

static int KshimSpmiGranted(struct spmi_port *port, UINTN Address)
{
#if CONFIG_QCOM_IRQCHIP
  for (unsigned Grant = 0; Grant < SPMI_GRANTS; Grant++)
    for (unsigned Word = 0; port->grants[Grant].references && Word < SPMI_GRANT_WORDS; Word++)
      if (port->grants[Grant].words[Word] == Address)
        return 1;
#else
  (void)port; (void)Address;
#endif
  return 0;
}

static void KshimSpmiWrite(void *Context, UINTN Address, UINT32 Value)
{
  struct spmi_port *port = Context;
  /* Key reads grant only CMD. Power writes temporarily grant exactly one
   * WDATA word as well. IRQ configuration is reachable only for the
   * peripherals the interrupt cascade claimed (kshim_qcom_spmi_grant). */
  if (!KshimSpmiAddressValid(port, Address) ||
      (Address != port->command && (!port->write_data || Address != port->write_data) &&
       !KshimSpmiGranted(port, Address))) {
    port->fault = 1;
    return;
  }
  KshimSpmiBarrier();
  *(volatile uint32_t *)Address = Value;
  KshimSpmiBarrier();
}

static void KshimSpmiStall(void *Context, UINT32 Microseconds)
{
  (void)Context;
  kshim_qcom_spmi_delay_us(Microseconds);
}

static int KshimSpmiInit(struct spmi_port *port)
{
  SpmiDeviceContext *ctx = port->spmi;
  if (!ctx) return -1;
  if (ctx->Initialized) return 0;
  port->fault = 0;
  ctx->Io = (SpmiIoOps){KshimSpmiRead, KshimSpmiWrite, KshimSpmiStall, port};
  if (CR_ERROR(SpmiLibInit(&ctx)) || port->fault) {
    ctx->Initialized = FALSE;
    return -1;
  }
  return 0;
}

/* CrDK supplies Linux-compatible PMIC Arbiter v1/v2/v3/v5/v7/v8/v8.5
 * transactions. The compatibility API returns the single PMIC status byte. */
static int spmi_read_locked(struct spmi_port *port, uint8_t Sid, uint32_t Address, uint32_t *Value)
{
  uint8_t Byte;
  uint32_t Offset;
  SpmiMemoryRegion *ReadRegion;
  if (Value == NULL || Sid > 15U || Address > UINT16_MAX ||
      KshimSpmiInit(port) != 0)
    return -1;
  port->fault = 0;
  Offset = port->spmi->PmicArb.Ops->GetChannelOffset(
      port->spmi, Sid, (uint16_t)Address, SPMI_ARB_CHANNEL_OBS);
  ReadRegion = &port->spmi->Regions[port->spmi->Version == SPMI_ARB_VERSION_1
                                ? SPMI_MEMORY_REGION_TYPE_CORE
                                : SPMI_MEMORY_REGION_TYPE_OBSERVER];
  if (Offset == UINT32_MAX || port->fault ||
      ReadRegion->BaseAddress > UINTPTR_MAX - Offset ||
      !KshimSpmiContains(ReadRegion, ReadRegion->BaseAddress + Offset,
                        SPMI_PMIC_ARB_RDATA0 + sizeof(uint32_t)))
    return -1;
  port->command = ReadRegion->BaseAddress + Offset;
  CR_STATUS Status = SpmiRead(port->spmi, Sid, (uint16_t)Address, &Byte, 1);
  port->command = 0;
  if (CR_ERROR(Status) || port->fault)
    return -1;
  *Value = Byte;
  return 0;
}

static int spmi_read(struct spmi_port *port, uint8_t Sid, uint32_t Address, uint32_t *Value)
{
  SPMI_LOCK();
  int Status = spmi_read_locked(port, Sid, Address, Value);
  SPMI_UNLOCK();
  return Status;
}

/* Dedicated one-byte PON transaction adapter, not a general PMIC write API. */
int kshim_qcom_power_read8(uint8_t controller,uint8_t bus,uint8_t sid,uint16_t address,uint8_t *value)
{
    uint32_t v;
    if(!value||controller>=port_count||!ports[controller].spmi||ports[controller].spmi->BusId!=bus||
       spmi_read(&ports[controller],sid,address,&v))return -1;
    *value=(uint8_t)v;return 0;
}
int kshim_qcom_power_write8(uint8_t controller,uint8_t bus,uint8_t sid,uint16_t address,uint8_t value)
{
    unsigned reg=address&255U;
    if(controller>=port_count||!ports[controller].spmi||ports[controller].spmi->BusId!=bus||sid>15||
       (reg!=0x5a&&reg!=0x5b&&reg!=0x57&&reg!=0x8f))return -1;
    struct spmi_port *p=&ports[controller];
    SPMI_LOCK();
    int result=-1;
    if(KshimSpmiInit(p))goto out;
    p->fault=0;
    uint32_t offset=p->spmi->PmicArb.Ops->GetChannelOffset(p->spmi,sid,address,SPMI_ARB_CHANNEL_RW);
    SpmiMemoryRegion *r=&p->spmi->Regions[p->spmi->Version==SPMI_ARB_VERSION_1?
        SPMI_MEMORY_REGION_TYPE_CORE:SPMI_MEMORY_REGION_TYPE_CH_SLAVES];
    if(offset==UINT32_MAX||p->fault||r->BaseAddress>UINTPTR_MAX-offset||
       !KshimSpmiContains(r,r->BaseAddress+offset,SPMI_PMIC_ARB_WDATA0+sizeof(uint32_t)))goto out;
    p->command=r->BaseAddress+offset;p->write_data=p->command+SPMI_PMIC_ARB_WDATA0;
    CR_STATUS status=SpmiWrite(p->spmi,sid,address,&value,1);
    p->command=p->write_data=0;
    result=CR_ERROR(status)||p->fault?-1:0;
out:
    SPMI_UNLOCK();
    return result;
}

#if CONFIG_QCOM_IRQCHIP
SpmiDeviceContext *kshim_qcom_spmi_context(unsigned controller)
{
  if (controller >= port_count || !ports[controller].spmi) return NULL;
  SPMI_LOCK();
  int status = KshimSpmiInit(&ports[controller]);
  SPMI_UNLOCK();
  return status ? NULL : ports[controller].spmi;
}

static int spmi_grant_words(struct spmi_port *port, uint8_t sid, uint8_t peripheral,
                            uintptr_t words[SPMI_GRANT_WORDS])
{
  SpmiDeviceContext *ctx = port->spmi;
  UINT16 apid;
  uint16_t address = (uint16_t)(peripheral << 8);
  if (CR_ERROR(SpmiGetApid(ctx, sid, peripheral, &apid))) return -1;
  uint32_t rw = ctx->PmicArb.Ops->GetChannelOffset(ctx, sid, address, SPMI_ARB_CHANNEL_RW);
  uint32_t obs = ctx->PmicArb.Ops->GetChannelOffset(ctx, sid, address, SPMI_ARB_CHANNEL_OBS);
  if (rw == UINT32_MAX || obs == UINT32_MAX || !ctx->PmicArb.Ops->GetAccEnable ||
      !ctx->PmicArb.Ops->GetIrqClear) return -1;
  uintptr_t write = ctx->PmicArb.WriteAddr + rw, read = ctx->PmicArb.ReadAddr + obs;
  words[0] = write + SPMI_PMIC_ARB_CMD;
  words[1] = write + SPMI_PMIC_ARB_WDATA0;
  words[2] = write + SPMI_PMIC_ARB_WDATA1;
  words[3] = read + SPMI_PMIC_ARB_CMD;
  words[4] = ctx->PmicArb.Ops->GetAccEnable(ctx, apid);
  words[5] = ctx->PmicArb.Ops->GetIrqClear(ctx, apid);
  for (unsigned i = 0; i < SPMI_GRANT_WORDS; ++i)
    if (!KshimSpmiAddressValid(port, words[i])) return -1;
  return 0;
}

int kshim_qcom_spmi_grant(unsigned controller, uint8_t sid, uint8_t peripheral)
{
  if (controller >= port_count || !ports[controller].spmi || sid > 15) return -1;
  struct spmi_port *port = &ports[controller];
  struct spmi_grant *slot = NULL;
  int status = -1;
  SPMI_LOCK();
  if (KshimSpmiInit(port)) goto out;
  for (unsigned i = 0; i < SPMI_GRANTS; ++i) {
    struct spmi_grant *g = &port->grants[i];
    if (g->references && g->sid == sid && g->peripheral == peripheral) {
      if (g->references == UINT8_MAX) goto out;
      ++g->references; status = 0; goto out;
    }
    if (!g->references && !slot) slot = g;
  }
  if (!slot) goto out;
  port->fault = 0;
  if (spmi_grant_words(port, sid, peripheral, slot->words) || port->fault) {
    port->fault = 0;
    goto out;
  }
  slot->sid = sid; slot->peripheral = peripheral; slot->references = 1;
  status = 0;
out:
  SPMI_UNLOCK();
  return status;
}

void kshim_qcom_spmi_revoke(unsigned controller, uint8_t sid, uint8_t peripheral)
{
  if (controller >= port_count) return;
  SPMI_LOCK();
  for (unsigned i = 0; i < SPMI_GRANTS; ++i) {
    struct spmi_grant *g = &ports[controller].grants[i];
    if (g->references && g->sid == sid && g->peripheral == peripheral && !--g->references)
      memset(g, 0, sizeof(*g));
  }
  SPMI_UNLOCK();
}
#endif

static CR_STATUS pmic_read(void *context, UINT8 sid, UINT16 address, UINT8 *buffer, UINTN size)
{
  uint32_t value;
  if (!buffer || size != 1 || spmi_read(context, sid, address, &value)) return CR_DEVICE_ERROR;
  *buffer = value;
  return CR_SUCCESS;
}

__attribute__((weak)) int kshim_qcom_spmi_read32(void *context, uint8_t sid,
                                               uint32_t address, uint32_t *value)
{
  (void)context;
  return port_count ? spmi_read(&ports[0], sid, address, value) : -1;
}

#endif

int kshim_qcom_key_read32_status(
    void *Context, uint64_t Address, uint32_t *Value)
{
  (void)Context;
  if (Value == NULL)
    return -1;
  if (QCOM_KEY_IS_SPMI_TOKEN(Address)) {
#if CONFIG_QCOM_SPMI
    unsigned index = QCOM_KEY_SPMI_TOKEN_CONTROLLER(Address);
    if (index >= port_count || !ports[index].spmi ||
        ports[index].spmi->BusId != QCOM_KEY_SPMI_TOKEN_BUS(Address)) return -1;
    if (Address & QCOM_KEY_PMIC_GPIO_FLAG) {
      uint32_t offset = QCOM_KEY_SPMI_TOKEN_ADDRESS(Address);
      if (offset > UINT16_MAX || (offset & 0xff) != 0x10) return -1;
      PmicGpioBusOps bus = {.Read = pmic_read, .Context = &ports[index]};
      uint8_t value;
      if (CR_ERROR(PmicGpioReadStatus(&bus, QCOM_KEY_SPMI_TOKEN_SID(Address), offset - 0x10, &value))) return -1;
      *Value = value;
      return 0;
    }
    return spmi_read(&ports[index], QCOM_KEY_SPMI_TOKEN_SID(Address),
                     QCOM_KEY_SPMI_TOKEN_ADDRESS(Address), Value);
#else
    return -1;
#endif
  }
  if (Address == 0U)
    return -1;
  if ((Address & 3) || Address < 4) return -1;
  static GpioDeviceContext gpio;
  gpio.PinCount = 1;
  gpio.RegCfg[0] = (GpioConfigRegInfo){.IoRegOffset = 4, .InputMsk = 1};
  gpio.GpioPins[0].PinAddress = Address - 4;
  gpio.GpioPins[0].pRegCfg = &gpio.RegCfg[0];
  *Value = GpioReadInputVal(&gpio, 0);
  return 0;
}

uint32_t kshim_qcom_key_read32(void *Context, uint64_t Address)
{
  uint32_t Value;
  return kshim_qcom_key_read32_status(Context, Address, &Value) == 0
             ? Value : UINT32_MAX;
}
