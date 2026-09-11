#include "dsp_volume.h"
#include <string.h>

static bool running(const omni_dsp_volume *v)
{ return v->phase>=OMNI_DSP_VOLUME_SEND && v->phase<=OMNI_DSP_VOLUME_COOLDOWN; }
static void fail(omni_dsp_volume *v, omni_dsp_volume_error why)
{ v->error=why; v->phase=OMNI_DSP_VOLUME_FAILED; }
static uint8_t opcode(const omni_dsp_volume *v)
{ return v->query_index ? 0x47u : 0x43u; }

bool omni_dsp_volume_init(omni_dsp_volume *v, omni_dsp_volume_io io)
{
    if(!v || !io.tx || !io.rx || !io.tx_complete) return false;
    memset(v,0,sizeof(*v)); v->io=io; v->desired_db=-30*256;
    v->desired_muted=1u; v->initialized=1u;
    return omni_link_init(&v->parser,20u);
}
bool omni_dsp_volume_begin(omni_dsp_volume *v,uint32_t now)
{
    if(!v || !v->initialized || v->phase!=OMNI_DSP_VOLUME_IDLE) return false;
    v->phase=OMNI_DSP_VOLUME_SEND; v->started_ms=v->query_ms=now;
    return true;
}
bool omni_dsp_volume_desire(omni_dsp_volume *v,int16_t db,bool muted)
{
    if(!v || !v->initialized || db < -60*256 || db>0) return false;
    if(v->desired_db!=db || v->desired_muted!=(uint8_t)muted) {
        v->desired_db=db; v->desired_muted=(uint8_t)muted;
        ++v->desired_revision;
    }
    return true;
}
void omni_dsp_volume_cancel(omni_dsp_volume *v)
{ if(v && running(v)) v->phase=OMNI_DSP_VOLUME_CANCELED; }

static void receive(omni_dsp_volume *v,uint32_t now)
{
    omni_link_expire(&v->parser,now);
    for(unsigned i=0;i<OMNI_DSP_VOLUME_RX_BUDGET;++i) {
        uint8_t b; int result=v->io.rx(v->io.context,&b);
        if(!result) return;
        if(result!=1) { fail(v,OMNI_DSP_VOLUME_IO_ERROR); return; }
        ++v->rx_bytes;
        const uint8_t *p; size_t n;
        uint32_t ignored=v->parser.ignored;
        bool complete=omni_link_feed(&v->parser,b,now,&p,&n);
        /* The shared framing parser deliberately hides DD. Its public buffer
         * still contains the four completed bytes when ignored increments. */
        if(v->parser.ignored!=ignored) {
            p=v->parser.bytes;
            if(v->tx_offset==4u && p[0]==0xddu && p[1]==3u && p[2]==opcode(v)) {
                ++v->ack_frames; v->peer_status=p[3];
                if(p[3]) { fail(v,OMNI_DSP_VOLUME_PEER_ERROR); return; }
                v->got_ack=1u;
            } else ++v->unexpected_frames;
        }
        if(!complete) continue;
        if(v->tx_offset!=4u || n<4u || p[0]!=0xdbu || p[2]!=opcode(v) || p[3]!=3u) {
            ++v->unexpected_frames; continue;
        }
        if(!v->query_index) {
            if(n!=5u || p[1]!=5u || p[4]>2u) { fail(v,OMNI_DSP_VOLUME_BAD_REPLY); return; }
            v->mode=p[4]; v->mode_valid=1u; v->mode_received_ms=now;
        } else {
            if(n!=8u || p[1]!=8u || p[4]>100u || p[5]>100u || p[6]>100u || p[7]>100u) {
                fail(v,OMNI_DSP_VOLUME_BAD_REPLY); return;
            }
            memcpy(v->levels,p+4,4u); v->levels_valid=1u; v->levels_received_ms=now;
        }
        v->got_reply=1u;
    }
}
void omni_dsp_volume_poll(omni_dsp_volume *v,uint32_t now)
{
    if(!v || !running(v)) return;
    if(now-v->started_ms>=OMNI_DSP_VOLUME_TIMEOUT_MS || now-v->query_ms>=OMNI_DSP_VOLUME_QUERY_MS) {
        fail(v,OMNI_DSP_VOLUME_TIMEOUT); return;
    }
    if(v->phase==OMNI_DSP_VOLUME_SEND) {
        uint8_t query[4]={0xbdu,4u,opcode(v),2u};
        /* At most four TX callbacks, with no automatic retries of a failed
         * partial frame. Zero preserves the exact next unsent byte. */
        for(unsigned i=0;i<4u && v->tx_offset<4u;++i) {
            int result=v->io.tx(v->io.context,query[v->tx_offset]);
            if(!result) break;
            if(result!=1) { fail(v,OMNI_DSP_VOLUME_IO_ERROR); return; }
            ++v->tx_offset; ++v->tx_bytes;
        }
        if(v->tx_offset==4u) v->phase=OMNI_DSP_VOLUME_DRAIN;
    } else if(v->phase==OMNI_DSP_VOLUME_DRAIN) {
        int result=v->io.tx_complete(v->io.context);
        if(result!=0 && result!=1) { fail(v,OMNI_DSP_VOLUME_IO_ERROR); return; }
        if(result==1) { v->drained_ms=now; v->phase=OMNI_DSP_VOLUME_WAIT; }
    }
    receive(v,now);
    if(!running(v)) return;
    if(v->phase==OMNI_DSP_VOLUME_WAIT && v->got_ack && v->got_reply)
        v->phase=OMNI_DSP_VOLUME_COOLDOWN;
    if(v->phase==OMNI_DSP_VOLUME_COOLDOWN && now-v->drained_ms>=OMNI_DSP_VOLUME_GAP_MS) {
        if(v->query_index) { v->phase=OMNI_DSP_VOLUME_COMPLETE; return; }
        v->query_index=1u; v->tx_offset=v->got_ack=v->got_reply=0u;
        v->query_ms=now; v->phase=OMNI_DSP_VOLUME_SEND;
    }
}
