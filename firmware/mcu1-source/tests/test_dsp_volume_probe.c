#include "dsp_volume_probe.h"
#include "dsp_volume.h"
#include "dsp_gain_trial.h"
#include "control_uart.h"
#include "fsl_device_registers.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

test_usart test_uart3;
static uint8_t tx_bytes[128],rx_bytes[256];
static uint8_t mixer[4]={20,21,40,60};
static unsigned frame_start;
static unsigned sent,received,available,starts,stops;
static bool start_ok=true,send_reply=true;
static uint32_t primask,uart_fault;
static int16_t current_db=-20*256;
static uint8_t current_mute=1;
uint32_t __get_PRIMASK(void) { return primask; }
void __disable_irq(void) { primask=1; }
void __set_PRIMASK(uint32_t v) { primask=v; }
void audio_probe_volume_snapshot(int16_t *db,uint8_t *mute)
{ *db=current_db; *mute=current_mute; }
static int tx(void *context,uint8_t b)
{
    assert(!primask && context==(void *)3);
    assert(sent<sizeof(tx_bytes)); tx_bytes[sent++]=b;
    if(send_reply && sent-frame_start>=2u && sent-frame_start==tx_bytes[frame_start+1u]) {
        uint8_t op=tx_bytes[frame_start+2u];
        const uint8_t mode[]={0xdb,5,0x43,3,2,0xdd,3,0x43,0};
        uint8_t levels[]={0xdb,8,0x47,3,0,0,0,0,0xdd,3,0x47,0};
        const uint8_t ack[]={0xdd,3,0x47,0};
        bool set=tx_bytes[frame_start+3u]==1;
        if(set) { assert(op==0x47); memcpy(mixer,tx_bytes+frame_start+4u,4); }
        memcpy(levels+4,mixer,4);
        const uint8_t *p=set?ack:(op==0x43u?mode:levels);
        unsigned n=set?sizeof(ack):(op==0x43u?sizeof(mode):sizeof(levels));
        memcpy(rx_bytes+available,p,n); available+=n;
        frame_start=sent;
    }
    return 1;
}
static int rx(void *context,uint8_t *b)
{
    assert(!primask && context==(void *)3);
    if(received==available) return 0;
    *b=rx_bytes[received++]; return 1;
}
bool omni_control_uart_start(unsigned port,mcu2_link_io *io)
{
    assert(!primask && port==3); ++starts;
    if(!start_ok) return false;
    *io=(mcu2_link_io){(void *)3,tx,rx}; return true;
}
void omni_control_uart_stop(unsigned port) { assert(!primask && port==3); ++stops; }
void omni_control_uart_stats(unsigned port,uint32_t out[6])
{ assert(port==3); memset(out,0,24); out[0]=received;out[1]=sent;out[5]=uart_fault; }
static uint32_t field(unsigned page,unsigned index)
{ uint32_t out[15];assert(omni_dsp_volume_probe_status(page,out));return out[index]; }

