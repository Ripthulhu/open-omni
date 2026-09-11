#include "dsp_volume.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t tx[32],rx[1024]; unsigned sent,used,read,tx_calls,rx_calls,drain_calls;
    int tx_result,rx_result,drain_result;
    unsigned tx_allow;
} fake;
static int tx(void *ctx,uint8_t b)
{
    fake *f=ctx; ++f->tx_calls;
    if(f->tx_result!=1) return f->tx_result;
    if(f->sent>=f->tx_allow) return 0;
    assert(f->sent<sizeof(f->tx)); f->tx[f->sent++]=b; return 1;
}
static int rx(void *ctx,uint8_t *b)
{
    fake *f=ctx; ++f->rx_calls;
    if(f->rx_result!=1) return f->rx_result;
    if(f->read==f->used) return 0;
    *b=f->rx[f->read++]; return 1;
}
static int drain(void *ctx)
{ fake *f=ctx; ++f->drain_calls; return f->drain_result; }
static void setup(omni_dsp_volume *v,fake *f,uint32_t now)
{
    *f=(fake){.tx_result=1,.rx_result=1,.drain_result=1,.tx_allow=32};
    assert(omni_dsp_volume_init(v,(omni_dsp_volume_io){f,tx,rx,drain}));
    assert(omni_dsp_volume_begin(v,now));
}
static void push(fake *f,const uint8_t *p,unsigned n)
{ assert(f->used+n<=sizeof(f->rx)); memcpy(f->rx+f->used,p,n); f->used+=n; }
static const uint8_t mode[]={0xdb,5,0x43,3,2,0xdd,3,0x43,0};
static const uint8_t levels[]={0xdb,8,0x47,3,35,36,40,45,0xdd,3,0x47,0};
static void good(uint32_t start)
{
    omni_dsp_volume v; fake f; setup(&v,&f,start);
    assert(v.desired_muted && v.desired_db==-30*256);
    assert(omni_dsp_volume_desire(&v,-20*256,true));
    assert(omni_dsp_volume_desire(&v,-20*256,false));
    assert(v.desired_revision==2 && v.desired_db==-20*256);
    assert(omni_dsp_volume_desire(&v,-20*256,false) && v.desired_revision==2);
    assert(!omni_dsp_volume_desire(&v,1,false));
    assert(!omni_dsp_volume_desire(&v,-60*256-1,false));
    push(&f,mode,sizeof(mode)); omni_dsp_volume_poll(&v,start);
    assert(v.mode_valid && v.mode==2 && v.got_ack && f.sent==4);
    omni_dsp_volume_poll(&v,start+1u); assert(v.phase==OMNI_DSP_VOLUME_COOLDOWN);
    omni_dsp_volume_poll(&v,start+20u); assert(f.sent==4);
    omni_dsp_volume_poll(&v,start+21u); assert(v.phase==OMNI_DSP_VOLUME_SEND);
    push(&f,levels,sizeof(levels)); omni_dsp_volume_poll(&v,start+22u);
    omni_dsp_volume_poll(&v,start+23u); omni_dsp_volume_poll(&v,start+43u);
    assert(v.phase==OMNI_DSP_VOLUME_COMPLETE && v.error==OMNI_DSP_VOLUME_OK);
    assert(v.levels_valid && !memcmp(v.levels,levels+4,4));
    assert(v.mode_received_ms==start && v.levels_received_ms==start+22u);
    assert(v.ack_frames==2 && v.tx_bytes==8);
    assert(!memcmp(f.tx,(uint8_t[]){0xbd,4,0x43,2,0xbd,4,0x47,2},8));
    assert(!omni_dsp_volume_begin(&v,start+44u));
}
static void failures(void)
{
    omni_dsp_volume v; fake f;
    setup(&v,&f,0); push(&f,mode,5); /* Matching data without generic reply. */
    omni_dsp_volume_poll(&v,0); omni_dsp_volume_poll(&v,1);
    omni_dsp_volume_poll(&v,250); assert(v.error==OMNI_DSP_VOLUME_TIMEOUT);
    unsigned count=f.tx_calls+f.rx_calls+f.drain_calls;
    omni_dsp_volume_poll(&v,999); assert(count==f.tx_calls+f.rx_calls+f.drain_calls);
    setup(&v,&f,0); uint8_t bad[9]; memcpy(bad,mode,9); bad[8]=2;
    push(&f,bad,9); omni_dsp_volume_poll(&v,0); assert(v.error==OMNI_DSP_VOLUME_PEER_ERROR);
    setup(&v,&f,0); memcpy(bad,mode,9); bad[4]=3;
    push(&f,bad,9); omni_dsp_volume_poll(&v,0); assert(v.error==OMNI_DSP_VOLUME_BAD_REPLY);
    setup(&v,&f,0); f.tx_result=-1; omni_dsp_volume_poll(&v,0);
    assert(v.error==OMNI_DSP_VOLUME_IO_ERROR && !f.sent);
    setup(&v,&f,0); f.tx_result=2; omni_dsp_volume_poll(&v,0);
    assert(v.error==OMNI_DSP_VOLUME_IO_ERROR);
    setup(&v,&f,0); f.rx_result=-1; omni_dsp_volume_poll(&v,0);
    assert(v.error==OMNI_DSP_VOLUME_IO_ERROR);
    setup(&v,&f,0); f.drain_result=-1; omni_dsp_volume_poll(&v,0);
    omni_dsp_volume_poll(&v,1); assert(v.error==OMNI_DSP_VOLUME_IO_ERROR);
    setup(&v,&f,0); f.drain_result=0; push(&f,mode,9);
    omni_dsp_volume_poll(&v,0); omni_dsp_volume_poll(&v,249);
    assert(v.phase==OMNI_DSP_VOLUME_DRAIN && f.sent==4);
    omni_dsp_volume_poll(&v,250); assert(v.error==OMNI_DSP_VOLUME_TIMEOUT);
    setup(&v,&f,0); f.tx_allow=2; omni_dsp_volume_poll(&v,0);
    assert(v.tx_offset==2 && f.sent==2); f.tx_allow=32;
    omni_dsp_volume_poll(&v,1); assert(f.sent==4 && f.tx[2]==0x43 && f.tx[3]==2);
    omni_dsp_volume_cancel(&v); count=f.rx_calls; omni_dsp_volume_poll(&v,2);
    assert(v.phase==OMNI_DSP_VOLUME_CANCELED && count==f.rx_calls);
}
static void parser_limits(void)
{
    omni_dsp_volume v; fake f; setup(&v,&f,0);
    memset(f.rx,0x80,100); f.used=100; omni_dsp_volume_poll(&v,0);
    assert(f.rx_calls==32 && v.rx_bytes==32 && f.tx_calls==4);
    f.read=f.used; push(&f,(uint8_t[]){0xdb,2,0xdb,5,0x43,2,2},7);
    omni_dsp_volume_poll(&v,1); assert(!v.mode_valid && v.parser.malformed==1);
    push(&f,(uint8_t[]){0xdb,5,0x43},3); omni_dsp_volume_poll(&v,2);
    omni_dsp_volume_poll(&v,22); assert(v.parser.expired==1);
    push(&f,mode,9); omni_dsp_volume_poll(&v,23); assert(v.mode_valid);
    omni_dsp_volume_poll(&v,24); /* second query gets transmitted */
    uint8_t bad[12]; memcpy(bad,levels,12); bad[6]=101; push(&f,bad,12);
    omni_dsp_volume_poll(&v,25); assert(v.error==OMNI_DSP_VOLUME_BAD_REPLY);
    setup(&v,&f,0); f.tx_allow=0; push(&f,mode,9);
    omni_dsp_volume_poll(&v,0); assert(!v.mode_valid && !v.got_ack && v.unexpected_frames==2);
}
int main(void)
{
    good(0); good(UINT32_MAX-25u); failures(); parser_limits();
    puts("DSP volume read-only discovery: all tests passed"); return 0;
}
