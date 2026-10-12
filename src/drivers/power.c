/* SPDX-License-Identifier: MIT
 * Register facts: Linux pm8941-pwrkey and Qualcomm msm-poweroff. See docs/power.md.
 * Independent, callback-based sequencing; no kernel implementation is copied.
 */
#include <power.h>
struct saved_pmic { uint8_t ctl, enable, watchdog, reason; uint16_t enreg; };
static int read_reg(const struct kshim_power_ops *o,const struct kshim_power_pmic *p,
                    unsigned reg,uint8_t *value)
{ return o->read8(o->context,p,(uint16_t)(p->base+reg),value); }
static int write_reg(const struct kshim_power_ops *o,const struct kshim_power_pmic *p,
                     unsigned reg,uint8_t value)
{ return o->write8(o->context,p,(uint16_t)(p->base+reg),value); }
static int scm(const struct kshim_power_ops *o,unsigned svc,unsigned cmd,
                unsigned argc,uint64_t a,uint64_t b)
{
    uint64_t result=0;
    if(!o->scm)return -1;
    int status=o->scm(o->context,svc,cmd,argc,a,b,&result);
    /* SDI has an A1 service status in addition to the A0 transport status.
     * Download-mode and secure-I/O calls use only the transport status. */
    return status?status:(svc==1&&cmd==9&&result!=0?-1:0);
}
/* Downstream boot SCM and the TCSR secure-I/O fallback share mode values.
 * Mainline boot SCM instead uses (download selector, enabled value). */
