/* SPDX-License-Identifier: MIT */
#include <oskal/cr_interrupt.h>
#include <crdk_os.h>
#include "../../kernel/include/error_no.h"
#include <stdarg.h>
#include <stdio.h>
#if !defined(__aarch64__)
#include <time.h>
#endif

void CrOsBarrier(void)
{
#if defined(__aarch64__)
  __asm__ volatile("dsb sy" ::: "memory");
#else
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
uint32_t CrMmioRead32(uintptr_t address)
{ CrOsBarrier(); uint32_t v = *(volatile uint32_t *)address; CrOsBarrier(); return v; }
uint32_t CrMmioWrite32(uintptr_t address, uint32_t value)
{ CrOsBarrier(); *(volatile uint32_t *)address = value; CrOsBarrier(); return value; }
uint64_t CrOsTime(void)
{
#if defined(__aarch64__)
  uint64_t ticks, frequency;
  __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(ticks));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
  if (!frequency) CrOsPanic("Missing timer frequency");
  return ticks / frequency * 1000000 + (ticks % frequency) * 1000000 / frequency;
#else
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
#endif
}
void CrOsDelay(uint64_t us)
{
  uint64_t start = CrOsTime();
  while (CrOsTime() - start < us) {
#if defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#endif
  }
}
void CrOsLog(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
}
_Noreturn void CrOsPanic(const char *condition)
{
  CrOsLog("CrDK assertion: %s\n", condition);
  for (;;) {
#if defined(__aarch64__)
    __asm__ volatile("wfe");
#else
    abort();
#endif
  }
}

/*
 * Interrupts go through the kShimC interrupt core (kernel/include/kshim/irq.h).
 * Its entry points are weak references so host builds of this port, and a
 * runtime without CONFIG_KSHIM_IRQ, report CR_UNSUPPORTED and stay polled.
 *
 * As in CrDK's UEFI OskInterruptLib, InterruptNumber is the GIC INTID (SPI n is
 * 32 + n) and the trigger follows ArmGicV3Dxe: level-high and rising edge
 * (EDGE_BOTH is programmed as rising, as OskInterruptLib translates it);
 * inverted polarities belong to a secondary controller and are unsupported.
 */
typedef void (*KshimIrqHandler)(uint32_t Intid, void *Context);
extern int kshim_irq_request(uint32_t Intid, unsigned Trigger, KshimIrqHandler Handler,
                             void *Context) __attribute__((weak));
extern int kshim_irq_free(uint32_t Intid) __attribute__((weak));
extern unsigned long kshim_irq_save(void) __attribute__((weak));
extern void kshim_irq_restore(unsigned long Flags) __attribute__((weak));
/* enum kshim_irq_trigger: the devicetree flag values */
#define KSHIM_TRIGGER_EDGE_RISING 1U
#define KSHIM_TRIGGER_LEVEL_HIGH 4U

typedef struct {
  CR_INTERRUPT_CONFIG *Config;
  CR_INTERRUPT_HANDLER Handler;
  VOID *Param;
  UINT32 Source;
} CR_PORT_INTERRUPT;

static CR_PORT_INTERRUPT mCrInterrupts[MAX_INTERRUPT_ENTRIES];
static volatile UINT32 mCrDispatchDepth;

static CR_PORT_INTERRUPT *CrPortFind(UINT32 Source)
{
  for (UINTN Index = 0; Index < MAX_INTERRUPT_ENTRIES; Index++)
    if (mCrInterrupts[Index].Config != NULL && mCrInterrupts[Index].Source == Source)
      return &mCrInterrupts[Index];
  return NULL;
}

/* IRQ context on the boot CPU; the core completes (EOIs) afterwards. */
static void CrPortDispatch(uint32_t Intid, void *Context)
{
  CR_PORT_INTERRUPT *Entry = Context;
  (void)Intid;
  ++mCrDispatchDepth;
  if (Entry->Handler != NULL) Entry->Handler(Entry->Param);
  --mCrDispatchDepth;
}

