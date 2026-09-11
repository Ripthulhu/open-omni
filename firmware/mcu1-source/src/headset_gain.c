#include "headset_gain.h"
#include <string.h>

bool omni_headset_gain_init(omni_headset_gain *v)
{
    if(!v) return false;
    memset(v,0,sizeof(*v));(void)omni_link_init(&v->parser,20u);
    v->initialized=true;v->peer_status=255u;return true;
}
bool omni_headset_gain_busy(const omni_headset_gain *v)
{ return v && v->phase>=HEADSET_GAIN_QUEUED && v->phase<=HEADSET_GAIN_GAP; }
bool omni_headset_gain_active(const omni_headset_gain *v)
{ return v && v->phase>=HEADSET_GAIN_SEND && v->phase<=HEADSET_GAIN_GAP; }
bool omni_headset_gain_primed(const omni_headset_gain *v) { return v && v->primed; }
bool omni_headset_gain_ready(const omni_headset_gain *v)
{ return v && v->online && v->have_verified && !v->blocked && !v->fault && (!v->have_link || v->connected); }
bool omni_headset_gain_settled(const omni_headset_gain *v)
{ return omni_headset_gain_ready(v) && v->desired_valid && v->desired==v->verified && !omni_headset_gain_busy(v); }
bool omni_headset_gain_frame_pending(const omni_headset_gain *v) { return v && v->parser.used; }