static int download_mode(const struct kshim_power_config *c,const struct kshim_power_ops *o,int edl)
{
    if(c->scm_dload)return scm(o,1,0x10,2,c->downstream?(edl?1:0):0x10,0);
    if(c->downstream&&c->tcsr)return c->scm_io?scm(o,5,2,2,c->tcsr,edl?1:0):-1;
    return 0; /* Optional IMEM-only route, or no download facility described. */
}
int kshim_power_probe(struct kshim_power_config *c,const struct kshim_power_ops *o)
{
    if(!c||!o)return KSHIM_POWER_IO;
    c->pon_ready=0;c->capabilities=c->psci_off&&o->psci_off?KSHIM_POWER_BIT(KSHIM_POWER_OFF):0;
    if(c->unsupported_pon||!c->count||c->count>KSHIM_POWER_MAX_PMICS||!o->read8||!o->write8||
        !o->write32||!o->delay_us||(!c->pshold&&!(c->scm_pshold&&o->scm)))return 0;
    for(unsigned i=0;i<c->count;i++) {
        struct kshim_power_pmic *p=&c->pmic[i];
        if(p->base>0xff00||(p->base&255)||p->sid>15||p->controller>3||p->bus>3)return 0;
        for(unsigned j=0;j<i;j++)if(p->controller==c->pmic[j].controller&&p->sid==c->pmic[j].sid&&p->base==c->pmic[j].base)return 0;
        if(read_reg(o,p,1,&p->revision)||read_reg(o,p,5,&p->subtype))return 0;
        /* Gen3 HLOS/PBS has no legacy PS_HOLD reset-control contract. */
        if(p->subtype<1||p->subtype>5)return 0;
    }
    /* A declared downstream secure cookie must be clearable before reset. */
    if(c->downstream&&c->tcsr&&!(o->scm&&(c->scm_dload||c->scm_io)))return 0;
    c->pon_ready=1;c->capabilities|=KSHIM_POWER_BIT(KSHIM_POWER_OFF)|KSHIM_POWER_BIT(KSHIM_POWER_WARM)|KSHIM_POWER_BIT(KSHIM_POWER_HARD);
    if(c->downstream && (c->edl||(o->scm&&(c->scm_dload||(c->tcsr&&c->scm_io)))))c->capabilities|=KSHIM_POWER_BIT(KSHIM_POWER_EDL);
    if(c->force_warm)c->capabilities&=~KSHIM_POWER_BIT(KSHIM_POWER_HARD);
    c->capabilities&=~c->blocked;
    if(!(c->capabilities&KSHIM_POWER_BIT(KSHIM_POWER_WARM)))c->capabilities&=~KSHIM_POWER_BIT(KSHIM_POWER_EDL);
    return 0;
}
static int restore(struct kshim_power_config *c,const struct kshim_power_ops *o,
                   const struct saved_pmic *s,kshim_power_action_t action)
{
    int bad=0;
    for(unsigned i=0;i<c->count;i++) {
        const struct kshim_power_pmic *p=&c->pmic[i];
        bad|=write_reg(o,p,s[i].enreg,s[i].enable&0x7f)!=0;
        o->delay_us(o->context,500);
        bad|=write_reg(o,p,0x5a,s[i].ctl&((s[i].enreg==0x5a)?0x7f:0xff))!=0;
        if(action==KSHIM_POWER_EDL&&i==0)bad|=write_reg(o,p,0x57,s[i].watchdog)!=0;
        if(i==0&&p->store_reason)bad|=write_reg(o,p,0x8f,s[i].reason)!=0;
        bad|=write_reg(o,p,s[i].enreg,s[i].enable)!=0;
    }
    return bad?KSHIM_POWER_UNSAFE:KSHIM_POWER_IO;
}
int kshim_power_execute(struct kshim_power_config *c,const struct kshim_power_ops *o,kshim_power_action_t action)
{
    if(!c||!o||action<KSHIM_POWER_OFF||action>KSHIM_POWER_EDL||
        !(c->capabilities&KSHIM_POWER_BIT(action)))return KSHIM_POWER_UNSUPPORTED;
    if(!c->pon_ready) {
        if(action!=KSHIM_POWER_OFF||!c->psci_off||!o->psci_off)return KSHIM_POWER_UNSUPPORTED;
        (void)o->psci_off(o->context);return KSHIM_POWER_RETURNED;
    }
    if(!c->count||c->count>KSHIM_POWER_MAX_PMICS||!o->read8||!o->write8||
       !o->write32||!o->delay_us)return KSHIM_POWER_IO;
    struct saved_pmic saved[KSHIM_POWER_MAX_PMICS]={0};
    unsigned type=action==KSHIM_POWER_OFF?4:action==KSHIM_POWER_HARD?7:1;
    /* Snapshot every PMIC before the first mutation. A read failure has no side effects. */
    for(unsigned i=0;i<c->count;i++) {
        struct kshim_power_pmic *p=&c->pmic[i];struct saved_pmic *s=&saved[i];
        s->enreg=p->subtype<=2&&!p->revision?0x5a:0x5b;
        if(read_reg(o,p,0x5a,&s->ctl)||read_reg(o,p,s->enreg,&s->enable)||
           (action==KSHIM_POWER_EDL&&i==0&&read_reg(o,p,0x57,&s->watchdog))||
           (i==0&&p->store_reason&&read_reg(o,p,0x8f,&s->reason)))return KSHIM_POWER_IO;
    }
    for(unsigned i=0;i<c->count;i++) {
        const struct kshim_power_pmic *p=&c->pmic[i];const struct saved_pmic *s=&saved[i];
        if(write_reg(o,p,s->enreg,s->enable&0x7f))goto rollback;
        o->delay_us(o->context,500); /* Conservative downstream requirement: ten sleep cycles plus tolerance. */
        uint8_t ctl=(s->ctl&0xf0)|(uint8_t)type;
        if(s->enreg==0x5a)ctl&=0x7f;
        if(write_reg(o,p,0x5a,ctl))goto rollback;
        if(action==KSHIM_POWER_EDL&&i==0&&write_reg(o,p,0x57,s->watchdog&0x7f))goto rollback;
        /* Clear stale PMIC restart reason without disturbing unrelated bits. */
        if(i==0&&p->store_reason&&write_reg(o,p,0x8f,s->reason&
            (p->subtype>=4&&!p->legacy_reason?0x01:0x03)))goto rollback;
        if(write_reg(o,p,s->enreg,s->enreg==0x5a?(ctl|0x80):(s->enable|0x80)))goto rollback;
    }
    /* Stale dump/EDL cookies must not survive a normal restart. */
    if(c->dload&&(o->write32(o->context,c->dload,0)||o->write32(o->context,c->dload+4,0)))goto rollback;
    if(c->reason&&o->write32(o->context,c->reason,0x77665501U))goto rollback;
    if(c->edl) {
        const uint32_t magic[]={0x322a4f99U,0xc67e4350U,0x77777777U};
        for(unsigned i=0;i<3;i++)if(o->write32(o->context,c->edl+4*i,action==KSHIM_POWER_EDL?magic[i]:0))goto clear_edl;
    }
    if(download_mode(c,o,action==KSHIM_POWER_EDL))goto clear_edl;
    if(c->scm_sdi&&scm(o,1,9,2,1,0))goto clear_edl;
    /* No SPMI use or UI recovery after this point: arbiter may be stopped. */
    c->capabilities=0;
    if(c->scm_arbiter&&scm(o,9,1,1,0,0))return KSHIM_POWER_UNSAFE;
    if(c->scm_pshold)(void)scm(o,9,2,1,0,0);
    if(c->pshold)(void)o->write32(o->context,c->pshold,0);
    o->delay_us(o->context,10000000U);
    return KSHIM_POWER_RETURNED;
clear_edl:
    {
        int bad=0;
        if(c->edl)for(unsigned i=0;i<3;i++)bad|=o->write32(o->context,c->edl+4*i,0)!=0;
        bad|=download_mode(c,o,0)!=0;
        c->capabilities=0;
        int restored=restore(c,o,saved,action);
        return bad?KSHIM_POWER_UNSAFE:restored;
    }
rollback:
    c->capabilities=0;return restore(c,o,saved,action);
}