int main(int argc,char **argv)
{
    assert(argc==2); unsigned scenario=(unsigned)strtoul(argv[1],NULL,10);
    uint32_t out[15]; memset(out,0xa5,sizeof(out));
    assert(!omni_dsp_volume_probe_status(3,out) && out[0]==0xa5a5a5a5u);
    omni_dsp_volume_probe_poll(0,false,true);
    omni_dsp_volume_probe_poll(1,true,false);
    assert(!starts && !stops && field(0,8)==(uint32_t)(int32_t)current_db);
    if(scenario==1u) start_ok=false;
    if(scenario==2u) send_reply=false;
    test_uart3.FIFOSTAT=USART_FIFOSTAT_TXEMPTY_MASK;
    test_uart3.STAT=0; /* FIFO empty is insufficient physical completion. */
    /* A brief stream must not spend the once-per-boot query attempt. */
    omni_dsp_volume_probe_poll(2,true,true);
    omni_dsp_volume_probe_poll(401,true,true);
    assert(!starts);
    omni_dsp_volume_probe_poll(402,false,true);
    omni_dsp_volume_probe_poll(500,true,true);
    omni_dsp_volume_probe_poll(999,true,true);
    assert(!starts);
    /* Losing permission also restarts the full debounce interval. Test its
     * unsigned elapsed-time handling across the uint32 millisecond wrap. */
    omni_dsp_volume_probe_poll(1000,true,false);
    omni_dsp_volume_probe_poll(UINT32_MAX-249u,true,true);
    omni_dsp_volume_probe_poll(249,true,true);
    assert(!starts && !stops);
    omni_dsp_volume_probe_poll(250,true,true);
    if(scenario==1u) {
        assert(starts==1 && !stops && field(0,3)==OMNI_DSP_VOLUME_FAILED);
        assert(field(2,10)==1);
    } else if(scenario==2u) {
        test_uart3.STAT=USART_STAT_TXIDLE_MASK;
        omni_dsp_volume_probe_poll(251,true,true);
        omni_dsp_volume_probe_poll(500,true,true);
        assert(stops==1 && field(0,4)==OMNI_DSP_VOLUME_TIMEOUT);
    } else if(scenario==3u) {
        omni_dsp_volume_probe_poll(251,false,true);
        assert(stops==1 && field(0,3)==OMNI_DSP_VOLUME_CANCELED && field(2,11)==1);
    } else if(scenario==4u) {
        uart_fault=1; omni_dsp_volume_probe_poll(251,true,true);
        assert(stops==1 && field(0,4)==OMNI_DSP_VOLUME_IO_ERROR && field(2,8)==1);
    } else {
        omni_dsp_volume_probe_poll(251,true,true);
        omni_dsp_volume_probe_poll(278,true,true);
        assert(sent==4 && omni_dsp_volume_probe_busy() && field(0,3)==OMNI_DSP_VOLUME_DRAIN);
        test_uart3.STAT=USART_STAT_TXIDLE_MASK;
        omni_dsp_volume_probe_poll(279,true,true);
        omni_dsp_volume_probe_poll(298,true,true); assert(sent==4);
        omni_dsp_volume_probe_poll(299,true,true);
        omni_dsp_volume_probe_poll(300,true,true);
        omni_dsp_volume_probe_poll(301,true,true);
        omni_dsp_volume_probe_poll(321,true,true);
        assert(!omni_dsp_volume_probe_busy() && starts==1 && stops==1 && sent==8);
        assert(field(0,3)==OMNI_DSP_VOLUME_COMPLETE && field(0,5)==3);
        assert(field(0,7)==0x3c281514u && field(2,14)==USART_STAT_TXIDLE_MASK);
    }
    unsigned stopped=stops, sent_at_end=sent;
    current_db=-42*256; current_mute=0;
    omni_dsp_volume_probe_poll(1001,false,false);
    omni_dsp_volume_probe_poll(1002,true,true);
    omni_dsp_volume_probe_poll(1502,true,true);
    assert(starts==1 && stops==stopped && sent==sent_at_end);
    assert(field(0,8)==(uint32_t)(int32_t)current_db && field(0,9)==0);
    assert(!primask);
    if(scenario>=5u) {
        assert(!omni_dsp_gain_trial_request(0));
        mixer[0]=scenario==8u?75:100;
        if(scenario==8u) {
            const uint8_t original[]={100,21,40,60};
            assert(omni_dsp_gain_restore_request(123,mixer,original));
            assert(omni_dsp_gain_restore_request(123,mixer,original));
            assert(!omni_dsp_gain_trial_request(123));
        } else {
            assert(omni_dsp_gain_trial_request(123));
            assert(omni_dsp_gain_trial_request(123));
        }
        assert(!omni_dsp_gain_trial_request(456));
        assert(omni_dsp_volume_probe_busy());
        if(scenario==6u) start_ok=false;
        for(uint32_t now=1503;now<13000u && omni_dsp_volume_probe_busy();++now)
            omni_dsp_volume_probe_poll(now,scenario!=7u,true);
        assert(omni_dsp_gain_trial_status(0,out) && out[3]==3);
        if(scenario==5u || scenario==8u) {
            assert(starts==2 && stops==2 && out[4]==GAIN_DONE && out[8]==(scenario==8u?2u:3u));
            assert(mixer[0]==100 && mixer[1]==21 && mixer[2]==40 && mixer[3]==60);
        } else if(scenario==6u) assert(out[4]==GAIN_FAILED && starts==2 && stops==1);
        else assert(out[4]==GAIN_CANCELED && starts==1 && stops==1);
        unsigned end=sent;
        assert(omni_dsp_gain_trial_request(123)==(scenario!=8u));
        omni_dsp_volume_probe_poll(14000,true,true); assert(sent==end);
    }
    puts("DSP volume adapter scenario passed");return 0;
}
