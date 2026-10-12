/* SPDX-License-Identifier: MIT */
#include <power.h>
#include <qcom_dt.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
static uint64_t storage[2048];
static void reg2(void *f,int n,uint32_t a,uint32_t b)
{ fdt32_t p[]={cpu_to_fdt32(a),cpu_to_fdt32(b)};assert(!fdt_setprop(f,n,"reg",p,sizeof p)); }
static int node(void *f,int parent,const char *name,const char *compatible)
{ int n=fdt_add_subnode(f,parent,name);assert(n>0);assert(!fdt_setprop_string(f,n,"compatible",compatible));return n; }
static void *fixture(void)
{
    void *f=storage;assert(!fdt_create_empty_tree(f,sizeof storage));
    assert(!fdt_setprop_u32(f,0,"#address-cells",1));assert(!fdt_setprop_u32(f,0,"#size-cells",1));
    int n=node(f,0,"power@1000","qcom,pshold");reg2(f,n,0x1000,4);
    assert(!fdt_setprop_string(f,n,"reg-names","pshold-base"));
    n=node(f,0,"edl@2000","qcom,msm-imem-emergency_download_mode");reg2(f,n,0x2000,12);
    n=node(f,0,"dump@3000","qcom,msm-imem-download_mode");reg2(f,n,0x3000,8);
    n=node(f,0,"reason@4000","qcom,msm-imem-restart_reason");reg2(f,n,0x4000,4);
    int arb=node(f,0,"spmi","qcom,spmi-pmic-arb");
    assert(!fdt_setprop_u32(f,arb,"#address-cells",2));assert(!fdt_setprop_u32(f,arb,"#size-cells",0));
    assert(!fdt_setprop_u32(f,arb,"qcom,bus-id",1));
    int pmic=node(f,arb,"pmic@0","qcom,spmi-pmic");reg2(f,pmic,0,0);
    assert(!fdt_setprop_u32(f,pmic,"#address-cells",1));assert(!fdt_setprop_u32(f,pmic,"#size-cells",0));
    n=node(f,pmic,"pon@800","qcom,qpnp-power-on");assert(!fdt_setprop_u32(f,n,"reg",0x800));
    assert(!fdt_setprop(f,n,"qcom,system-reset",NULL,0));return f;
}
int main(void)
{
    struct kshim_power_config c;void *f=fixture();
    assert(!kshim_dt_power(f,&c));assert(c.count==1&&c.pmic[0].sid==0&&c.pmic[0].bus==1&&c.pmic[0].base==0x800);
    assert(c.downstream&&c.pshold==0x1000&&c.edl==0x2000&&c.dload==0x3000&&c.reason==0x4000);
    assert(!fdt_setprop(f,fdt_path_offset(f,"/power@1000"),"qcom,force-warm-reboot",NULL,0));
    assert(!kshim_dt_power(f,&c)&&c.force_warm);
    f=fixture();reg2(f,fdt_path_offset(f,"/edl@2000"),0x1000,12);assert(kshim_dt_power(f,&c)<0&&!c.pshold&&!c.count);
    f=fixture();reg2(f,fdt_path_offset(f,"/edl@2000"),0x2000,8);assert(kshim_dt_power(f,&c)<0);
    f=fixture();reg2(f,fdt_path_offset(f,"/power@1000"),0x1001,4);assert(kshim_dt_power(f,&c)<0);
    f=fixture();assert(!fdt_setprop_u32(f,fdt_path_offset(f,"/spmi/pmic@0/pon@800"),"qcom,system-reset",1));assert(kshim_dt_power(f,&c)<0);
    f=fixture();assert(!fdt_setprop_string(f,fdt_path_offset(f,"/spmi"),"status","disabled"));assert(!kshim_dt_power(f,&c)&&!c.count);
    f=fixture();int pon=fdt_path_offset(f,"/spmi/pmic@0/pon@800");
    assert(!fdt_setprop_string(f,pon,"compatible","qcom,pm8998-pon"));assert(!kshim_dt_power(f,&c)&&c.count==1&&!c.downstream);
    f=fixture();pon=fdt_path_offset(f,"/spmi/pmic@0/pon@800");assert(!fdt_setprop_string(f,pon,"compatible","qcom,pmk8350-pon"));
    assert(!kshim_dt_power(f,&c)&&!c.count); /* unsupported Gen3 is never guessed */
    f=fixture();pon=node(f,fdt_path_offset(f,"/spmi/pmic@0"),"pon@900","qcom,qpnp-power-on");
    assert(!fdt_setprop_u32(f,pon,"reg",0x900));assert(!fdt_setprop(f,pon,"qcom,system-reset",NULL,0));
    assert(kshim_dt_power(f,&c)<0&&!c.count); /* ambiguous primary */
    assert(kshim_dt_power(NULL,&c)<0&&!c.count);
    const char *boards[]={"andromeda-fdt.dtb","hdk8150-fdt.dtb"};
    for(unsigned i=0;i<2;i++){
        char path[1024];snprintf(path,sizeof path,"%s/%s",FIXTURES,boards[i]);FILE *file=fopen(path,"rb");assert(file);
        assert(!fseek(file,0,SEEK_END));long size=ftell(file);assert(size>0);rewind(file);
        void *data=malloc((size_t)size);assert(data&&fread(data,1,(size_t)size,file)==(size_t)size);fclose(file);
        assert(!kshim_dt_power(data,&c));assert(c.count==1&&c.downstream&&c.pmic[0].store_reason);
        assert(c.pshold&&c.tcsr==0x1fd3000&&!c.edl&&!c.dload&&!c.unsupported_pon);
        printf("DT %s: PON controller=%u bus=%u SID=%u base=%#x, PS_HOLD=%#lx TCSR=%#lx\n",boards[i],
            c.pmic[0].controller,c.pmic[0].bus,c.pmic[0].sid,c.pmic[0].base,(unsigned long)c.pshold,(unsigned long)c.tcsr);
        free(data);
    }
    puts("PASS: DT resources, bus identity, download ABI, disabled/unknown PON, duplicate primary, malformed and overlapping ranges");
}
