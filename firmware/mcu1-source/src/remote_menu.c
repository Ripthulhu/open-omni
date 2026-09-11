#include "remote_menu.h"
#include <string.h>

bool omni_remote_menu_init(omni_remote_menu *s)
{
    if(!s) return false;
    memset(s,0,sizeof(*s));s->peer_status=255;
    return omni_link_init(&s->parser,OMNI_REMOTE_MENU_GAP_MS);
}
bool omni_remote_menu_active(const omni_remote_menu *s)
{return s && s->phase>=REMOTE_MENU_SEND && s->phase<=REMOTE_MENU_GAP;}
bool omni_remote_menu_busy(const omni_remote_menu *s)
{return s && (s->pending || omni_remote_menu_active(s));}
bool omni_remote_menu_frame_pending(const omni_remote_menu *s)
{return s && s->parser.used!=0u;}
static void result(omni_remote_menu *s,uint32_t token,omni_remote_menu_phase phase)
{s->last_token=token;s->last_result=(uint32_t)phase;}
static void finish(omni_remote_menu *s,omni_remote_menu_phase phase)
{
    if(phase==REMOTE_MENU_ACCEPTED) ++s->completed;
    /* A sent91 without its ACK leaves accepted context unknown. A failed92
     * does not erase an already accepted91 context. */
    if(phase!=REMOTE_MENU_ACCEPTED && s->count && s->frames[s->index][2]==0x91u && s->offset && !s->ack)
        s->mode_known=false;
    result(s,s->token,phase);s->phase=phase;
}
static void queue(omni_remote_menu *s,uint32_t token,bool open,unsigned context,bool force,uint32_t now)
{
    if(s->pending) ++s->coalesced;
    s->pending=true;s->pending_token=token;s->pending_open=open;
    s->pending_context=(uint8_t)context;s->pending_force=force;s->queued_ms=now;
    s->desired_valid=true;s->desired_open=open;++s->generation;
    if(!omni_remote_menu_active(s)) s->phase=REMOTE_MENU_QUEUED;
}
bool omni_remote_menu_request(omni_remote_menu *s,uint32_t token,bool open,unsigned context,uint32_t now)
{
    if(!s || !token || token>=0x80000000u || context>2u || s->fault) return false;
    if(token==s->local_token) return s->local_open==open && s->local_context==context;
    if((omni_remote_menu_active(s) && token==s->token) || (s->pending && token==s->pending_token)) return false;
    s->local_token=token;s->local_open=open;s->local_context=context;
    queue(s,token,open,context,false,now);return true;
}
static uint32_t peer_token(omni_remote_menu *s)
{s->serial=(s->serial+1u)&0x7fffffffu;if(!s->serial)s->serial=1;return s->serial|0x80000000u;}
static void peer_mode(omni_remote_menu *s,bool open,uint32_t now)
{
    ++s->peer_requests;
    if(s->desired_valid && s->desired_open==open &&
       ((s->pending && s->pending_open==open) ||
        (omni_remote_menu_active(s) && s->planned_open==open))) {++s->duplicates;return;}
    queue(s,peer_token(s),open,open?2u:1u,true,now);
}
static bool eligible(const omni_remote_menu *s)
{return omni_remote_menu_active(s) && s->phase!=REMOTE_MENU_GAP && s->offset==4u;}
static bool observe(omni_remote_menu *s,const uint8_t *p,size_t n,uint32_t now,bool valid_ack)
{
    if(!s || !p) return false;
    if(n>=1u && p[0]==0xddu) {
        if(n!=4u) {++s->invalid;return false;}
        if(!valid_ack || !eligible(s) || p[2]!=s->frames[s->index][2]) {++s->unrelated;return false;}
        if(p[1]!=3u) {++s->invalid;s->bad_reply=true;return false;}
        ++s->acks;s->peer_status=p[3];
        if(p[3]) s->negative=true;else s->ack=true;
        return true;
    }
    if(n==4u && p[0]==0xdbu && p[1]==4u && p[2]==0x91u && (p[3]==10u || p[3]==8u)) {
        if(s->fault) return false;
        peer_mode(s,p[3]==10u,now);return true;
    }
    if(n==5u && p[0]==0xdbu && p[1]==5u && p[2]==0xe4u && p[3]==3u && p[4]>=1u && p[4]<=3u) {
        bool was_connected=s->have_link && s->connected;
        s->have_link=true;s->connected=p[4]==3u;
        if(!s->connected) {
            s->mode_known=false;omni_remote_menu_yield(s,now);
        } else if(!was_connected && s->desired_valid && s->desired_open && !s->fault &&
                  (!s->mode_known || !s->mode_open) &&
                  !(s->pending && s->pending_open) &&
                  !(omni_remote_menu_active(s) && !s->cancel && s->planned_open)) {
            queue(s,peer_token(s),true,0u,true,now);
        }
        return true;
    }
    return false;
}
bool omni_remote_menu_observe(omni_remote_menu *s,const uint8_t *p,size_t n,uint32_t now)
{return observe(s,p,n,now,s && eligible(s));}
void omni_remote_menu_byte(omni_remote_menu *s,uint8_t byte,uint32_t now)
{
    if(!s) return;
    omni_link_expire(&s->parser,now);
    if(!s->parser.used) s->frame_eligible=eligible(s);
    uint32_t ignored=s->parser.ignored;const uint8_t *p;size_t n;
    bool complete=omni_link_feed(&s->parser,byte,now,&p,&n);
    if(s->parser.ignored!=ignored) (void)observe(s,s->parser.bytes,4u,now,s->frame_eligible);
    else if(complete) (void)observe(s,p,n,now,s->frame_eligible);
}
static void plan(omni_remote_menu *s,uint32_t now)
{
    s->token=s->pending_token;s->planned_open=s->pending_open;s->context=s->pending_context;
    s->count=0;s->index=s->offset=0;s->cancel=s->ack=s->negative=s->bad_reply=false;s->peer_status=255;
    if(s->pending_force || !s->mode_known || s->mode_open!=s->planned_open) {
        const uint8_t p[]={0xbd,4,0x91,s->planned_open?10u:9u};
        memcpy(s->frames[s->count++],p,4);
    }
    if(s->context) {
        const uint8_t p[]={0xbd,4,0x92,s->context};memcpy(s->frames[s->count++],p,4);
    }
    s->pending=false;s->started_ms=now;s->phase=REMOTE_MENU_SEND;
    if(!s->count) finish(s,REMOTE_MENU_ACCEPTED);
}
void omni_remote_menu_io_error(omni_remote_menu *s,uint32_t now)
{
    (void)now;if(!s)return;s->fault=true;s->mode_known=false;
    if(omni_remote_menu_active(s)) finish(s,REMOTE_MENU_IO_ERROR);
    if(s->pending) {result(s,s->pending_token,REMOTE_MENU_IO_ERROR);s->pending=false;}
    s->phase=REMOTE_MENU_IO_ERROR;
}
void omni_remote_menu_poll(omni_remote_menu *s,uint32_t now,bool can_start,omni_remote_menu_io io)
{
    if(!s) return;
    omni_link_expire(&s->parser,now);
    if(s->pending && (uint32_t)(now-s->queued_ms)>=OMNI_REMOTE_MENU_QUEUE_MS) {
        ++s->timeouts;result(s,s->pending_token,REMOTE_MENU_TIMEOUT);s->pending=false;
        if(!omni_remote_menu_active(s)) s->phase=REMOTE_MENU_TIMEOUT;
    }
    if(s->fault) return;
    if(!omni_remote_menu_active(s)) {
        if(!s->pending || !can_start || s->parser.used || (s->have_link && !s->connected)) return;
        plan(s,now);
        if(!omni_remote_menu_active(s)) return;
    }
    if(!io.tx || !io.drained) {omni_remote_menu_io_error(s,now);return;}
    if((uint32_t)(now-s->started_ms)>=OMNI_REMOTE_MENU_TIMEOUT_MS) {
        ++s->timeouts;
        if(s->phase==REMOTE_MENU_DRAIN || (s->phase==REMOTE_MENU_SEND && s->offset)) s->fault=true;
        finish(s,REMOTE_MENU_TIMEOUT);return;
    }
    if(s->phase==REMOTE_MENU_GAP) {
        if(s->cancel) {finish(s,REMOTE_MENU_CANCELLED);return;}
        if(!can_start || s->parser.used || (uint32_t)(now-s->drained_ms)<OMNI_REMOTE_MENU_GAP_MS) return;
        ++s->index;s->offset=0;s->ack=s->negative=s->bad_reply=false;s->peer_status=255;s->phase=REMOTE_MENU_SEND;
    }
    if(s->phase==REMOTE_MENU_SEND) {
        for(unsigned budget=0;budget<4u && s->offset<4u;++budget) {
            int r=io.tx(io.context,s->frames[s->index][s->offset]);
            if(!r) break;
            if(r!=1) {omni_remote_menu_io_error(s,now);return;}
            ++s->offset;++s->tx_bytes;
        }
        if(s->offset==4u) s->phase=REMOTE_MENU_DRAIN;
    }
    if(s->phase==REMOTE_MENU_DRAIN) {
        int r=io.drained(io.context);
        if(r!=0 && r!=1) {omni_remote_menu_io_error(s,now);return;}
        if(r==1) {s->drained_ms=now;s->phase=REMOTE_MENU_WAIT;}
    }
    if(s->phase==REMOTE_MENU_WAIT) {
        if(s->cancel) finish(s,REMOTE_MENU_CANCELLED);
        else if(s->negative) {++s->nacks;finish(s,REMOTE_MENU_NACK);}
        else if(s->bad_reply) finish(s,REMOTE_MENU_INVALID_REPLY);
        else if(s->ack) {
            if(s->frames[s->index][2]==0x91u) {s->mode_open=s->planned_open;s->mode_known=true;}
            if(s->index+1u<s->count) s->phase=REMOTE_MENU_GAP;
            else finish(s,REMOTE_MENU_ACCEPTED);
        }
    }
}
void omni_remote_menu_yield(omni_remote_menu *s,uint32_t now)
{
    (void)now;if(!s)return;
    if(s->pending) {result(s,s->pending_token,REMOTE_MENU_CANCELLED);s->pending=false;}
    if(omni_remote_menu_active(s)) {
        s->cancel=true;
        if(s->phase==REMOTE_MENU_GAP || s->phase==REMOTE_MENU_WAIT ||
           (s->phase==REMOTE_MENU_SEND && !s->offset)) finish(s,REMOTE_MENU_CANCELLED);
    } else s->phase=REMOTE_MENU_CANCELLED;
}
void omni_remote_menu_release(omni_remote_menu *s,uint32_t now)
{
    if(!s)return;
    if(omni_remote_menu_active(s) && (s->phase==REMOTE_MENU_DRAIN ||
       (s->phase==REMOTE_MENU_SEND && s->offset))) s->fault=true;
    omni_remote_menu_yield(s,now);
    if(omni_remote_menu_active(s)) finish(s,REMOTE_MENU_CANCELLED);
    s->mode_known=false;s->have_link=false;s->parser.used=s->parser.expected=0;
}
void omni_remote_menu_acquire(omni_remote_menu *s)
{if(s){s->fault=false;s->parser.used=s->parser.expected=0;}}
bool omni_remote_menu_status(const omni_remote_menu *s,unsigned page,uint32_t out[15])
{
    if(!s || !out || page>1u)return false;
    memset(out,0,60);out[0]=1;out[1]=page;
    if(!page) {
        out[2]=(uint32_t)s->phase;out[3]=(uint32_t)omni_remote_menu_active(s)|((uint32_t)s->pending<<1)|
            ((uint32_t)s->fault<<2)|((uint32_t)s->cancel<<3)|((uint32_t)s->desired_valid<<4)|
            ((uint32_t)s->mode_known<<5)|((uint32_t)s->have_link<<6)|((uint32_t)s->connected<<7)|((uint32_t)s->ack<<8);
        out[4]=s->token;out[5]=s->pending?s->pending_token:0u;
        out[6]=s->desired_valid?(uint32_t)s->desired_open:255u;
        out[7]=s->mode_known?(uint32_t)s->mode_open:255u;
        out[8]=s->context;out[9]=s->pending_context;out[10]=s->index;out[11]=s->offset;
        out[12]=s->started_ms;out[13]=s->drained_ms;out[14]=s->peer_status;
    } else {
        out[2]=s->tx_bytes;out[3]=s->acks;out[4]=s->timeouts;out[5]=s->nacks;out[6]=s->invalid;
        out[7]=s->unrelated;out[8]=s->coalesced;out[9]=s->peer_requests;out[10]=s->duplicates;
        out[11]=s->completed;out[12]=s->last_token;out[13]=s->last_result;out[14]=s->generation;
    }
    return true;
}
