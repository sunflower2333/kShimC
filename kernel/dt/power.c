/* SPDX-License-Identifier: MIT */
#include <power.h>
#include <qcom_dt.h>
#include <string.h>
static int compat(const void *f,int n,const char *s){return !fdt_node_check_compatible(f,n,s);}
static int region(const void *f,int n,const char *name,size_t bytes,uintptr_t *out)
{
    struct dt_range r;
    int ret=name?dt_reg_named(f,n,name,&r):dt_reg(f,n,0,&r);
    if(ret||!r.base||(r.base&3)||r.size<bytes||r.base>UINTPTR_MAX-bytes||*out)return -1;
    *out=(uintptr_t)r.base;return 0;
}
int kshim_dt_power(const void *f,struct kshim_power_config *c)
{
    if(!c)return -1;
    *c=(struct kshim_power_config){0};
    if(!f||fdt_check_header(f))return -1;
    unsigned primary_count=0;
    for(int n=fdt_next_node(f,-1,NULL);n>=0;n=fdt_next_node(f,n,NULL)) {
        /* Checking ancestor status walks the FDT. Filter first so large board
         * trees do not pay that cost for every unrelated node. */
        if(!compat(f,n,"qcom,pshold")&&!compat(f,n,"qcom,msm-imem-emergency_download_mode")&&
           !compat(f,n,"qcom,msm-imem-download_mode")&&!compat(f,n,"qcom,msm-imem-restart_reason")&&
           !compat(f,n,"qcom,qpnp-power-on")&&!compat(f,n,"qcom,pm8916-pon")&&
           !compat(f,n,"qcom,pm8998-pon")&&!compat(f,n,"qcom,pms405-pon"))continue;
        if(!dt_available(f,n))continue;
        if(compat(f,n,"qcom,pshold")) {
            if(region(f,n,fdt_getprop(f,n,"reg-names",NULL)?"pshold-base":NULL,4,&c->pshold))goto bad;
            if(dt_string_index(f,n,"reg-names","tcsr-boot-misc-detect")>=0&&
               region(f,n,"tcsr-boot-misc-detect",4,&c->tcsr))goto bad;
            bool warm=false;
            if(dt_bool(f,n,"qcom,force-warm-reboot",&warm))goto bad;
            c->force_warm=warm;
        } else if(compat(f,n,"qcom,msm-imem-emergency_download_mode")) {
            if(region(f,n,NULL,12,&c->edl))goto bad;
        } else if(compat(f,n,"qcom,msm-imem-download_mode")) {
            if(region(f,n,NULL,8,&c->dload))goto bad;
        } else if(compat(f,n,"qcom,msm-imem-restart_reason")) {
            if(region(f,n,NULL,4,&c->reason))goto bad;
        }
        int downstream=compat(f,n,"qcom,qpnp-power-on");
        int mainline=compat(f,n,"qcom,pm8916-pon")||compat(f,n,"qcom,pm8998-pon")||compat(f,n,"qcom,pms405-pon");
        if(!downstream&&!mainline)continue;
        bool primary=false,secondary=false;
        if(downstream) {
            if(dt_bool(f,n,"qcom,system-reset",&primary)||dt_bool(f,n,"qcom,secondary-pon-reset",&secondary))goto bad;
            if(!primary&&!secondary)continue;
            if(primary&&secondary)goto bad;
            c->downstream=1;
        } else primary=true;
        if(c->count==KSHIM_POWER_MAX_PMICS)goto bad;
        struct kshim_power_pmic p={0};
        if(kshim_dt_spmi_identity(f,n,&p.controller,&p.bus,&p.sid)||kshim_dt_spmi_reg(f,n,&p.base))goto bad;
        if(downstream) {
            bool value=false;
            if(dt_bool(f,n,"qcom,store-hard-reset-reason",&value))goto bad;
            p.store_reason=value;
            if(dt_bool(f,n,"qcom,use-legacy-hard-reset-offset",&value))goto bad;
            p.legacy_reason=value;
            const char *complex[]={"qcom,resin-pon-reset","qcom,support-twm-config"};
            for(unsigned i=0;i<2;i++){if(dt_bool(f,n,complex[i],&value))goto bad;if(value)c->unsupported_pon=1;}
            const char *overrides[]={"qcom,shutdown-poweroff-type","qcom,warm-reset-poweroff-type","qcom,hard-reset-poweroff-type"};
            const unsigned types[]={4,1,7};
            for(unsigned i=0;i<3;i++)if(fdt_getprop(f,n,overrides[i],NULL)){
                uint32_t type;if(dt_u32(f,n,overrides[i],&type)||!type||type>15)goto bad;
                if(type!=types[i])c->blocked|=KSHIM_POWER_BIT(i+1);
            }
            if(dt_bool(f,n,"qcom,ps-hold-shutdown-disable",&value))goto bad;
            if(value)c->blocked|=KSHIM_POWER_BIT(KSHIM_POWER_OFF);
            if(dt_bool(f,n,"qcom,ps-hold-hard-reset-disable",&value))goto bad;
            if(value)c->blocked|=KSHIM_POWER_BIT(KSHIM_POWER_HARD);
        }
        if(primary) {
            if(++primary_count>1)goto bad;
            if(c->count)c->pmic[c->count]=c->pmic[0];
            c->pmic[0]=p;
        } else c->pmic[c->count]=p;
        c->count++;
    }
    if(c->count&&!primary_count)goto bad;
    /* An overlapping DT mapping must never turn cookie writes into PS_HOLD. */
    const uintptr_t addresses[]={c->pshold,c->edl,c->dload,c->reason,c->tcsr};
    const unsigned lengths[]={4,12,8,4,4};
    for(unsigned i=0;i<5;i++)for(unsigned j=0;j<i;j++)if(addresses[i]&&addresses[j]&&
        addresses[i]<addresses[j]+lengths[j]&&addresses[j]<addresses[i]+lengths[i])goto bad;
    return 0;
bad:
    *c=(struct kshim_power_config){0};return -1;
}
