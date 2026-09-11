#include "mcu2_probe.h"
#include "mcu2_runtime.h"
#include "mcu2_uart_lpc5528.h"
#include "control_uart.h"
#include "fsl_device_registers.h"
#include <string.h>

static omni_mcu2_runtime runtime;
static bool initialized, stopped;
static volatile uint32_t token, stage; /* 0 idle,1 queued,2 active,3 terminal. */
static uint8_t query_profile;
static uint32_t query_start,query_generation,query_tx_frame;
static omni_mcu2_phase query_phase;
static bool runtime_tx_idle(void *context)
{ (void)context;return omni_control_uart_tx_idle(7U); }

bool omni_mcu2_probe_busy(void) { return stage==1U || stage==2U; }
bool omni_mcu2_probe_request(uint32_t requested)
{ return omni_mcu2_probe_request_query(requested,0); }
bool omni_mcu2_probe_request_query(uint32_t requested,uint8_t profile)
{
    if(!requested || profile>1U || stopped || runtime.fault) return false;
    if(stage && requested==token) return profile==query_profile;
    if(stage==1U || stage==2U) return false;
    token=requested;query_profile=profile;stage=1;query_phase=OMNI_MCU2_RUNNING;return true;
}
void omni_mcu2_service(uint32_t now,bool allowed)
{
    if(!allowed) {
        if(initialized && !stopped) { omni_mcu2_runtime_stop(&runtime);omni_mcu2_uart_stop();stopped=true; }
        if(stage==1U || stage==2U) { stage=3;query_phase=OMNI_MCU2_CANCELLED; }
        return;
    }
    if(stopped) return;
    if(!initialized) {
        mcu2_link_io io=omni_mcu2_uart_init();
        if(!omni_mcu2_runtime_init(&runtime,io,now)) {
            stopped=true;query_phase=OMNI_MCU2_IO_ERROR;stage=3;return;
        }
        runtime.tx_idle=runtime_tx_idle;
        initialized=true;
    }
    if(stage==1U) {
        query_generation=query_profile?runtime.detect_generation:runtime.version_generation;
        query_tx_frame=runtime.query_sent[query_profile];query_start=now;
        if(!omni_mcu2_runtime_query(&runtime,(omni_mcu2_query)query_profile)) {
            stage=3;query_phase=OMNI_MCU2_IO_ERROR;
        } else stage=2;
    }
    omni_mcu2_runtime_poll(&runtime,now);
    if(runtime.fault) {
        omni_mcu2_uart_stop();stopped=true;
        if(stage==1U || stage==2U) { stage=3;query_phase=OMNI_MCU2_IO_ERROR; }
    }
    if(stage==2U) {
        uint32_t generation=query_profile?runtime.detect_generation:runtime.version_generation;
        if(generation!=query_generation && runtime.query_replied[query_profile]!=query_tx_frame) {
            stage=3;query_phase=OMNI_MCU2_SUCCESS;
        } else if((uint32_t)(now-query_start)>=OMNI_MCU2_QUERY_TIMEOUT_MS) {
            stage=3;query_phase=OMNI_MCU2_TIMEOUT;
        }
    }
}
/* Legacy probe never reserves/stops UART3. Main uses the persistent service. */
void omni_mcu2_probe_poll(uint32_t now,bool allowed)
{ if(stage==1U || stage==2U) omni_mcu2_service(now,allowed); }
bool omni_mcu2_forward_lifecycle(uint8_t state)
{ return omni_mcu2_runtime_lifecycle(&runtime,state); }
bool omni_mcu2_forward_dsp_state86(uint8_t state)
{ return omni_mcu2_runtime_dsp86(&runtime,state); }
bool omni_mcu2_forward_controls(uint8_t volume,uint8_t balance,uint8_t mic_state,
                                uint8_t mic_percent,uint8_t setting_percent)
{ return omni_mcu2_runtime_controls(&runtime,volume,balance,mic_state,mic_percent,setting_percent); }
bool omni_mcu2_select_input(uint8_t side)
{ return omni_mcu2_runtime_select(&runtime,side); }
bool omni_mcu2_set_source_gain(uint8_t index)
{ return omni_mcu2_runtime_gain(&runtime,index); }
bool omni_mcu2_runtime_read(unsigned page,uint32_t out[15])
{
    if(!out || page>2U) return false;
    uint32_t mask=__get_PRIMASK();__disable_irq();
    omni_mcu2_runtime_status(&runtime,page,out);__set_PRIMASK(mask);return true;
}
void omni_mcu2_probe_status(uint8_t out[60])
{
    /* V2 counters describe the persistent listener; token/stage/phase still
     * describe the last explicit query. Readiness A0/FF is in runtime status. */
    uint32_t words[15]={2};
    uint32_t mask=__get_PRIMASK();__disable_irq();
    words[1]=token;words[2]=stage;words[3]=(uint32_t)query_phase;
    words[4]=runtime.tx_bytes;words[5]=runtime.rx_bytes;
    words[6]=runtime.rx_frames;words[7]=runtime.rejected_frames;
    words[8]=query_profile?runtime.queried_ports:
        (uint32_t)runtime.version[0]|((uint32_t)runtime.version[1]<<8)|((uint32_t)runtime.version[2]<<16);
    omni_mcu2_uart_stats(words+9);
    words[14]|=((uint32_t)runtime.rx_used<<16)|((uint32_t)query_profile<<8);
    __set_PRIMASK(mask);memcpy(out,words,sizeof(words));
}
bool omni_mcu2_probe_trace(uint8_t page,uint32_t out[15])
{
    if(!out || page>2U) return false;
    uint32_t mask=__get_PRIMASK();__disable_irq();memset(out,0,60);
    out[0]=2;out[1]=page;out[2]=token;
    if(page==0U) {
        out[3]=stage;out[4]=(uint32_t)query_phase;out[5]=OMNI_MCU2_QUERY_TIMEOUT_MS;
        out[6]=initialized?runtime.trace_first_tx_ms:UINT32_MAX;
        out[7]=initialized?runtime.trace_last_tx_ms:UINT32_MAX;out[8]=UINT32_MAX;
        out[9]=initialized?runtime.trace_first_rx_ms:UINT32_MAX;
        out[10]=initialized?runtime.trace_last_rx_ms:UINT32_MAX;
        out[11]=runtime.trace_rx_used;out[12]=runtime.trace_rx_truncated;
        out[13]=runtime.tx_bytes;out[14]=runtime.rx_bytes;
    } else { out[3]=runtime.trace_rx_used;memcpy(out+4,runtime.trace_rx+(page-1U)*32U,32); }
    __set_PRIMASK(mask);return true;
}
