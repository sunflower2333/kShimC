/* SPDX-License-Identifier: MIT */
#include <config.h>
#include <power.h>
#include <qcom_dt.h>
#include <qcom_key_platform.h>
#include <psci.h>
#include <timer.h>
static struct kshim_power_config power;
static unsigned convention;
static int read8(void *unused,const struct kshim_power_pmic *p,uint16_t address,uint8_t *value)
{
    (void)unused;
#if CONFIG_QCOM_SPMI
    return kshim_qcom_power_read8(p->controller,p->bus,p->sid,address,value);
#else
    (void)p;(void)address;(void)value;return -1;
#endif
}
static int write8(void *unused,const struct kshim_power_pmic *p,uint16_t address,uint8_t value)
{
    (void)unused;
#if CONFIG_QCOM_SPMI
    return kshim_qcom_power_write8(p->controller,p->bus,p->sid,address,value);
#else
    (void)p;(void)address;(void)value;return -1;
#endif
}
static int write32(void *unused,uintptr_t address,uint32_t value)
{
    (void)unused;
    if(!address||(address&3))return -1;
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)address=value;
    __asm__ volatile("dsb sy" ::: "memory");return 0;
}
static void wait_us(void *unused,uint32_t us){(void)unused;(void)delay(us);}
/* Qualcomm SMCCC fast-call convention. Preserve the A6 continuation cookie
 * across interrupted calls and bound retries; never guess a legacy SCM ABI. */
static int raw_scm(uint32_t id,unsigned argc,uint64_t a,uint64_t b,uint64_t *result)
{
    uint64_t call=id,cookie=0;
    for(unsigned i=0;i<32;i++) {
        register uint64_t x0 __asm__("x0")=call;
        register uint64_t x1 __asm__("x1")=argc;
        register uint64_t x2 __asm__("x2")=a;
        register uint64_t x3 __asm__("x3")=b;
        register uint64_t x4 __asm__("x4")=0;
        register uint64_t x5 __asm__("x5")=0;
        register uint64_t x6 __asm__("x6")=cookie;
        register uint64_t x7 __asm__("x7")=0;
        __asm__ volatile("smc #0" : "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),"+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7) :: "x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","cc","memory");
        if(x0==1){call=1;cookie=x6;continue;}
        if(result)*result=x1;
        return x0? -1:0;
    }
    return -1;
}
static int scm_call(void *unused,unsigned svc,unsigned cmd,unsigned argc,uint64_t a,uint64_t b,uint64_t *result)
{
    (void)unused;if(!convention)return -1;
    if(convention==0x82000000U&&(a>UINT32_MAX||b>UINT32_MAX))return -1;
    return raw_scm(convention|(svc<<8)|cmd,argc,a,b,result);
}
static int available(unsigned svc,unsigned cmd)
{
    uint64_t value=0;
    return !scm_call(NULL,6,1,1,0x02000000U|(svc<<8)|cmd,0,&value)&&value==1;
}
static int system_off(void *unused){(void)unused;return (int)kshim_psci_call(0x84000008U,0,0,0);}
static const struct kshim_power_ops operations={NULL,read8,write8,write32,wait_us,scm_call,system_off};
int kshim_power_map_resources(struct kshim_resources *r)
{
    struct kshim_power_config c;
    if(kshim_dt_power(r->fdt,&c))return 0; /* unsupported is optional */
    const uintptr_t addresses[]={c.pshold,c.edl,c.dload,c.reason};
    const unsigned lengths[]={4,12,8,4};
    for(unsigned i=0;i<4;i++)if(addresses[i]&&kshim_resource_map(r,addresses[i],lengths[i],
        KSHIM_MMU_DEVICE,KSHIM_MMU_READ|KSHIM_MMU_WRITE))return -1;
    return 0;
}
uint32_t kshim_platform_power_init(const void *fdt)
{
    convention=0;
    (void)kshim_dt_power(fdt,&power);
    if(kshim_psci_conduit()==KSHIM_PSCI_CONDUIT_NONE)(void)kshim_psci_init(fdt);
    power.psci_off=kshim_psci_conduit()!=KSHIM_PSCI_CONDUIT_NONE;
    /* Probe SCM only on a DT-described Qualcomm PS_HOLD system. */
    if(power.pshold) {
        uint64_t result=0;
        const uint32_t abis[]={0xc2000000U,0x82000000U};
        for(unsigned i=0;i<2&&!convention;i++)
            if(!raw_scm(abis[i]|0x601U,1,0x02000601U,0,&result)&&result==1)convention=abis[i];
        if(convention){power.scm_io=available(5,2)&&(convention==0xc2000000U||power.tcsr<=UINT32_MAX);
            power.scm_dload=available(1,0x10);power.scm_sdi=available(1,9);
            power.scm_arbiter=available(9,1);power.scm_pshold=available(9,2);}
    }
    (void)kshim_power_probe(&power,&operations);return power.capabilities;
}
int kshim_platform_power_execute(kshim_power_action_t action)
{ return kshim_power_execute(&power,&operations,action); }
