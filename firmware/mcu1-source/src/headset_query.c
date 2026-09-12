#include "headset_query.h"
#include "interchip.h"
#include <stdatomic.h>
#include <string.h>

/* AB1585 stock TX DSP v0.36.0 quirk (dispatcher 0x0836D77E): the bank-2
 * sidetone GET (BD 05 D4 02 02) unconditionally emits its local negative ACK
 * (DD 03 D4 01) before the sidetone constructor at 0x0836B45C sends the real
 * DB 07 D4 02 03 reply. For THIS profile only, keep the raw negative status
 * (peer_status) but DEFER the terminal so the bounded WAIT window still
 * accepts the late DB. Not device-verified; scoped to one profile id. Define
 * 0u to disable on images whose bank-2 path does not pre-NACK. */
#ifndef OMNI_HEADSET_QUERY_D4_BANK2_NACK_QUIRK
#define OMNI_HEADSET_QUERY_D4_BANK2_NACK_QUIRK 11u
#endif

typedef struct { uint8_t request[5],reply[5],prefix_length,reply_length; } profile;
static const profile profiles[13]={
    {{0xbd,4,0xe1,2},{0xdb,13,0xe1,3},4,13},
    {{0xbd,4,0xe4,2},{0xdb,5,0xe4,3},4,5},
    {{0xbd,4,0x20,1},{0xdb,46,0x20,1},4,46},
    {{0xbd,4,0x80,2},{0xdb,14,0x80,3},4,14},
    {{0xbd,4,0xd5,2},{0xdb,5,0xd5,3},4,5},
    {{0xbd,5,0xd5,2,0x10},{0xdb,6,0xd5,3,0x10},5,6},
    {{0xbd,5,0xd5,2,0x11},{0xdb,6,0xd5,3,0x11},5,6},
    {{0xbd,5,0xd3,2,1},{0xdb,6,0xd3,3,1},5,6},
    {{0xbd,5,0xd3,2,2},{0xdb,6,0xd3,3,2},5,6},
    {{0xbd,5,0xd4,1,2},{0xdb,7,0xd4,1,3},5,7},
    {{0xbd,5,0xd4,2,2},{0xdb,7,0xd4,2,3},5,7},
    {{0xbd,4,0xdb,2},{0xdb,7,0xdb,3},4,7},
    {{0xbd,5,0xe3,2,1},{0xdb,9,0xe3,3,1},5,9}
};
static struct {
    omni_link_parser parser;
    uint32_t token,profile,queued_ms,started_ms,finished_ms,received_ms,drained_ms;
    uint32_t tx_bytes,rx_bytes,unrelated,invalid,ack_count,timeout_reason;
    uint8_t raw[OMNI_HEADSET_QUERY_MAX_REPLY],length,offset,peer_status;
    volatile omni_headset_query_phase phase;
    bool matched,negative,bad_reply,fault,cancel,frame_eligible,nack_pending;
} state;
static bool transport_faulted;
bool omni_headset_query_busy(void)
{ return state.phase>=HEADSET_QUERY_QUEUED && state.phase<=HEADSET_QUERY_WAIT; }
bool omni_headset_query_active(void)
{ return state.phase>=HEADSET_QUERY_SEND && state.phase<=HEADSET_QUERY_WAIT; }
bool omni_headset_query_transport_fault(void) { return transport_faulted; }
static void publish(omni_headset_query_phase phase)
{ atomic_signal_fence(memory_order_release);state.phase=phase; }
static void finish(omni_headset_query_phase phase,uint32_t now)
{ state.finished_ms=now;publish(phase); }
bool omni_headset_query_request(uint32_t token,unsigned selected,uint32_t now)
{
    if(!token || selected<1u || selected>13u) return false;
    if(state.phase!=HEADSET_QUERY_IDLE && state.token==token) return state.profile==selected;
    if(omni_headset_query_busy() || transport_faulted) return false;
    memset(&state,0,sizeof(state));
    (void)omni_link_init(&state.parser,20u);
    state.token=token;state.profile=selected;state.queued_ms=now;state.peer_status=255u;
    publish(HEADSET_QUERY_QUEUED);return true;
}
void omni_headset_query_observe(uint8_t byte,uint32_t now)
{
    if(!omni_headset_query_active()) return;
    ++state.rx_bytes;
    const profile *spec=&profiles[state.profile-1u];
    bool submitted=state.offset==spec->request[1];
    omni_link_expire(&state.parser,now);
    if(!state.parser.used) state.frame_eligible=submitted;
    uint32_t ignored=state.parser.ignored;
    const uint8_t *p;size_t n;
    bool complete=omni_link_feed(&state.parser,byte,now,&p,&n);
    if(state.parser.ignored!=ignored) {
        p=state.parser.bytes;
        if(submitted && state.frame_eligible && p[2]==spec->request[2]) {
            if(p[1]!=3u) { ++state.invalid;state.bad_reply=true;return; }
            ++state.ack_count;state.peer_status=p[3];
            /* Quirk: defer the bank-2 sidetone NACK so a late,correctly framed
             * DB 07 D4 02 03 can still complete;the raw status stays in
             * peer_status. Every other profile NACKs immediately. */
            if(p[3]) {
                if(state.profile==OMNI_HEADSET_QUERY_D4_BANK2_NACK_QUIRK) state.nack_pending=true;
                else state.negative=true;
            }
        } else ++state.unrelated;
    }
    if(!complete) return;
    if(!submitted || !state.frame_eligible || p[0]!=0xdbu || n<3u || p[2]!=spec->reply[2]) {
        ++state.unrelated;return;
    }
    /* Distinct settings share opcodes. A different documented subfield is
     * unrelated telemetry,not a malformed response to the current request. */
    if(p[2]==0xd4u && n>=4u && p[3]!=spec->reply[3]) { ++state.unrelated;return; }
    if((p[2]==0xd3u || p[2]==0xd5u) && n>=4u && p[3]==3u) {
        if(spec->prefix_length==5u && (n==5u || (n>=5u && p[4]!=spec->reply[4]))) {
            ++state.unrelated;return;
        }
        if(state.profile==5u && n==6u && (p[4]==0x10u || p[4]==0x11u)) {
            ++state.unrelated;return;
        }
    }
    if(n!=spec->reply_length || memcmp(p,spec->reply,spec->prefix_length)) {
        ++state.invalid;state.bad_reply=true;return;
    }
    if(state.matched) { ++state.unrelated;return; }
    memcpy(state.raw,p,n);state.length=(uint8_t)n;state.received_ms=now;state.matched=true;
}
void omni_headset_query_io_error(uint32_t now)
{
    transport_faulted=true;
    if(omni_headset_query_busy()) { state.fault=true;finish(HEADSET_QUERY_IO_ERROR,now); }
}
void omni_headset_query_poll(uint32_t now,bool can_start,omni_headset_query_io io)
{
    if(!omni_headset_query_busy()) return;
    if(state.phase==HEADSET_QUERY_QUEUED) {
        if((uint32_t)(now-state.queued_ms)>=OMNI_HEADSET_QUERY_QUEUE_MS) {
            state.timeout_reason=1;finish(HEADSET_QUERY_TIMEOUT,now);return;
        }
        if(!can_start) return;
        if(!io.tx || !io.tx_complete) { omni_headset_query_io_error(now);return; }
        state.started_ms=now;state.parser.used=state.parser.expected=0;
        publish(HEADSET_QUERY_SEND);
    }
    if(!io.tx || !io.tx_complete) { omni_headset_query_io_error(now);return; }
    if((uint32_t)(now-state.started_ms)>=OMNI_HEADSET_QUERY_TIMEOUT_MS) {
        state.timeout_reason=2;
        if(state.phase==HEADSET_QUERY_SEND || state.phase==HEADSET_QUERY_DRAIN) {
            state.fault=true;transport_faulted=true;
        }
        /* Quirk: a deferred bank-2 NACK with no DB by the deadline resolves as
         * the NACK it was;a matched (or malformed) DB still takes precedence. */
        if(state.phase==HEADSET_QUERY_WAIT && state.nack_pending) {
            finish(state.matched?HEADSET_QUERY_DONE:
                   state.bad_reply?HEADSET_QUERY_INVALID_REPLY:HEADSET_QUERY_NACK,now);
            return;
        }
        finish(HEADSET_QUERY_TIMEOUT,now);return;
    }
    omni_link_expire(&state.parser,now);
    const profile *spec=&profiles[state.profile-1u];
    if(state.phase==HEADSET_QUERY_SEND) {
        for(unsigned budget=0;budget<5u && state.offset<spec->request[1];++budget) {
            int r=io.tx(io.context,spec->request[state.offset]);
            if(!r) break;
            if(r!=1) { omni_headset_query_io_error(now);return; }
            ++state.offset;++state.tx_bytes;
        }
        if(state.offset==spec->request[1]) publish(HEADSET_QUERY_DRAIN);
    }
    if(state.phase==HEADSET_QUERY_DRAIN) {
        int r=io.tx_complete(io.context);
        if(r!=0 && r!=1) { omni_headset_query_io_error(now);return; }
        if(r==1) { state.drained_ms=now;publish(HEADSET_QUERY_WAIT); }
    }
    if(state.phase==HEADSET_QUERY_WAIT) {
        if(state.cancel) finish(HEADSET_QUERY_CANCELLED,now);
        else if(state.negative) finish(HEADSET_QUERY_NACK,now);
        else if(state.bad_reply) finish(HEADSET_QUERY_INVALID_REPLY,now);
        else if(state.matched) finish(HEADSET_QUERY_DONE,now);
    }
}
void omni_headset_query_yield(uint32_t now)
{
    if(!omni_headset_query_busy()) return;
    state.cancel=true;
    if(state.phase==HEADSET_QUERY_QUEUED || (state.phase==HEADSET_QUERY_SEND && !state.offset))
        finish(HEADSET_QUERY_CANCELLED,now);
}
void omni_headset_query_release(uint32_t now)
{
    if(omni_headset_query_busy()) { state.cancel=true;finish(HEADSET_QUERY_CANCELLED,now); }
}
void omni_headset_query_acquire(void) { transport_faulted=false; }
bool omni_headset_query_status(unsigned page,uint32_t now,uint32_t out[15])
{
    (void)now;if(!out || page>1u) return false;
    uint32_t w[15]={1u,page,state.token,state.profile,(uint32_t)state.phase};
    if(!page) {
        w[5]=(uint32_t)omni_headset_query_busy()|((uint32_t)omni_headset_query_active()<<1)|
            ((uint32_t)state.matched<<2)|((uint32_t)state.fault<<3)|((uint32_t)state.cancel<<4);
        w[6]=state.queued_ms;w[7]=state.started_ms;w[8]=state.finished_ms;
        w[9]=state.tx_bytes;w[10]=state.rx_bytes;w[11]=state.length;
        w[12]=state.unrelated;w[13]=state.invalid;
        w[14]=state.phase==HEADSET_QUERY_IDLE?255u:state.peer_status;
    } else {
        w[5]=state.received_ms;w[6]=state.drained_ms;w[7]=state.parser.frames;
        w[8]=state.parser.malformed;w[9]=state.parser.expired;
        w[10]=state.ack_count;w[11]=state.timeout_reason;
        if(state.profile) {
            const profile *spec=&profiles[state.profile-1u];
            w[12]=spec->request[1];memcpy(w+13,spec->request,spec->request[1]);
        }
    }
    memcpy(out,w,sizeof(w));return true;
}
bool omni_headset_query_reply(uint32_t token,uint8_t out[60])
{
    if(!out || !token || token!=state.token || omni_headset_query_busy() || !state.matched) return false;
    uint32_t header[3]={1u,token,state.length};memset(out,0,60);
    memcpy(out,header,sizeof(header));memcpy(out+sizeof(header),state.raw,state.length);return true;
}
