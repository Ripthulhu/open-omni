#include "dsp_gain_trial.h"
#include <string.h>

bool omni_gain_trial_busy(const omni_gain_trial *v)
{ return v && v->phase>=GAIN_SEND && v->phase<=GAIN_HOLD; }
static void fail(omni_gain_trial *v, omni_gain_error why)
{ v->error=why; v->phase=GAIN_FAILED; }
static bool setting(const omni_gain_trial *v) { return v->step==2u || v->step==6u; }
static void prepare(omni_gain_trial *v,uint32_t now)
{
    uint8_t *p=v->tx_frame[v->step];
    bool set=setting(v);
    p[0]=0xbdu; p[1]=set?8u:4u;
    p[2]=(v->step==0u || v->step==4u)?0x43u:0x47u;
    p[3]=set?1u:2u;
    if(set) memcpy(p+4,v->step==2u?v->reduced:v->original,4);
    v->tx_length[v->step]=p[1];
    v->offset=v->ack=v->reply=0;
    v->request_ms=now; v->phase=GAIN_SEND;
}
bool omni_gain_trial_begin(omni_gain_trial *v,omni_dsp_volume_io io,uint32_t now)
{
    if(!v || v->phase!=GAIN_IDLE || !io.tx || !io.rx || !io.tx_complete) return false;
    memset(v,0,sizeof(*v)); v->io=io; v->batch_ms=now;
    (void)omni_link_init(&v->parser,20u); prepare(v,now); return true;
}
void omni_gain_trial_cancel(omni_gain_trial *v)
{ if(omni_gain_trial_busy(v)) v->phase=GAIN_CANCELED; }

bool omni_gain_apply_begin(omni_gain_trial *v,omni_dsp_volume_io io,uint32_t now,uint8_t target)
{
    if(target>100u || !omni_gain_trial_begin(v,io,now)) return false;
    v->apply_only=true; v->target=target; v->target_mask=1u; v->targets[0]=target; return true;
}

bool omni_gain_apply_masked_begin(omni_gain_trial *v,omni_dsp_volume_io io,uint32_t now,uint8_t mask,const uint8_t targets[4])
{
    if(!targets || !mask || (mask&~5u)) return false;
    for(unsigned i=0;i<4;i++) if((mask&(1u<<i)) && targets[i]>100u) return false;
    if(!omni_gain_apply_begin(v,io,now,targets[0])) return false;
    v->target_mask=mask;memcpy(v->targets,targets,4);return true;
}

bool omni_gain_restore_begin(omni_gain_trial *v,omni_dsp_volume_io io,uint32_t now,
                            const uint8_t expected[4],const uint8_t original[4])
{
    if(!expected || !original || expected[0]==0u ||
       (unsigned)expected[0]+25u!=original[0] || memcmp(expected+1,original+1,3)) return false;
    for(unsigned i=0;i<4u;++i) if(expected[i]>100u || original[i]>100u) return false;
    if(!omni_gain_trial_begin(v,io,now)) return false;
    memcpy(v->original,original,4); memcpy(v->reduced,expected,4);
    v->step=4u; prepare(v,now); return true;
}

static void receive(omni_gain_trial *v,uint32_t now)
{
    uint32_t malformed=v->parser.malformed, expired=v->parser.expired;
    omni_link_expire(&v->parser,now);
    if(v->parser.expired!=expired) { fail(v,GAIN_FRAME); return; }
    for(unsigned i=0;i<32u;++i) {
        uint8_t b; int r=v->io.rx(v->io.context,&b);
        if(!r) return;
        if(r!=1) { fail(v,GAIN_IO); return; }
        ++v->rx_bytes;
        uint32_t ignored=v->parser.ignored;
        const uint8_t *p; size_t n;
        bool complete=omni_link_feed(&v->parser,b,now,&p,&n);
        if(v->parser.malformed!=malformed || v->parser.expired!=expired) {
            fail(v,GAIN_FRAME); return;
        }
        bool pending=v->phase!=GAIN_HOLD && v->offset==v->tx_length[v->step];
        if(v->parser.ignored!=ignored) {
            p=v->parser.bytes;
            if(pending && p[2]==v->tx_frame[v->step][2]) {
                memcpy(v->ack_frame[v->step],p,4);
                if(p[1]!=3u) { fail(v,GAIN_FRAME); return; }
                v->peer_status=p[3];
                if(p[3]) { fail(v,GAIN_PEER); return; }
                v->ack=1;
            } else ++v->unrelated;
        }
        if(!complete) continue;
        if(!pending || p[0]!=0xdbu || n<3u || p[2]!=v->tx_frame[v->step][2]) {
            ++v->unrelated; continue;
        }
        if(setting(v)) { ++v->unrelated; continue; }
        if(n<4u || p[3]!=3u || n>8u) { fail(v,GAIN_FRAME); return; }
        memcpy(v->rx_frame[v->step],p,n); v->rx_length[v->step]=(uint8_t)n;
        if(p[2]==0x43u) {
            if(n!=5u) { fail(v,GAIN_FRAME); return; }
            v->mode=p[4];
            if(v->mode!=2u) { fail(v,GAIN_MODE); return; }
        } else {
            if(n!=8u) { fail(v,GAIN_FRAME); return; }
            for(unsigned j=4;j<8u;++j) if(p[j]>100u) { fail(v,GAIN_LEVEL); return; }
            memcpy(v->observed,p+4,4);
        }
        v->reply=1;
    }
}