static CR_STATUS CrPortStatus(int Status)
{
  switch (Status) {
  case 0: return CR_SUCCESS;
  case ERROR_INVALID_PARAMETER: return CR_INVALID_PARAMETER;
  case ERROR_NOT_SUPPORTED: return CR_UNSUPPORTED;
  case ERROR_BUSY: return CR_ALREADY_STARTED; /* another owner, as EFI_ALREADY_STARTED */
  case ERROR_NOT_READY: return CR_NOT_READY;
  default: return CR_DEVICE_ERROR;
  }
}

static int CrPortAvailable(void)
{
  return kshim_irq_request != NULL && kshim_irq_free != NULL &&
         kshim_irq_save != NULL && kshim_irq_restore != NULL;
}

CR_STATUS CrRegisterInterrupt(CR_INTERRUPT_CONFIG *Config)
{
  unsigned Trigger;
  CR_PORT_INTERRUPT *Entry = NULL;
  CR_STATUS Status;
  unsigned long Flags;

  if (Config == NULL) return CR_INVALID_PARAMETER;
  if (Config->Handler == NULL) return CrUnregisterInterrupt(Config);
  switch (Config->TriggerType) {
  case CR_INTERRUPT_TRIGGER_LEVEL_HIGH: Trigger = KSHIM_TRIGGER_LEVEL_HIGH; break;
  case CR_INTERRUPT_TRIGGER_EDGE_RISING:
  case CR_INTERRUPT_TRIGGER_EDGE_BOTH: Trigger = KSHIM_TRIGGER_EDGE_RISING; break;
  case CR_INTERRUPT_TRIGGER_LEVEL_LOW:
  case CR_INTERRUPT_TRIGGER_EDGE_FALLING: return CR_UNSUPPORTED;
  default: return CR_INVALID_PARAMETER;
  }
  if (!CrPortAvailable()) return CR_UNSUPPORTED;
  if (mCrDispatchDepth != 0) return CR_BUSY;

  Flags = kshim_irq_save();
  if (CrPortFind(Config->InterruptNumber) != NULL) {
    kshim_irq_restore(Flags);
    return CR_BUSY;
  }
  for (UINTN Index = 0; Index < MAX_INTERRUPT_ENTRIES && Entry == NULL; Index++)
    if (mCrInterrupts[Index].Config == NULL) Entry = &mCrInterrupts[Index];
  if (Entry == NULL) {
    kshim_irq_restore(Flags);
    return CR_OUT_OF_RESOURCES;
  }
  /* Publish the entry before the interrupt can fire. */
  *Entry = (CR_PORT_INTERRUPT){Config, Config->Handler, Config->Param, Config->InterruptNumber};
  Status = CrPortStatus(kshim_irq_request(Config->InterruptNumber, Trigger, CrPortDispatch, Entry));
  if (Status != CR_SUCCESS) *Entry = (CR_PORT_INTERRUPT){0};
  kshim_irq_restore(Flags);
  return Status;
}

CR_STATUS CrUnregisterInterrupt(CR_INTERRUPT_CONFIG *Config)
{
  CR_PORT_INTERRUPT *Entry;
  CR_STATUS Status;
  unsigned long Flags;

  if (Config == NULL) return CR_INVALID_PARAMETER;
  if (!CrPortAvailable()) return CR_NOT_FOUND;
  if (mCrDispatchDepth != 0) return CR_BUSY;
  Flags = kshim_irq_save();
  Entry = CrPortFind(Config->InterruptNumber);
  if (Entry == NULL) Status = CR_NOT_FOUND;
  else if (Entry->Config != Config ||
           (Config->Handler != NULL &&
            (Entry->Handler != Config->Handler || Entry->Param != Config->Param)))
    Status = CR_BUSY;
  else {
    /* Disabled and drained on return: the callback can no longer run. */
    Status = CrPortStatus(kshim_irq_free(Config->InterruptNumber));
    if (Status == CR_SUCCESS) *Entry = (CR_PORT_INTERRUPT){0};
  }
  kshim_irq_restore(Flags);
  return Status;
}

BOOLEAN CrInterruptIsRegistered(CR_INTERRUPT_CONFIG *Config)
{
  CR_PORT_INTERRUPT *Entry;

  if (Config == NULL || Config->Handler == NULL || !CrPortAvailable()) return FALSE;
  Entry = CrPortFind(Config->InterruptNumber);
  return Entry != NULL && Entry->Config == Config && Entry->Handler == Config->Handler &&
         Entry->Param == Config->Param;
}
