/* SPDX-License-Identifier: MIT */
#include <power.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct sim { uint8_t r[4][256]; int calls, fail_at, writes, dropped, scm_calls, scm_fail_on;
    uint64_t download_arg, secure_address, secure_value, sdi_result;
    uint32_t waited, type; uintptr_t addr[16];uint32_t val[16];unsigned n; };
static int rd(void *p,const struct kshim_power_pmic *m,uint16_t a,uint8_t *v) {
    struct sim *s=p; if(++s->calls==s->fail_at)return -1;*v=s->r[m->sid][a-m->base];return 0; }
static int wr(void *p,const struct kshim_power_pmic *m,uint16_t a,uint8_t v) {
    struct sim *s=p; if(++s->calls==s->fail_at)return -1;
    if((a&255)==0x5a && (v&15)!=0){if((v&15)!=(s->r[m->sid][a-m->base]&15))assert(s->waited>=500);s->type=v&15;}
    s->r[m->sid][a-m->base]=v;s->writes++;return 0; }
static int mm(void *p,uintptr_t a,uint32_t v){struct sim *s=p;assert(s->n<16);s->addr[s->n]=a;s->val[s->n++]=v;if(a==0x1000)s->dropped++;return 0;}
static void wait(void *p,uint32_t us){((struct sim*)p)->waited+=us;}
static int scm(void *p,unsigned svc,unsigned cmd,unsigned argc,uint64_t a,uint64_t b,uint64_t *res){
    struct sim *s=p;(void)svc;(void)cmd;(void)argc;(void)a;(void)b;(void)res;if(res)*res=svc==1&&cmd==9?s->sdi_result:0;s->scm_calls++;if(svc==5&&cmd==2){s->secure_address=a;s->secure_value=b;}if(svc==1&&cmd==0x10)s->download_arg=a;return s->scm_calls==s->scm_fail_on?-1:0;}
static int off(void *p){((struct sim*)p)->dropped++;return 0;}
static struct kshim_power_ops ops(struct sim *s){return (struct kshim_power_ops){s,rd,wr,mm,wait,scm,off};}
static struct kshim_power_config cfg(struct sim *s){memset(s,0,sizeof *s);s->r[0][1]=1;s->r[0][5]=4;s->r[0][0x5a]=0xa7;s->r[0][0x5b]=0x80;
    return (struct kshim_power_config){.pmic={{.base=0x800,.sid=0,.store_reason=1}},.count=1,.pshold=0x1000,.edl=0x2000,.dload=0x3000,.reason=0x4000,.scm_dload=1,.downstream=1};}