static void advance(omni_gain_trial *v,uint32_t now)
{
    if(v->step==1u) {
        memcpy(v->original,v->observed,4); memcpy(v->reduced,v->observed,4);
        /* Fixed downward wire step; factory curve is monotonic. Do not cross
         * its special zero endpoint, or turn a quiet setting into an increase. */
        if(v->apply_only) {
            for(unsigned i=0;i<4;i++) if(v->target_mask&(1u<<i)) v->reduced[i]=v->targets[i];
            if(!memcmp(v->original,v->reduced,4)) {
                v->attenuated_verified=true; v->phase=GAIN_DONE; return;
            }
        } else {
            if(v->original[0]<=25u) { fail(v,GAIN_LEVEL); return; }
            v->reduced[0]=(uint8_t)(v->original[0]-25u);
        }
    } else if(v->step==3u || v->step==7u) {
        const uint8_t *expected=v->step==3u?v->reduced:v->original;
        if(memcmp(v->observed,expected,4)) {
            /* Allow bounded settling observations, but never repeat a SET. */
            if(++v->attempts>=3u) { fail(v,GAIN_READBACK); return; }
            prepare(v,now); return;
        }
        if(v->step==7u) { v->restored_verified=true; v->phase=GAIN_DONE; return; }
        v->attenuated_verified=true; v->hold_ms=now;
        v->phase=v->apply_only?GAIN_DONE:GAIN_HOLD; return;
    } else if(v->step==5u && memcmp(v->observed,v->reduced,4)) {
        /* Another writer changed a lane during the listening interval. Do not
         * overwrite it with the saved tuple or claim a successful restore. */
        fail(v,GAIN_CHANGED); return;
    }
    ++v->step; v->attempts=0; prepare(v,now);
}
void omni_gain_trial_poll(omni_gain_trial *v,uint32_t now)
{
    if(!omni_gain_trial_busy(v)) return;
    if(v->phase==GAIN_HOLD) {
        receive(v,now);
        if(v->phase==GAIN_HOLD && (uint32_t)(now-v->hold_ms)>=10000u) {
            v->step=4u; v->batch_ms=now; v->attempts=0; prepare(v,now);
        }
        return;
    }
    bool settling=v->phase==GAIN_GAP && setting(v);
    if((uint32_t)(now-v->batch_ms)>=750u ||
       (!settling && (uint32_t)(now-v->request_ms)>=250u)) {
        fail(v,GAIN_TIMEOUT); return;
    }
    if(v->phase==GAIN_SEND) {
        for(unsigned i=0;i<8u && v->offset<v->tx_length[v->step];++i) {
            int r=v->io.tx(v->io.context,v->tx_frame[v->step][v->offset]);
            if(!r) break;
            if(r!=1) { fail(v,GAIN_IO); return; }
            ++v->offset; ++v->tx_bytes;
            if(setting(v) && v->offset==8u) v->set_may_have_applied|=1u<<v->step;
        }
        if(v->offset==v->tx_length[v->step]) v->phase=GAIN_DRAIN;
    } else if(v->phase==GAIN_DRAIN) {
        int r=v->io.tx_complete(v->io.context);
        if(r!=0 && r!=1) { fail(v,GAIN_IO); return; }
        if(r==1) { v->drained_ms=now; v->phase=GAIN_WAIT; }
    }
    receive(v,now);
    if(v->phase==GAIN_WAIT && v->ack && (setting(v)||v->reply)) v->phase=GAIN_GAP;
    /* Stock SET47 posts event 0x37 with due_time=clock_now+200, not an
     * immediate state update. Allow 250ms after physical TX before GET.
     * Generic acceptance remains distinct from verified application. */
    uint32_t gap=setting(v)?250u:20u;
    if(v->phase==GAIN_GAP && (uint32_t)(now-v->drained_ms)>=gap) advance(v,now);
}
