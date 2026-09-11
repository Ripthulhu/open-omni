#include "charger_adc.h"
#include "fsl_device_registers.h"
#include "fsl_clock.h"
#include <string.h>
/* Source-owned, bounded implementation of the recovered stock ADC sequence.
 * GPADC only; no audio PLL, USB clocks or DMA changes. */
static uint32_t adc_phase,deadline,sampled_at,samples,raw_battery,raw_presence,error;
static uint32_t pending_battery;
static volatile uint32_t *r(uint32_t offset) { return (volatile uint32_t *)(uintptr_t)(0x400a0000u+offset); }
void omni_charger_adc_poll(uint32_t now)
{
    if(adc_phase==0) {
        CLOCK_EnableClock(kCLOCK_Iocon); CLOCK_EnableClock(kCLOCK_Adc0);
        CLOCK_SetClkDiv(kCLOCK_DivAdcAsyncClk,8u,true);
        CLOCK_AttachClk(kFRO_HF_to_ADC_CLK);
        *(volatile uint32_t *)0x4000107c=0x400;
        *(volatile uint32_t *)0x400010a4=0x400;
        *(volatile uint32_t *)0x400200c8=(1u<<15)|(1u<<19);
        *r(0x10)=2; *r(0x10)=0; *r(0x10)=0x300;
        *r(0x10)=0x70000; *r(0x20)=0x10800040; *r(0x24)=0;
        *r(0xe0)=0; *r(0xe4)=0; *r(0x10)=0x70001;
        *r(0x40)=0x100010;
        deadline=now+10; adc_phase=1; return;
    }
    if(adc_phase==7) return;
    if(adc_phase==1) {
        if((int32_t)(now-deadline)<0) return;
        *r(0x10)|=8; deadline=now+100; adc_phase=2; return;
    }
    if(adc_phase==2) {
        uint32_t a=*r(0xf0),b=*r(0xf4);
        if((a&(1u<<24)) && (b&(1u<<24))) {
            *r(0xf8)=((a<<16)/(0x1ffffu-(a&0xffffu)))|(1u<<24);
            *r(0xfc)=((b<<16)/(0x1ffffu-(b&0xffffu)))|(1u<<24);
            adc_phase=3;
        }
    } else if(adc_phase==3) {
        if(*r(0x14)&(1u<<10)) {
            *r(0x100)=3; *r(0x104)=0x700;
            *r(0x108)=0x24; *r(0x10c)=0x700;
            *r(0xa0)=0x01000000; *r(0xa4)=0x02000000;
            adc_phase=4; deadline=now;
        }
    } else if(adc_phase==4) {
        if((int32_t)(now-deadline)<0) return;
        *r(0x34)=1; adc_phase=5; deadline=now+20;
    } else if(adc_phase==5 || adc_phase==6) {
        uint32_t data=*r(0x300);
        if(data&(1u<<31)) {
            if(adc_phase==5) {pending_battery=(data&0xffffu)>>3; *r(0x34)=2; adc_phase=6;}
            else {raw_battery=pending_battery; raw_presence=(data&0xffffu)>>3; sampled_at=now; ++samples; adc_phase=4; deadline=now+1000;}
            return;
        }
    }
    if(adc_phase!=4 && (int32_t)(now-deadline)>0) { error=adc_phase; adc_phase=7; }
}
int omni_charger_adc_present(uint32_t now)
{
    if(!samples || error || (uint32_t)(now-sampled_at)>2500u) return -1;
    return raw_presence<0x834u && raw_battery>1000u && raw_battery<2100u;
}
int omni_charger_adc_millivolts(uint32_t now)
{
    if(omni_charger_adc_present(now)!=1) return -1;
    return (int)((raw_battery*0x2c4fcu)/0x15feau-20u);
}
void omni_charger_adc_status(uint32_t out[15])
{
    uint32_t v[15]={1,adc_phase,error,samples,sampled_at,raw_battery,raw_presence,
        raw_battery?((raw_battery*0x2c4fcu)/0x15feau-20u):0};
    v[8]=*r(0x14);v[9]=*r(0xf0);v[10]=*r(0xf4);v[11]=*r(0x20);
    memcpy(out,v,sizeof(v));
}
