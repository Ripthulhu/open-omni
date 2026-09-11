#include "headset_battery.h"
#include "interchip.h"
#include <string.h>

enum { QUERY_IDLE,QUERY_SEND,QUERY_DRAIN,QUERY_WAIT };
static struct {
    omni_link_parser parser;
    uint32_t percent_ms,charge_ms,next_ms,query_ms,cycles,timeouts,invalid;
    uint32_t cancellations,tx_bytes,rx_bytes,io_errors,nacks;
    uint32_t battery_next_ms;
    uint8_t percent,charge,connection,phase,kind,offset;
    uint8_t percent_frame[8],charge_frame[8],percent_length,charge_length;
    bool initialized,percent_valid,charge_valid,connection_seen,scheduled,reply,negative,fault;
    bool battery_scheduled,frame_eligible;
} state;

void omni_headset_battery_init(void)
{
    memset(&state,0,sizeof(state));
    (void)omni_link_init(&state.parser,20u);
    state.percent=state.charge=state.connection=255u;
    state.kind=0u;state.initialized=true;
}
static void ensure_init(void) { if(!state.initialized) omni_headset_battery_init(); }
static bool connected_or_unknown(void) { return !state.connection_seen || state.connection==3u; }
bool omni_headset_battery_busy(void) { return state.phase!=QUERY_IDLE; }
bool omni_headset_battery_frame_pending(void) { return state.parser.used!=0u; }
bool omni_headset_battery_transport_fault(void) { return state.fault; }
void omni_headset_battery_expire(uint32_t now)
{ ensure_init();omni_link_expire(&state.parser,now); }

void omni_headset_battery_observe(uint8_t byte,uint32_t now)
{
    ensure_init();++state.rx_bytes;
    omni_link_expire(&state.parser,now);
    unsigned request_length=state.kind==0u?4u:5u;
    bool submitted=omni_headset_battery_busy() && state.offset==request_length;
    if(!state.parser.used) state.frame_eligible=submitted;
    uint32_t ignored=state.parser.ignored;
    const uint8_t *p;size_t n;
    bool complete=omni_link_feed(&state.parser,byte,now,&p,&n);
    if(state.parser.ignored!=ignored && omni_headset_battery_busy() &&
       submitted && state.frame_eligible && state.parser.bytes[1]==3u &&
       state.parser.bytes[2]==(state.kind==0u?0xe4u:0xe2u) && state.parser.bytes[3]!=0u) {
        ++state.nacks;state.negative=true;
    }
    if(!complete || n<3u) return;
    if(p[0]!=0xdbu) return;
    if(p[2]==0xe4u) {
        if(n!=5u || p[3]!=3u) { ++state.invalid;return; }
        bool was_connected=state.connection_seen && state.connection==3u;
        state.connection=p[4];state.connection_seen=true;
        if(p[4]!=3u) state.percent_valid=state.charge_valid=false;
        else if(!was_connected) {
            state.percent_valid=state.charge_valid=false;
            state.battery_scheduled=false;
            if(!omni_headset_battery_busy()) state.scheduled=false;
        }
        if(submitted && state.frame_eligible && state.kind==0u) {
            state.reply=p[4]>=1u && p[4]<=3u;state.negative=!state.reply;
            if(state.negative) ++state.invalid;
        }
        return;
    }
    if(p[2]!=0xe2u) return;
    if(n<5u || p[3]!=3u) { ++state.invalid;return; }
    bool accepted=false;
    if(p[4]==1u) {
        if(n!=8u) { ++state.invalid;state.percent_valid=false;return; }
        state.percent_valid=false;
        memcpy(state.percent_frame,p,8);state.percent_length=8;
        state.percent=p[7];state.percent_ms=now;
        state.percent_valid=p[7]<=100u && connected_or_unknown();
        accepted=state.percent_valid;
        if(p[7]>100u) ++state.invalid;
    } else if(p[4]==2u) {
        if(n!=6u) { ++state.invalid;state.charge_valid=false;return; }
        state.charge_valid=false;
        memcpy(state.charge_frame,p,6);state.charge_length=6;
        state.charge=p[5];state.charge_ms=now;
        state.charge_valid=p[5]<=2u && connected_or_unknown();
        accepted=state.charge_valid;
        if(p[5]>2u) ++state.invalid;
    } else { ++state.invalid;return; }
    if(submitted && state.frame_eligible && p[4]==state.kind) {
        state.reply=accepted;state.negative=!accepted;
    }
}