#ifdef POWER_RED
int kshim_power_probe(struct kshim_power_config *c,const struct kshim_power_ops *o){(void)c;(void)o;return -1;}
int kshim_power_execute(struct kshim_power_config *c,const struct kshim_power_ops *o,kshim_power_action_t a){(void)c;(void)o;(void)a;return -1;}
#endif
int main(void){
    for(unsigned a=1;a<=4;a++){struct sim s;struct kshim_power_config c=cfg(&s);struct kshim_power_ops o=ops(&s);
        assert(kshim_power_probe(&c,&o)==0);assert(c.capabilities==KSHIM_POWER_ALL);assert(s.writes==0);
        assert(kshim_power_execute(&c,&o,(kshim_power_action_t)a)==KSHIM_POWER_RETURNED);
        assert(s.dropped==1);assert(s.type==(a==1?4:a==3?7:1));assert(s.r[0][0x5b]&0x80);
        assert((s.r[0][0x5a]&0xf0)==0xa0);assert(s.val[0]==0); /* stale dload cookie cleared */
    }
    /* Fail at each preflight/prepare SPMI step: never deassert PS_HOLD. */
    for(int fail=1;fail<=10;fail++){struct sim s;struct kshim_power_config c=cfg(&s);struct kshim_power_ops o=ops(&s);
        assert(kshim_power_probe(&c,&o)==0);s.calls=0;s.fail_at=fail;
        int result=kshim_power_execute(&c,&o,KSHIM_POWER_EDL);
        if(result!=KSHIM_POWER_RETURNED)assert(!s.dropped);
    }
    struct sim s;struct kshim_power_config c=cfg(&s);struct kshim_power_ops o=ops(&s);
    s.r[0][5]=9;assert(kshim_power_probe(&c,&o)==0);assert(!c.capabilities);
    assert(kshim_power_execute(&c,&o,KSHIM_POWER_EDL)==KSHIM_POWER_UNSUPPORTED);assert(!s.writes&&!s.dropped);
    c=cfg(&s);c.count=0;c.psci_off=1;assert(kshim_power_probe(&c,&o)==0);assert(c.capabilities==KSHIM_POWER_BIT(KSHIM_POWER_OFF));
    assert(kshim_power_execute(&c,&o,KSHIM_POWER_HARD)==KSHIM_POWER_UNSUPPORTED);
    assert(kshim_power_execute(&c,&o,KSHIM_POWER_OFF)==KSHIM_POWER_RETURNED);
    c=cfg(&s);c.count=2;c.pmic[1]=(struct kshim_power_pmic){.base=0x800,.sid=1};s.r[1][1]=1;s.r[1][5]=5;s.r[1][0x5b]=0x80;
    assert(!kshim_power_probe(&c,&o));assert(kshim_power_execute(&c,&o,KSHIM_POWER_WARM)==KSHIM_POWER_RETURNED);assert((s.r[1][0x5a]&15)==1);
    c=cfg(&s);c.downstream=0;assert(!kshim_power_probe(&c,&o));
    assert(!(c.capabilities&KSHIM_POWER_BIT(KSHIM_POWER_EDL)));
    c=cfg(&s);c.force_warm=1;assert(!kshim_power_probe(&c,&o));
    assert(!(c.capabilities&KSHIM_POWER_BIT(KSHIM_POWER_HARD)));
    assert(kshim_power_execute(&c,&o,KSHIM_POWER_HARD)==KSHIM_POWER_UNSUPPORTED);
    c=cfg(&s);s.r[0][0x8f]=0xff;assert(!kshim_power_probe(&c,&o));
    assert(kshim_power_execute(&c,&o,KSHIM_POWER_WARM)==KSHIM_POWER_RETURNED);
    assert(s.r[0][0x8f]==1); /* Gen2 reason is bits 7:1, bit 0 is unrelated. */
    c=cfg(&s);c.pmic[0].store_reason=0;s.r[0][0x8f]=0xad;assert(!kshim_power_probe(&c,&o));
    assert(kshim_power_execute(&c,&o,KSHIM_POWER_WARM)==KSHIM_POWER_RETURNED);
    assert(s.r[0][0x8f]==0xad);
    for(int fail=1;fail<=2;fail++){
        c=cfg(&s);c.scm_sdi=1;s.scm_fail_on=fail;assert(!kshim_power_probe(&c,&o));
        assert(kshim_power_execute(&c,&o,KSHIM_POWER_EDL)==KSHIM_POWER_IO);
        assert(!s.dropped&&s.r[0][0x5a]==0xa7&&s.r[0][0x5b]==0x80);
    }
    /* SDI reports a separate service status in A1 even when transport A0 succeeds. */
    for(unsigned mode=0;mode<2;mode++) {
        c=cfg(&s);c.downstream=mode;c.scm_sdi=1;s.sdi_result=1;
        assert(!kshim_power_probe(&c,&o));
        assert(kshim_power_execute(&c,&o,KSHIM_POWER_WARM)==KSHIM_POWER_IO);
        assert(!s.dropped && s.r[0][0x5a]==0xa7 && s.r[0][0x5b]==0x80);
        assert(!c.capabilities);
    }
    c=cfg(&s);c.downstream=0;assert(!kshim_power_probe(&c,&o));
    assert(kshim_power_execute(&c,&o,KSHIM_POWER_HARD)==KSHIM_POWER_RETURNED);assert(s.download_arg==0x10);
    c=cfg(&s);c.pmic[0].revision=0;s.r[0][1]=0;s.r[0][5]=1;assert(!kshim_power_probe(&c,&o));
    assert(kshim_power_execute(&c,&o,KSHIM_POWER_WARM)==KSHIM_POWER_RETURNED);assert(s.r[0][0x5a]==0xa1);
    c=cfg(&s);c.unsupported_pon=1;assert(!kshim_power_probe(&c,&o));assert(!c.capabilities&&!s.writes);
    c=cfg(&s);c.blocked=KSHIM_POWER_BIT(KSHIM_POWER_WARM);assert(!kshim_power_probe(&c,&o));
    assert(!(c.capabilities&(KSHIM_POWER_BIT(KSHIM_POWER_WARM)|KSHIM_POWER_BIT(KSHIM_POWER_EDL))));
    for(unsigned subtype=3;subtype<=5;subtype++){
        c=cfg(&s);s.r[0][1]=0;s.r[0][5]=subtype;s.r[0][0x5b]=0x12;assert(!kshim_power_probe(&c,&o));
        assert(kshim_power_execute(&c,&o,KSHIM_POWER_WARM)==KSHIM_POWER_RETURNED);
        assert(s.r[0][0x5b]==0x92); /* Gen2/1REG always has a separate enable register. */
    }
    /* Actual MSM board DTs may expose TCSR instead of IMEM cookies. */
    for(unsigned a=1;a<=4;a++) {
        c=cfg(&s);c.edl=0;c.dload=0;c.scm_dload=0;c.tcsr=0x5000;c.scm_io=1;
        assert(!kshim_power_probe(&c,&o));assert(c.capabilities==KSHIM_POWER_ALL);
        assert(kshim_power_execute(&c,&o,(kshim_power_action_t)a)==KSHIM_POWER_RETURNED);
        assert(s.secure_address==0x5000&&s.secure_value==(a==KSHIM_POWER_EDL?1:0));
        for(unsigned i=0;i<s.n;i++)assert(s.addr[i]!=c.tcsr); /* never raw-write TCSR */
    }
    c=cfg(&s);c.edl=0;c.dload=0;c.scm_dload=0;c.tcsr=0x5000;c.scm_io=1;s.scm_fail_on=1;
    assert(!kshim_power_probe(&c,&o));assert(kshim_power_execute(&c,&o,KSHIM_POWER_EDL)==KSHIM_POWER_IO);
    assert(!s.dropped&&s.r[0][0x5a]==0xa7&&s.secure_value==0);
    c=cfg(&s);c.scm_dload=0;c.tcsr=0x5000;c.scm_io=0;
    assert(!kshim_power_probe(&c,&o));assert(!c.capabilities); /* known secure cookie cannot be cleared */
    puts("PASS: power types, EDL gate, Gen3 rejection, multi-PMIC, failure-before-reset and PSCI-off fallback");
}
