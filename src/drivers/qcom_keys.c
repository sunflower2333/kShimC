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
struct spmi_port { SpmiDeviceContext *spmi; int fault; uintptr_t command, write_data; };
static struct spmi_port ports[SPMI_PORTS];
static unsigned port_count;
void kshim_qcom_spmi_bind(SpmiDeviceContext **contexts, unsigned count)
{
    memset(ports, 0, sizeof(ports));
    port_count = count <= SPMI_PORTS ? count : 0;
    for (unsigned i = 0; i < port_count; ++i) ports[i].spmi = contexts[i];
}

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

static void KshimSpmiWrite(void *Context, UINTN Address, UINT32 Value)
{
  struct spmi_port *port = Context;
  /* Key reads grant only CMD. Power writes temporarily grant exactly one
   * WDATA word as well; IRQ ownership/configuration remains inaccessible. */
  if (!KshimSpmiAddressValid(port, Address) || (Address != port->command && (!port->write_data || Address != port->write_data))) {
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
static int spmi_read(struct spmi_port *port, uint8_t Sid, uint32_t Address, uint32_t *Value)
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
    if(KshimSpmiInit(p))return -1;
    p->fault=0;
    uint32_t offset=p->spmi->PmicArb.Ops->GetChannelOffset(p->spmi,sid,address,SPMI_ARB_CHANNEL_RW);
    SpmiMemoryRegion *r=&p->spmi->Regions[p->spmi->Version==SPMI_ARB_VERSION_1?
        SPMI_MEMORY_REGION_TYPE_CORE:SPMI_MEMORY_REGION_TYPE_CH_SLAVES];
    if(offset==UINT32_MAX||p->fault||r->BaseAddress>UINTPTR_MAX-offset||
       !KshimSpmiContains(r,r->BaseAddress+offset,SPMI_PMIC_ARB_WDATA0+sizeof(uint32_t)))return -1;
    p->command=r->BaseAddress+offset;p->write_data=p->command+SPMI_PMIC_ARB_WDATA0;
    CR_STATUS status=SpmiWrite(p->spmi,sid,address,&value,1);
    p->command=p->write_data=0;
    return CR_ERROR(status)||p->fault?-1:0;
}

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