static void finish_query(uint32_t now,bool success)
{
    uint8_t completed=state.kind;
    state.phase=QUERY_IDLE;state.offset=0;state.reply=false;state.negative=false;
    bool battery_due=!state.battery_scheduled ||
        (uint32_t)(now-state.battery_next_ms)<0x80000000u;
    if(success && completed==0u && state.connection==3u && battery_due) state.kind=1u;
    else if(success && completed==1u && connected_or_unknown()) state.kind=2u;
    else state.kind=0u;
    state.next_ms=now+(state.kind==0u?OMNI_HEADSET_LINK_QUERY_MS:20u);
    state.scheduled=true;
}
void omni_headset_battery_io_error(void)
{
    ensure_init();++state.io_errors;state.fault=true;
    state.percent_valid=state.charge_valid=false;
    if(omni_headset_battery_busy()) ++state.cancellations;
    state.phase=QUERY_IDLE;state.kind=0;state.offset=0;
}
void omni_headset_battery_poll(uint32_t now,bool can_start,omni_headset_battery_io io)
{
    ensure_init();omni_link_expire(&state.parser,now);
    if(state.fault) return;
    if(omni_headset_battery_busy() && (!io.tx || !io.tx_complete)) {
        omni_headset_battery_io_error();return;
    }
    if(!omni_headset_battery_busy()) {
        if(!can_start || !io.tx || !io.tx_complete || state.parser.used ||
           (state.scheduled && (uint32_t)(now-state.next_ms)>=0x80000000u)) return;
        if(state.kind!=0u && !connected_or_unknown()) state.kind=0u;
        if(state.kind==1u) {
            ++state.cycles;state.battery_scheduled=true;
            state.battery_next_ms=now+OMNI_HEADSET_BATTERY_QUERY_MS;
        }
        state.phase=QUERY_SEND;state.offset=0;state.query_ms=now;
        state.reply=false;state.negative=false;
    }
    if((uint32_t)(now-state.query_ms)>=OMNI_HEADSET_BATTERY_TIMEOUT_MS) {
        ++state.timeouts;
        /* A partial TX or unconfirmed stop bit cannot be reused safely. */
        if(state.phase==QUERY_SEND || state.phase==QUERY_DRAIN) {
            state.fault=true;state.percent_valid=state.charge_valid=false;
        }
        finish_query(now,false);return;
    }
    if(state.phase==QUERY_SEND) {
        const uint8_t query[5]={0xbd,state.kind==0u?4u:5u,state.kind==0u?0xe4u:0xe2u,2,state.kind};
        unsigned length=state.kind==0u?4u:5u;
        for(unsigned budget=0;budget<5u && state.offset<length;++budget) {
            int r=io.tx(io.context,query[state.offset]);
            if(r==0) break;
            if(r!=1) { omni_headset_battery_io_error();return; }
            ++state.offset;++state.tx_bytes;
        }
        if(state.offset==length) state.phase=QUERY_DRAIN;
    }
    if(state.phase==QUERY_DRAIN) {
        int r=io.tx_complete(io.context);
        if(r!=0 && r!=1) { omni_headset_battery_io_error();return; }
        if(r==1) state.phase=QUERY_WAIT;
    }
    if(state.phase==QUERY_WAIT && (state.reply || state.negative))
        finish_query(now,state.reply && !state.negative && (state.kind==0u || connected_or_unknown()));
}
void omni_headset_battery_release(void)
{
    ensure_init();
    if(omni_headset_battery_busy()) ++state.cancellations;
    state.phase=QUERY_IDLE;state.kind=0;state.offset=0;state.scheduled=false;state.battery_scheduled=false;
    state.reply=state.negative=false;
    state.percent_valid=state.charge_valid=false;state.connection=255u;state.connection_seen=false;
    state.parser.used=state.parser.expected=0;
}
void omni_headset_battery_acquire(void)
{ omni_headset_battery_release();state.fault=false; }

static bool fresh(bool valid,uint32_t updated,uint32_t now)
{ return state.initialized && !state.fault && valid && connected_or_unknown() && (uint32_t)(now-updated)<OMNI_HEADSET_BATTERY_STALE_MS; }
uint32_t omni_headset_battery_display(uint32_t now)
{
    uint32_t percent=fresh(state.percent_valid,state.percent_ms,now)?state.percent:255u;
    uint32_t charge=fresh(state.charge_valid,state.charge_ms,now)?(uint32_t)state.charge+1u:0u;
    return percent|(charge<<8);
}
bool omni_headset_battery_status(unsigned page,uint32_t now,uint32_t out[15])
{
    if(!out || page>1u) return false;
    uint32_t w[15]={1u,page};
    if(!page) {
        w[2]=(uint32_t)fresh(state.percent_valid,state.percent_ms,now)|
            ((uint32_t)fresh(state.charge_valid,state.charge_ms,now)<<1)|
            ((uint32_t)state.connection_seen<<2)|
            ((uint32_t)(state.connection_seen && state.connection==3u)<<3)|
            ((uint32_t)state.percent_valid<<4)|((uint32_t)state.charge_valid<<5)|
            ((uint32_t)omni_headset_battery_busy()<<6)|
            ((uint32_t)omni_headset_battery_frame_pending()<<7)|((uint32_t)state.fault<<8);
        w[3]=state.initialized?state.percent:255u;w[4]=state.initialized?state.charge:255u;
        w[5]=state.percent_ms;w[6]=state.charge_ms;
        w[7]=state.percent_length?now-state.percent_ms:UINT32_MAX;
        w[8]=state.initialized?state.connection:255u;w[9]=state.phase;
        w[10]=state.cycles;w[11]=state.timeouts;w[12]=state.parser.frames;
        w[13]=state.invalid;w[14]=state.cancellations;
    } else {
        w[2]=state.kind;w[3]=state.tx_bytes;w[4]=state.rx_bytes;w[5]=state.io_errors;
        w[6]=state.nacks;w[7]=state.parser.malformed;w[8]=state.parser.expired;
        w[9]=state.percent_length;w[10]=state.charge_length;
        memcpy(w+11,state.percent_frame,8);memcpy(w+13,state.charge_frame,8);
    }
    memcpy(out,w,sizeof(w));return true;
}
