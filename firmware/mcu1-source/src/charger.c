#include "charger.h"
#include "charger_adc.h"
#include "charger_policy.h"
#include "fsl_device_registers.h"
#include "fsl_clock.h"
#include "fsl_reset.h"
#include <string.h>
/* Only stock address03. Preserve each recognized live configuration; writes are
 * charge enable/disable and configured watchdog refresh. No OTP/reset/config replay. */
static uint8_t initialized,phase,register_index,values[10],initial[10];
static uint32_t started,next_poll,errors,sweeps,valid,last_stat,last_feed,writes,policy_fault;
static uint8_t write_active,write_register,write_value;
static uint8_t verify_pending,verify_value;
static uint8_t indicator_status,indicator_fault,indicator_valid;
static uint32_t indicator_sampled;
static volatile uint32_t *reg(uint32_t a) { return (volatile uint32_t *)(uintptr_t)a; }
static void init(void)
{
    CLOCK_EnableClock(kCLOCK_Iocon);
    CLOCK_AttachClk(kFRO12M_to_FLEXCOMM6); CLOCK_EnableClock(kCLOCK_FlexComm6);
    RESET_PeripheralReset(kFC6_RST_SHIFT_RSTn);
    *reg(0x400010c0)=0x4112; *reg(0x400010b4)=0x4112;
    FLEXCOMM6->PSELID=(FLEXCOMM6->PSELID&~7u)|3u;
    I2C6->CFG=0; I2C6->CLKDIV=11; I2C6->MSTTIME=0x33;
    I2C6->STAT=(1u<<4)|(1u<<6); I2C6->CFG=1;
    initialized=1;
}
void omni_charger_poll(uint32_t now)
{
    if(!initialized) { init(); next_poll=now; }
    omni_charger_adc_poll(now);
    if(!phase && (int32_t)(now-next_poll)<0) return;
    uint32_t stat=I2C6->STAT; last_stat=stat;
    unsigned state=(stat>>1)&7u;
    if((stat&((1u<<4)|(1u<<6))) || (phase && (uint32_t)(now-started)>20u) ||
       ((stat&1u) && state>=3u)) {
        ++errors; I2C6->MSTCTL=4u; I2C6->CFG=0;
        I2C6->STAT=(1u<<4)|(1u<<6); I2C6->CFG=1;
        phase=0; register_index=0; valid=0; write_active=0; policy_fault=1; next_poll=now+1000u; return;
    }
    if(!(stat&1u)) return;
    switch(phase) {
    case 0:
        if(state!=0u) return;
        I2C6->MSTDAT=6u; I2C6->MSTCTL=2u; phase=1; started=now; break;
    case 1:
        if(state!=2u) break;
        I2C6->MSTDAT=write_active?write_register:register_index; I2C6->MSTCTL=1u; phase=2; break;
    case 2:
        if(state!=2u) break;
        if(write_active) {I2C6->MSTDAT=write_value; I2C6->MSTCTL=1u; phase=5;}
        else {I2C6->MSTDAT=7u; I2C6->MSTCTL=2u; phase=3;}
        break;
    case 3:
        if(state!=1u) break;
        values[register_index]=(uint8_t)I2C6->MSTDAT;
        if(!sweeps) initial[register_index]=values[register_index];
        valid|=1u<<register_index; I2C6->MSTCTL=4u; phase=4; break;
    case 4:
        if(state!=0u) break;
        phase=0;
        if(write_active) {
            write_active=0; ++writes; last_feed=now;
            if(write_register==1u) {verify_pending=1;verify_value=write_value;}
            register_index=0; valid=0; /* Fresh full readback before another decision. */
        } else if(++register_index==10u) {
            register_index=0; ++sweeps; next_poll=now+1000u;
            indicator_status=values[8]; indicator_fault=values[9];
            indicator_sampled=now; indicator_valid=1;
            if(verify_pending && values[1]!=verify_value) policy_fault=3;
            verify_pending=0;
            int action=omni_charger_action(values,omni_charger_adc_present(now),!policy_fault,
                                          now-last_feed,&write_register,&write_value);
            if(action<0) policy_fault=2;
            if(action>0) {write_active=1;next_poll=now;}
        }
        break;
    case 5:
        if(state!=2u) break;
        I2C6->MSTCTL=4u;phase=4;
        break;
    default: phase=0; break;
    }
}
unsigned omni_charger_indicator(uint32_t now)
{
    int present=omni_charger_adc_present(now);
    if(!present) return 0u;
    if(policy_fault || errors) return 4u;
    if(present<0 || !indicator_valid || (uint32_t)(now-indicator_sampled)>2500u) return 1u;
    if((indicator_status&0x80u) || (indicator_fault&0x3fu)) return 4u;
    unsigned state=(indicator_status>>3)&3u;
    return state==3u?3u:state?2u:1u;
}
void omni_charger_status(uint32_t out[15])
{
    uint32_t v[15]={1,initialized,phase,register_index,valid,sweeps,errors,last_stat};
    memcpy((uint8_t *)(v+8),values,10); memcpy((uint8_t *)(v+11),initial,10);
    v[14]=1u|(policy_fault<<8)|(writes<<16);
    memcpy(out,v,sizeof(v));
}
