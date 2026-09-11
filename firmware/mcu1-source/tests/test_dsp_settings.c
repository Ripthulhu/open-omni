#include "dsp_settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct { uint8_t data[300];unsigned used,budget;int drained,error; } bus;
static uint32_t token=100u;
static int tx(void *context,uint8_t byte)
{
    (void)context;
    if(bus.error) return bus.error;
    if(!bus.budget) return 0;
    --bus.budget;assert(bus.used<sizeof(bus.data));bus.data[bus.used++]=byte;return 1;
}
static int drain(void *context) { (void)context;return bus.drained; }
static void poll(uint32_t now,bool can_start,unsigned budget)
{
    bus.budget=budget;unsigned before=bus.used;
    omni_dsp_settings_poll(now,can_start,(omni_dsp_settings_io){0,tx,drain});
    assert(bus.used-before<=16u);
}
static void feed(const uint8_t *p,size_t n,uint32_t now)
{ for(size_t i=0;i<n;++i) omni_dsp_settings_observe(p[i],now); }
static uint32_t phase(void)
{ uint32_t w[15];assert(omni_dsp_settings_status(0,w));return w[4]; }
static void ack(uint8_t opcode,uint8_t status,uint32_t now)
{ const uint8_t p[4]={0xdd,3,opcode,status};feed(p,sizeof(p),now); }
static void reset(uint32_t now)
{
    omni_dsp_settings_release(now);omni_dsp_settings_acquire();++token;
    memset(&bus,0,sizeof(bus));bus.drained=1;
}
static void request(unsigned control,const uint8_t *value,size_t n,uint32_t now)
{ reset(now);assert(omni_dsp_settings_request(token,control,value,n,now)); }
static uint32_t cached(unsigned control,uint8_t out[60])
{ uint32_t flags;assert(omni_dsp_settings_value(control,0,out));memcpy(&flags,out+16,4);return flags; }
static void scalar_packets(void)
{
    static const struct { unsigned id,n;uint8_t value[3],wire[9]; } cases[]={
        {DSP_SETTING_LIMITER,1,{1},{0xbd,5,0xd2,8,1}},
        {DSP_SETTING_MIC_VOLUME,1,{8},{0xbd,6,0xd3,1,2,8}},
        {DSP_SETTING_SIDETONE,2,{1,5},{0xbd,7,0xd4,1,1,1,5}},
        {DSP_SETTING_MIC_NOISE,2,{1,3},{0xbd,7,0xdb,1,1,3,0}},
        {DSP_SETTING_ANC_STATE,1,{4},{0xbd,5,0xd5,1,4}},
        {DSP_SETTING_ANC_LEVEL,1,{2},{0xbd,6,0xd5,1,0x11,2}},
        {DSP_SETTING_TRANSPARENCY,1,{8},{0xbd,6,0xd5,1,0x10,8}},
        {DSP_SETTING_BT_STARTUP,3,{1,7,2},{0xbd,9,0xe3,1,1,1,7,2,0}},
        {DSP_SETTING_MIC_LED,3,{1,7,2},{0xbd,9,0xe3,1,1,1,7,2,1}},
        {DSP_SETTING_BT_CALL,3,{1,7,2},{0xbd,9,0xe3,1,1,1,7,2,2}},
        {DSP_SETTING_AUTO_OFF,1,{30},{0xbd,6,0xe3,1,2,30}},
        {DSP_SETTING_HOME_MODE,1,{0},{0xbd,6,0xd2,9,1,0}},
        {DSP_SETTING_HOME_MODE,1,{1},{0xbd,6,0xd2,9,1,1}},
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        request(cases[i].id,cases[i].value,cases[i].n,0);
        assert(phase()==DSP_SETTINGS_QUEUED);
        assert(omni_dsp_settings_request(token,cases[i].id,cases[i].value,cases[i].n,1));
        poll(1,false,16);assert(!bus.used);
        poll(2,true,16);assert(phase()==DSP_SETTINGS_WAIT);
        assert(bus.used==cases[i].wire[1] && !memcmp(bus.data,cases[i].wire,bus.used));
        /* Status query response or another opcode ACK cannot confirm a SET. */
        ack(0xe1,0,3);poll(3,false,16);assert(phase()==DSP_SETTINGS_WAIT);
        ack(cases[i].wire[2],0,4);poll(4,false,16);
        assert(phase()==DSP_SETTINGS_ACCEPTED);
        uint8_t out[60];assert(cached(cases[i].id,out)==3u);
        assert(!memcmp(out+24,cases[i].value,cases[i].n));
    }
}
static void validation_and_coalescing(void)
{
    const uint8_t mic8=8,mic7=7,bad=255;
    assert(omni_dsp_settings_valid(DSP_SETTING_MIC_VOLUME,&mic8,1));
    assert(!omni_dsp_settings_valid(DSP_SETTING_MIC_VOLUME,&bad,1));
    const uint8_t led_off[]={0,0,0};assert(omni_dsp_settings_valid(DSP_SETTING_MIC_LED,led_off,3));
    request(DSP_SETTING_MIC_VOLUME,&mic8,1,0);
    uint32_t before[15],after[15];assert(omni_dsp_settings_status(0,before));
    assert(!omni_dsp_settings_request(0,DSP_SETTING_MIC_VOLUME,&mic8,1,0));
    assert(!omni_dsp_settings_request(token,DSP_SETTING_MIC_VOLUME,&mic7,1,0));
    assert(!omni_dsp_settings_request(token+1,DSP_SETTING_MIC_VOLUME,&bad,1,0));
    assert(!omni_dsp_settings_request(token+1,DSP_SETTING_MIC_VOLUME,NULL,1,0));
    assert(!omni_dsp_settings_request(token+1,DSP_SETTING_MIC_VOLUME,&mic8,2,0));
    assert(!omni_dsp_settings_request(token+1,0,&mic8,1,0));
    assert(!omni_dsp_settings_request(token+1,DSP_SETTING_COUNT,&mic8,1,0));
    assert(!omni_dsp_settings_request(token+1,DSP_SETTING_AUTO_OFF,&mic8,1,0));
    assert(omni_dsp_settings_status(0,after));assert(!memcmp(before,after,sizeof(before)));
    assert(omni_dsp_settings_request(++token,DSP_SETTING_MIC_VOLUME,&mic7,1,2));
    poll(3,true,16);assert(bus.data[5]==7);
    assert(!omni_dsp_settings_request(token+1,DSP_SETTING_MIC_VOLUME,&mic8,1,4));
    ack(0xd3,0,4);poll(4,false,16);assert(phase()==DSP_SETTINGS_ACCEPTED);
    static const struct {unsigned id,n;uint8_t data[3];} invalid[]={
        {DSP_SETTING_LIMITER,1,{2}}, {DSP_SETTING_MIC_VOLUME,1,{0}},
        {DSP_SETTING_SIDETONE,2,{2,5}}, {DSP_SETTING_SIDETONE,2,{1,11}},
        {DSP_SETTING_MIC_NOISE,2,{1,0}}, {DSP_SETTING_MIC_NOISE,2,{1,4}},
        {DSP_SETTING_ANC_STATE,1,{5}}, {DSP_SETTING_ANC_LEVEL,1,{4}},
        {DSP_SETTING_TRANSPARENCY,1,{11}}, {DSP_SETTING_BT_STARTUP,1,{1}},
        {DSP_SETTING_BT_CALL,3,{0,10,3}}, {DSP_SETTING_AUTO_OFF,1,{2}},
        {DSP_SETTING_OUTPUT_MODE,1,{0}}, {DSP_SETTING_OUTPUT_MODE,1,{3}},
        {DSP_SETTING_HOME_MODE,1,{2}}, {DSP_SETTING_HOME_MODE,2,{0,1}},
    };
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i)
        assert(!omni_dsp_settings_request(token+1,invalid[i].id,invalid[i].data,invalid[i].n,5));
}
static void home_mode_shared_opcode(void)
{
    uint8_t one=1;request(DSP_SETTING_HOME_MODE,&one,1,0);
    /* A volume client's identically framed DD/D2 cannot complete a queued
     * home-mode intent. Adapter arbitration grants only one active owner. */
    ack(0xd2,0,1);poll(1,false,16);assert(phase()==DSP_SETTINGS_QUEUED && !bus.used);
    poll(2,true,5);assert(phase()==DSP_SETTINGS_SEND && bus.used==5u);
    ack(0xd2,0,3);poll(3,false,1);assert(phase()==DSP_SETTINGS_WAIT && bus.used==6u);
    assert(!memcmp(bus.data,(uint8_t[]){0xbd,6,0xd2,9,1,1},6));
    poll(4,false,16);assert(phase()==DSP_SETTINGS_WAIT);
    ack(0xd2,0,5);poll(5,false,16);assert(phase()==DSP_SETTINGS_ACCEPTED);
    uint8_t out[60];assert(cached(DSP_SETTING_HOME_MODE,out)==3u && out[24]==1u);
    /* Above ACK is local dispatch only, including stock's masked remote
     * failure path. Only an independent B1 report marks observed context. */
    const uint8_t click[]={0xdb,6,0xd2,9,3,0};feed(click,6,6);feed(click,6,7);
    assert(cached(DSP_SETTING_HOME_MODE,out)==5u && out[24]==0u);
    uint32_t observed;memcpy(&observed,out+20,4);assert(observed==7u);
    request(DSP_SETTING_HOME_MODE,&one,1,10);poll(10,true,16);
    poll(510,false,16);assert(phase()==DSP_SETTINGS_TIMEOUT);
    ack(0xd2,0,511);poll(512,true,16);assert(phase()==DSP_SETTINGS_TIMEOUT && bus.used==6u);
}
static void faults_and_boundaries(void)
{
    const uint8_t v=6;
    request(DSP_SETTING_MIC_VOLUME,&v,1,0xfffffff0u);
    poll(1983,false,16);assert(phase()==DSP_SETTINGS_QUEUED);
    poll(1984,false,16);assert(phase()==DSP_SETTINGS_TIMEOUT && !omni_dsp_settings_transport_fault());
    request(DSP_SETTING_MIC_VOLUME,&v,1,0);poll(0,true,16);
    poll(500,false,16);assert(phase()==DSP_SETTINGS_TIMEOUT && !omni_dsp_settings_transport_fault());
    ack(0xd3,0,501);poll(502,false,16);assert(phase()==DSP_SETTINGS_TIMEOUT);
    request(DSP_SETTING_MIC_VOLUME,&v,1,0);poll(0,true,2);
    assert(phase()==DSP_SETTINGS_SEND);ack(0xd3,0,1); /* too early */
    poll(2,false,16);assert(phase()==DSP_SETTINGS_WAIT);
    poll(500,false,16);assert(phase()==DSP_SETTINGS_TIMEOUT);
    request(DSP_SETTING_MIC_VOLUME,&v,1,0);poll(0,true,2);poll(500,false,0);
    assert(phase()==DSP_SETTINGS_TIMEOUT && omni_dsp_settings_transport_fault());
    assert(!omni_dsp_settings_request(++token,DSP_SETTING_MIC_VOLUME,&v,1,501));
    request(DSP_SETTING_MIC_VOLUME,&v,1,0);poll(0,true,16);ack(0xd3,7,1);poll(1,false,16);
    assert(phase()==DSP_SETTINGS_NACK);
    request(DSP_SETTING_MIC_VOLUME,&v,1,0);poll(0,true,16);
    const uint8_t malformed[]={0xdd,2,0xd3,0};feed(malformed,4,1);poll(1,false,16);
    assert(phase()==DSP_SETTINGS_INVALID_REPLY);
    request(DSP_SETTING_MIC_VOLUME,&v,1,0);bus.drained=0;poll(0,true,16);
    ack(0xd3,0,1);poll(1,false,16);assert(phase()==DSP_SETTINGS_DRAIN);
    bus.drained=1;poll(2,false,16);assert(phase()==DSP_SETTINGS_ACCEPTED);
    request(DSP_SETTING_MIC_VOLUME,&v,1,0);poll(0,true,2);omni_dsp_settings_yield(1);
    poll(2,false,16);assert(bus.used==6 && phase()==DSP_SETTINGS_CANCELLED);
    request(DSP_SETTING_MIC_VOLUME,&v,1,0);bus.error=-1;poll(0,true,16);
    assert(phase()==DSP_SETTINGS_IO_ERROR && omni_dsp_settings_transport_fault());
    request(DSP_SETTING_MIC_VOLUME,&v,1,0);const uint8_t partial[]={0xdb,6,0xd3};
    feed(partial,3,0);assert(omni_dsp_settings_frame_pending());
    poll(1,true,16);assert(!bus.used);poll(20,true,16);assert(bus.used==6);
    /* An ACK begun before the last request byte is submitted stays ineligible. */
    request(DSP_SETTING_MIC_VOLUME,&v,1,0);poll(0,true,5);
    const uint8_t start[]={0xdd,3},end[]={0xd3,0};feed(start,2,1);
    poll(2,false,16);feed(end,2,3);poll(3,false,16);assert(phase()==DSP_SETTINGS_WAIT);
    ack(0xd3,0,4);poll(4,false,16);assert(phase()==DSP_SETTINGS_ACCEPTED);
}
static void output_mode_readback(void)
{
    uint8_t two=2;request(DSP_SETTING_OUTPUT_MODE,&two,1,0);poll(0,true,16);
    const uint8_t set[]={0xbd,5,0x43,1,2};assert(bus.used==5 && !memcmp(bus.data,set,5));
    ack(0x43,0,1);poll(1,false,16);assert(phase()==DSP_SETTINGS_VERIFY_QUEUED);
    poll(20,true,16);assert(bus.used==5);poll(549,true,16);assert(bus.used==5);
    poll(550,false,16);assert(bus.used==5);
    poll(551,true,16);const uint8_t get[]={0xbd,4,0x43,2};
    assert(bus.used==9 && !memcmp(bus.data+5,get,4));
    ack(0x43,0,552);poll(552,false,16);assert(phase()==DSP_SETTINGS_WAIT);
    const uint8_t reply[]={0xdb,5,0x43,3,2};feed(reply,5,553);poll(553,false,16);
    assert(phase()==DSP_SETTINGS_ACCEPTED);uint8_t out[60];assert(cached(DSP_SETTING_OUTPUT_MODE,out)==7u);
    request(DSP_SETTING_OUTPUT_MODE,&two,1,0);poll(0,true,16);ack(0x43,0,1);poll(1,false,16);
    poll(551,true,16);const uint8_t wrong[]={0xdb,5,0x43,3,1};feed(wrong,5,552);
    ack(0x43,0,552);poll(552,false,16);assert(phase()==DSP_SETTINGS_INVALID_REPLY);
    request(DSP_SETTING_OUTPUT_MODE,&two,1,0);poll(0,true,16);ack(0x43,0,1);poll(1,false,16);
    poll(500,false,16);assert(phase()==DSP_SETTINGS_VERIFY_QUEUED);
    poll(1250,false,16);assert(phase()==DSP_SETTINGS_TIMEOUT);
}
static void eq_and_cache(void)
{
    uint8_t value[128],out[60],unchanged[128];memset(unchanged,0xa5,sizeof(unchanged));
    for(unsigned control=DSP_SETTING_EQ_WIRELESS;control<=DSP_SETTING_EQ_BT;++control) {
        unsigned custom=control==DSP_SETTING_EQ_MIC?8u:4u;
        size_t n=omni_dsp_settings_eq_preset(control,custom,value);
        assert(n==(control==DSP_SETTING_EQ_WIRELESS?128u:78u));
        request(control,value,n,0);
        for(unsigned i=0;i<20u && phase()!=DSP_SETTINGS_WAIT;++i) poll(i,true,7);
        assert(phase()==DSP_SETTINGS_WAIT && bus.used==n+4u);
        assert(!memcmp(bus.data+4,value,n));ack((uint8_t)(0x1bu+2u*(control-DSP_SETTING_EQ_WIRELESS)),0,21);
        poll(21,false,16);assert(phase()==DSP_SETTINGS_ACCEPTED);
        for(unsigned page=0;page<4u;++page) {
            assert(omni_dsp_settings_value(control,page,out));
            unsigned offset=page*36u,count=offset<n?(unsigned)n-offset:0u;if(count>36u) count=36u;
            if(count) assert(!memcmp(out+24,value+offset,count));
        }
        assert(!omni_dsp_settings_request(++token,control,value,n-1u,22));
        value[68]=128; /* invalid int8 gain OR frequency; wireless also invalidatesQ below */
        if(control==DSP_SETTING_EQ_WIRELESS) value[72]=value[73]=0;
        assert(!omni_dsp_settings_request(++token,control,value,n,22));
        for(unsigned preset=0;preset<=9u;++preset) {
            size_t size=omni_dsp_settings_eq_preset(control,preset,value);
            bool known=(control==DSP_SETTING_EQ_MIC) || preset<=4u;
            assert((size!=0u)==known);
            if(size) { reset(30);assert(omni_dsp_settings_request(token,control,value,size,30)); }
        }
    }
    memcpy(value,unchanged,128);assert(!omni_dsp_settings_eq_preset(DSP_SETTING_EQ_WIRELESS,5,value));
    assert(!memcmp(value,unchanged,128));
    reset(0);
    const uint8_t mic[]={0xdb,6,0xd3,3,2,9};feed(mic,6,1);
    assert(cached(DSP_SETTING_MIC_VOLUME,out)==5u && out[24]==9);
    const uint8_t invalid[]={0xdb,6,0xd3,3,2,255};feed(invalid,6,2);
    assert(cached(DSP_SETTING_MIC_VOLUME,out)==5u && out[24]==9);
    const uint8_t mic_active[]={0xdb,6,0xd3,3,1,0};feed(mic_active,6,2);
    assert(cached(DSP_SETTING_MIC_STATE,out)==5u && out[24]==0);
    const uint8_t mic_muted[]={0xdb,6,0xd3,3,1,1};feed(mic_muted,6,2);
    assert(cached(DSP_SETTING_MIC_STATE,out)==5u && out[24]==1);
    assert(!omni_dsp_settings_valid(DSP_SETTING_MIC_STATE,mic_muted+5,1));
    assert(!omni_dsp_settings_request(++token,DSP_SETTING_MIC_STATE,mic_muted+5,1,2));
    for(unsigned raw=0x30;raw<=0x35;++raw) {
        const uint8_t bt[]={0xdb,5,0x14,3,(uint8_t)raw};feed(bt,5,2);
        assert(cached(DSP_SETTING_BT_STATE,out)==5u && out[24]==raw);
        assert(!omni_dsp_settings_valid(DSP_SETTING_BT_STATE,bt+4,1));
        assert(!omni_dsp_settings_request(++token,DSP_SETTING_BT_STATE,bt+4,1,2));
    }
    const uint8_t invalid_bt[]={0xdb,5,0x14,3,0x36,0xdb,6,0x14,3,0x30,0,0xdb,5,0x14,2,0x30};
    feed(invalid_bt,sizeof(invalid_bt),2);
    assert(cached(DSP_SETTING_BT_STATE,out)==5u && out[24]==0x35);
    uint8_t link;uint32_t ms;assert(!omni_dsp_settings_link_state(&link,&ms));
    const uint8_t connected[]={0xdb,5,0xe4,3,3};feed(connected,5,3);
    assert(omni_dsp_settings_link_state(&link,&ms) && link==3u && ms==3u);
    const uint8_t disconnected[]={0xdb,5,0xe4,3,1};feed(disconnected,5,4);
    assert(cached(DSP_SETTING_MIC_VOLUME,out)==0u);
    uint8_t bulk[46]={0xdb,46,0x20,1};bulk[5]=4;bulk[6]=7;bulk[15]=8;bulk[22]=30;bulk[45]=2;
    feed(bulk,46,5);assert(cached(DSP_SETTING_AUTO_OFF,out)==5u && out[24]==30);
    bulk[12]=1;bulk[13]=2;bulk[18]=1;bulk[20]=7;
    bulk[24]=2;bulk[25]=3;bulk[26]=9;bulk[27]=2;bulk[29]=1;
    bulk[34]=1;bulk[35]=6;feed(bulk,46,6);
    const unsigned ids[]={1,3,4,6,8,9,10,12,13,14};
    const uint8_t values[][3]={{1},{1,6},{1,2},{1},{1,7,2},{1,7,2},{1,7,2},{2},{9},{3}};
    const unsigned sizes[]={1,2,2,1,3,3,3,1,1,1};
    for(unsigned i=0;i<sizeof(ids)/sizeof(ids[0]);++i) {
        assert(cached(ids[i],out)==5u);
        assert(!memcmp(out+24,values[i],sizes[i]));
    }
    /* Invalid tuples must not replace a complete snapshot. */
    bulk[12]=255;bulk[20]=255;bulk[35]=255;bulk[24]=255;feed(bulk,46,7);
    assert(cached(DSP_SETTING_MIC_LED,out)==5u && out[25]==7);
    assert(cached(DSP_SETTING_SIDETONE,out)==5u && out[25]==6);
    assert(cached(DSP_SETTING_EQ_WIRELESS,out)==5u && out[24]==2);
    const uint8_t siblings[]={0,4,1};request(DSP_SETTING_MIC_LED,siblings,3,8);
    poll(8,true,16);ack(0xe3,0,9);poll(9,false,16);
    assert(phase()==DSP_SETTINGS_ACCEPTED);
    assert(cached(DSP_SETTING_BT_CALL,out)&1u);
    assert(!memcmp(out+24,siblings,3));
    assert(!omni_dsp_settings_value(0,0,out));assert(!omni_dsp_settings_value(1,4,out));
    assert(!omni_dsp_settings_status(2,(uint32_t *)(void *)unchanged));
}
int main(void)
{
    scalar_packets();validation_and_coalescing();faults_and_boundaries();
    output_mode_readback();eq_and_cache();home_mode_shared_opcode();
    puts("DSP settings payloads, scheduler, faults, mode readback and EQ contracts passed");
    return 0;
}