static void queue(omni_headset_gain *v,uint32_t now)
{
    if(!v->online || !v->desired_valid || v->blocked || v->fault || v->cancel ||
       omni_headset_gain_busy(v) || v->phase==HEADSET_GAIN_DEFERRED || (v->have_link && !v->connected)) return;
    if(v->primed && v->have_verified && v->desired==v->verified) {
        v->verified_revision=v->revision;return;
    }
    v->operation=v->primed?1u:0u;v->queued_ms=now;v->phase=HEADSET_GAIN_QUEUED;
}
bool omni_headset_gain_desire(omni_headset_gain *v,uint8_t ll,uint32_t revision,uint32_t now)
{
    if(!v || !v->initialized || ll>56u) return false;
    bool changed=v->desired_valid && (v->desired!=ll || v->revision!=revision);
    if(v->desired_valid && v->revision!=revision) {
        v->conflict_attempts=0;
        /* A fresh explicit master revision may retry a semantic conflict,
         * never a timeout, NACK, malformed frame or transport failure. */
        if(v->phase==HEADSET_GAIN_CONFLICT && !v->fault) v->blocked=false;
    }
    if(v->desired_valid && v->desired!=ll && omni_headset_gain_busy(v)) ++v->coalesced;
    if(changed && v->phase==HEADSET_GAIN_QUEUED) v->queued_ms=now;
    if(changed && v->phase==HEADSET_GAIN_DEFERRED) v->phase=HEADSET_GAIN_IDLE;
    v->desired=ll;v->revision=revision;v->desired_valid=true;queue(v,now);return true;
}
static void finish(omni_headset_gain *v,omni_headset_gain_phase phase,uint32_t now)
{
    v->phase=phase;v->finished_ms=now;
    if(phase!=HEADSET_GAIN_DONE && phase!=HEADSET_GAIN_CANCELLED) {
        v->blocked=true;v->have_verified=false;
    }
    if(phase==HEADSET_GAIN_CANCELLED && !v->yielding && v->connected) {
        v->cancel=false;queue(v,now);
    }
}
void omni_headset_gain_io_error(omni_headset_gain *v,uint32_t now)
{
    if(!v) return;
    v->fault=true;v->primed=false;v->remote_pending=false;
    finish(v,HEADSET_GAIN_IO_ERROR,now);
}
static void prepare(omni_headset_gain *v,uint32_t now,unsigned operation)
{
    v->operation=(uint8_t)operation;v->offset=0;v->peer_status=255u;
    v->ack=v->negative=v->bad_reply=v->matched=false;
    v->readback=v->submitted;v->readback_conflict=false;
    v->frame[0]=0xbd;v->frame[1]=operation==0u?4u:5u;
    v->frame[2]=operation==0u?0x20u:0xd2u;
    v->frame[3]=operation==2u?2u:1u;v->frame[4]=operation==1u?v->submitted:0u;
    v->length=v->frame[1];v->phase=HEADSET_GAIN_SEND;
    if(operation!=2u) v->started_ms=now;
}
static bool recent_echo(const omni_headset_gain *v,uint8_t ll,uint32_t now)
{
    for(unsigned i=0;i<v->history_count;++i)
        if(v->history[i]==ll && (uint32_t)(now-v->history_ms[i])<OMNI_HEADSET_GAIN_ECHO_MS) return true;
    return false;
}
static void report(omni_headset_gain *v,const uint8_t *p,size_t n,uint32_t now)
{
    if(p[0]!=0xdbu) return;
    if(n==5u && p[2]==0xe4u && p[3]==3u && p[4]>=1u && p[4]<=3u) {
        bool first=!v->have_link,next=p[4]==3u,changed=v->have_link && next!=v->connected;
        v->have_link=true;v->connected=next;
        if(!next || changed) {
            v->primed=v->have_verified=v->have_seen=v->remote_pending=false;
            v->history_count=v->history_cursor=0;
            if(omni_headset_gain_active(v)) v->cancel=true;
            else {v->phase=HEADSET_GAIN_IDLE;v->cancel=false;}
        }
        if(next && !v->fault && (first || changed)) {
            v->blocked=false;v->primed=v->have_verified=false;
            v->conflict_attempts=0;
        }
        queue(v,now);return;
    }
    if(n==46u && p[2]==0x20u && p[3]==1u && p[4]<=56u && p[45]<=2u &&
       (!v->have_link || v->connected)) {
        /* The original bulk constructor enables subsequent dedicated D202. */
        v->primed=true;
        if(v->operation==0u && omni_headset_gain_active(v) && v->eligible) v->matched=true;
        return;
    }
    if(p[2]!=0xd2u || n<4u || p[3]!=3u) return;
    if(n!=6u || p[4]>2u || p[5]>56u) {
        ++v->invalid;
        if(v->operation==2u && omni_headset_gain_active(v) && v->eligible) v->bad_reply=true;
        return;
    }
    if(p[4]!=0u) {++v->unrelated;return;}
    bool duplicate=v->have_seen && v->seen==p[5];
    v->seen=p[5];v->have_seen=true;v->observed_ms=now;
    bool getter=v->operation==2u && omni_headset_gain_active(v) && v->eligible;
    if(getter) {
        /* GET and physical tag0 reports have identical wire framing. A
         * different valid LL is legal when the headset dial races this SET. */
        if(p[5]!=v->submitted && !v->readback_conflict) ++v->conflicts;
        v->readback=p[5];v->readback_conflict=p[5]!=v->submitted;v->matched=true;
        if(p[5]==v->submitted || v->frame_revision!=v->submitted_revision) return;
    }
    if(!omni_headset_gain_ready(v) || v->cancel) return;
    if(duplicate || p[5]==v->verified ||
       (omni_headset_gain_active(v) && p[5]==v->submitted) || recent_echo(v,p[5],now)) {
        ++v->echoes;return;
    }
    v->remote=p[5];v->remote_revision=v->frame_revision;v->remote_ms=now;
    v->remote_pending=true;++v->remote_events;
}
void omni_headset_gain_byte(omni_headset_gain *v,uint8_t byte,uint32_t now,uint32_t revision)
{
    if(!v || !v->initialized || !v->online) return;
    ++v->rx_bytes;
    uint32_t bad=v->parser.malformed+v->parser.expired;
    omni_link_expire(&v->parser,now);
    if(!v->parser.used) {
        v->frame_revision=revision;
        v->eligible=omni_headset_gain_active(v) && v->phase!=HEADSET_GAIN_GAP && v->offset==v->length;
    }
    uint32_t ignored=v->parser.ignored;
    const uint8_t *p;size_t n;
    bool complete=omni_link_feed(&v->parser,byte,now,&p,&n);
    if(v->parser.malformed+v->parser.expired!=bad && omni_headset_gain_active(v)) {
        ++v->invalid;v->bad_reply=true;
    }
    if(v->parser.ignored!=ignored) {
        p=v->parser.bytes;
        if(omni_headset_gain_active(v) && v->eligible && p[2]==v->frame[2]) {
            if(p[1]!=3u) {++v->invalid;v->bad_reply=true;}
            else {
                ++v->acks;v->peer_status=p[3];
                if(p[3]) {++v->nacks;v->negative=true;} else v->ack=true;
            }
        } else ++v->unrelated;
    }
    if(complete) report(v,p,n,now);
}
void omni_headset_gain_poll(omni_headset_gain *v,uint32_t now,bool can_start,bool can_apply,omni_headset_gain_io io)
{
    if(!v || !v->initialized || !v->online) return;
    uint32_t expired=v->parser.expired;omni_link_expire(&v->parser,now);
    if(expired!=v->parser.expired && omni_headset_gain_active(v)) {++v->invalid;v->bad_reply=true;}
    if(v->phase==HEADSET_GAIN_DEFERRED) {
        /* No bytes were submitted and no owner was acquired. The latest
         * intent can wait safely without invalidating the last valid gain. */
        if(!can_start || (v->operation==1u && !can_apply) || v->parser.used ||
           v->cancel || v->fault || v->blocked || (v->have_link && !v->connected)) return;
        v->phase=HEADSET_GAIN_IDLE;
    }
    queue(v,now);
    if(!omni_headset_gain_busy(v)) return;
    if(v->phase==HEADSET_GAIN_QUEUED) {
        if(v->primed && v->operation==0u) v->operation=1u;
        if((uint32_t)(now-v->queued_ms)>=OMNI_HEADSET_GAIN_QUEUE_MS) {
            ++v->queue_deferrals;v->phase=HEADSET_GAIN_DEFERRED;v->finished_ms=now;return;
        }
        if(!can_start || v->parser.used || (v->operation==1u && !can_apply)) return;
        v->submitted=v->desired;v->submitted_revision=v->revision;
        prepare(v,now,v->operation);
    }
    if(!io.tx || !io.drained) {omni_headset_gain_io_error(v,now);return;}
    if((uint32_t)(now-v->started_ms)>=OMNI_HEADSET_GAIN_TIMEOUT_MS) {
        ++v->timeouts;
        if(v->phase==HEADSET_GAIN_SEND || v->phase==HEADSET_GAIN_DRAIN) v->fault=true;
        finish(v,HEADSET_GAIN_TIMEOUT,now);return;
    }
    if(v->phase==HEADSET_GAIN_GAP) {
        if(v->cancel) {finish(v,HEADSET_GAIN_CANCELLED,now);return;}
        if(!can_start || v->parser.used || (uint32_t)(now-v->drained_ms)<OMNI_HEADSET_GAIN_GAP_MS) return;
        prepare(v,now,2u);
    }
    if(v->phase==HEADSET_GAIN_SEND) {
        for(unsigned budget=0;budget<5u && v->offset<v->length;++budget) {
            int r=io.tx(io.context,v->frame[v->offset]);
            if(!r) break;
            if(r!=1) {omni_headset_gain_io_error(v,now);return;}
            ++v->offset;++v->tx_bytes;
        }
        if(v->offset==v->length) v->phase=HEADSET_GAIN_DRAIN;
    }
    if(v->phase==HEADSET_GAIN_DRAIN) {
        int r=io.drained(io.context);
        if(r<0 || r>1) {omni_headset_gain_io_error(v,now);return;}
        if(r==1) {v->drained_ms=now;v->phase=HEADSET_GAIN_WAIT;}
    }
    if(v->phase!=HEADSET_GAIN_WAIT) return;
    if(v->cancel) {finish(v,HEADSET_GAIN_CANCELLED,now);return;}
    if(v->negative) {finish(v,HEADSET_GAIN_NACK,now);return;}
    if(v->bad_reply) {finish(v,HEADSET_GAIN_INVALID_REPLY,now);return;}
    if(v->operation==0u && v->matched) {
        v->primed=true;v->phase=HEADSET_GAIN_IDLE;queue(v,now);
    } else if(v->operation==1u && v->ack) {
        unsigned i=v->history_cursor;
        v->history[i]=v->submitted;v->history_ms[i]=now;
        v->history_cursor=(uint8_t)((i+1u)%4u);if(v->history_count<4u) ++v->history_count;
        v->phase=HEADSET_GAIN_GAP;
    } else if(v->operation==2u && v->matched) {
        v->verified=v->readback;
        v->verified_revision=v->readback==v->desired?v->revision:v->submitted_revision;
        if(v->readback==v->submitted || v->readback==v->desired) v->have_verified=true;
        if(v->readback!=v->submitted && v->readback!=v->desired && v->revision==v->submitted_revision) {
            if(v->conflict_attempts) {finish(v,HEADSET_GAIN_CONFLICT,now);return;}
            ++v->conflict_attempts;++v->reconciliations;
        }
        ++v->completed;finish(v,HEADSET_GAIN_DONE,now);queue(v,now);
    }
}
void omni_headset_gain_yield(omni_headset_gain *v,uint32_t now)
{
    if(!v) return;
    v->cancel=v->yielding=true;v->remote_pending=false;
    if(v->phase==HEADSET_GAIN_QUEUED || v->phase==HEADSET_GAIN_DEFERRED || v->phase==HEADSET_GAIN_GAP ||
       (v->phase==HEADSET_GAIN_SEND && !v->offset)) finish(v,HEADSET_GAIN_CANCELLED,now);
}
void omni_headset_gain_release(omni_headset_gain *v,uint32_t now)
{
    if(!v) return;
    if(omni_headset_gain_busy(v)) finish(v,HEADSET_GAIN_CANCELLED,now);
    v->online=v->primed=v->have_verified=v->remote_pending=v->have_seen=v->have_link=v->connected=false;
    v->parser.used=v->parser.expected=0;v->history_count=v->history_cursor=0;v->cancel=v->yielding=false;
    v->conflict_attempts=0;v->readback_conflict=false;
}
void omni_headset_gain_acquire(omni_headset_gain *v,uint32_t now)
{
    if(!v || !v->initialized) return;
    omni_headset_gain_release(v,now);v->online=true;v->blocked=v->fault=false;
    v->phase=HEADSET_GAIN_IDLE;++v->epoch;queue(v,now);
}
bool omni_headset_gain_take_remote(omni_headset_gain *v,uint8_t *ll,uint32_t *revision,uint32_t *ms)
{
    if(!v || !ll || !revision || !ms || !v->remote_pending) return false;
    *ll=v->remote;*revision=v->remote_revision;*ms=v->remote_ms;v->remote_pending=false;return true;
}
bool omni_headset_gain_status(const omni_headset_gain *v,unsigned page,uint32_t out[15])
{
    if(!v || !out || page>1u) return false;
    uint32_t w[15]={1u,page};
    if(!page) {
        w[2]=(uint32_t)v->phase;
        w[3]=(uint32_t)v->online|((uint32_t)v->primed<<1)|((uint32_t)v->have_verified<<2)|
            ((uint32_t)omni_headset_gain_ready(v)<<3)|((uint32_t)omni_headset_gain_busy(v)<<4)|
            ((uint32_t)omni_headset_gain_active(v)<<5)|((uint32_t)v->fault<<6)|((uint32_t)v->blocked<<7)|
            ((uint32_t)v->have_link<<8)|((uint32_t)v->connected<<9)|((uint32_t)v->remote_pending<<10)|
            ((uint32_t)v->readback_conflict<<11)|((uint32_t)(v->phase==HEADSET_GAIN_DEFERRED)<<12)|
            ((v->queue_deferrals>0xffffu?0xffffu:v->queue_deferrals)<<16);
        w[4]=v->operation;w[5]=v->desired;w[6]=v->submitted;w[7]=v->verified;
        w[8]=v->revision;w[9]=v->submitted_revision;w[10]=v->verified_revision;
        w[11]=v->queued_ms;w[12]=v->started_ms;w[13]=v->drained_ms;
        w[14]=v->peer_status|((v->conflicts>0xffffffu?0xffffffu:v->conflicts)<<8);
    } else {
        w[2]=v->tx_bytes;w[3]=v->rx_bytes;w[4]=v->acks;w[5]=v->timeouts;w[6]=v->invalid;
        w[7]=v->nacks;w[8]=v->unrelated;w[9]=v->coalesced;w[10]=v->completed;
        w[11]=v->remote_events;w[12]=v->echoes;w[13]=v->epoch;w[14]=v->gain_recoveries;
    }
    memcpy(out,w,sizeof(w));return true;
}
