#include "dsp_meter.h"
#include "interchip.h"
#include <stdatomic.h>
#include <string.h>

_Static_assert(ATOMIC_INT_LOCK_FREE==2,"cache publication must be lock-free");
enum { IDLE,SEND,DRAIN,WAIT };
typedef struct {
    uint8_t raw[OMNI_DSP_METER_FRAME_BYTES];
    uint32_t received_ms,generation;
} meter_cache;
static struct {
    omni_link_parser parser;
    meter_cache cache[2];
    atomic_uint published;
    uint8_t pending[OMNI_DSP_METER_FRAME_BYTES];
    uint32_t pending_ms,query_ms,next_ms,cycles,timeouts,tx_bytes,rx_bytes;
    uint32_t invalid,io_errors,cancellations;
    uint8_t phase,offset;
    bool initialized,acquired,fault,scheduled,yielding,eligible,reply,negative;
} state;

void omni_dsp_meter_init(void)
{
    memset(&state,0,sizeof(state));
    atomic_init(&state.published,0u);
    (void)omni_link_init(&state.parser,20u);
    state.initialized=true;
}
static void ensure_init(void) { if(!state.initialized) omni_dsp_meter_init(); }
bool omni_dsp_meter_busy(void) { return state.phase!=IDLE; }
bool omni_dsp_meter_transport_fault(void) { return state.fault; }
static void clear_frame(void)
{ state.parser.used=state.parser.expected=0;state.eligible=false; }
static void finish(void)
{ state.phase=IDLE;state.offset=0;state.reply=state.negative=false; }

void omni_dsp_meter_release(uint32_t now)
{
    (void)now;ensure_init();
    if(omni_dsp_meter_busy()) {
        ++state.cancellations;
        if((state.phase==SEND && state.offset!=0u) || state.phase==DRAIN)
            state.fault=true;
    }
    finish();clear_frame();state.acquired=false;state.yielding=false;
}
void omni_dsp_meter_acquire(uint32_t now)
{
    ensure_init();
    if(state.acquired) return; /* Idempotent: cannot reset a live query. */
    clear_frame();finish();state.acquired=true;state.fault=false;
    state.yielding=false;state.scheduled=false;state.next_ms=now;
}
void omni_dsp_meter_io_error(uint32_t now)
{
    (void)now;ensure_init();++state.io_errors;state.fault=true;
    if(omni_dsp_meter_busy()) ++state.cancellations;
    finish();clear_frame();
}
void omni_dsp_meter_yield(uint32_t now)
{
    (void)now;ensure_init();state.yielding=true;
    if(state.phase==WAIT || (state.phase==SEND && state.offset==0u)) {
        ++state.cancellations;finish();clear_frame();
    }
}
static bool request_eligible(uint32_t now)
{
    return state.acquired && !state.fault && !state.yielding &&
        (state.phase==DRAIN || state.phase==WAIT) && state.offset==4u &&
        (uint32_t)(now-state.query_ms)<OMNI_DSP_METER_TIMEOUT_MS;
}
void omni_dsp_meter_observe(uint8_t byte,uint32_t now)
{
    ensure_init();
    if(!state.acquired || state.fault) return;
    ++state.rx_bytes;
    omni_link_expire(&state.parser,now);
    if(!state.parser.used) state.eligible=request_eligible(now);
    uint32_t ignored=state.parser.ignored;
    const uint8_t *p;size_t n;
    bool complete=omni_link_feed(&state.parser,byte,now,&p,&n);
    if(state.parser.ignored!=ignored && state.eligible && request_eligible(now) &&
       state.parser.bytes[1]==3u && state.parser.bytes[2]==0x50u &&
       state.parser.bytes[3]!=0u) {
        ++state.invalid;state.negative=true;
    }
    if(!complete || p[0]!=0xdbu || n<3u || p[2]!=0x50u) return;
    if(n!=OMNI_DSP_METER_FRAME_BYTES || p[3]!=3u) {
        ++state.invalid;return;
    }
    /* A prefix that arrived before the complete request was submitted cannot
     * become eligible merely because the rest arrived after submission. */
    if(!state.eligible || !request_eligible(now) || state.reply || state.negative) {
        ++state.invalid;return;
    }
    memcpy(state.pending,p,OMNI_DSP_METER_FRAME_BYTES);
    state.pending_ms=now;state.reply=true;
}
static void publish(void)
{
    unsigned previous=atomic_load_explicit(&state.published,memory_order_relaxed);
    unsigned next=previous^1u;
    uint32_t generation=state.cache[previous].generation+1u;
    if(!generation) generation=1u; /* Zero always means no cached reply. */
    memcpy(state.cache[next].raw,state.pending,OMNI_DSP_METER_FRAME_BYTES);
    state.cache[next].received_ms=state.pending_ms;
    state.cache[next].generation=generation;
    atomic_store_explicit(&state.published,next,memory_order_release);
}
void omni_dsp_meter_poll(uint32_t now,bool can_start,omni_dsp_meter_io io)
{
    ensure_init();omni_link_expire(&state.parser,now);
    if(!state.acquired || state.fault) return;
    if(omni_dsp_meter_busy() && (!io.tx || !io.tx_complete)) {
        omni_dsp_meter_io_error(now);return;
    }
    if(!omni_dsp_meter_busy()) {
        if(!can_start || state.yielding || !io.tx || !io.tx_complete || state.parser.used ||
           (state.scheduled && (uint32_t)(now-state.next_ms)>=0x80000000u)) return;
        state.phase=SEND;state.offset=0;state.query_ms=now;
        state.next_ms=now+OMNI_DSP_METER_QUERY_MS;state.scheduled=true;
        state.reply=state.negative=false;state.eligible=false;++state.cycles;
    }
    if((uint32_t)(now-state.query_ms)>=OMNI_DSP_METER_TIMEOUT_MS) {
        ++state.timeouts;
        if(state.phase==SEND || state.phase==DRAIN) state.fault=true;
        finish();clear_frame();
        /* Coalesce missed slots; a timeout never creates an immediate retry. */
        state.next_ms=now+OMNI_DSP_METER_QUERY_MS;return;
    }
    if(state.phase==SEND) {
        static const uint8_t request[4]={0xbd,4,0x50,2};
        for(unsigned budget=0;budget<4u && state.offset<4u;++budget) {
            int r=io.tx(io.context,request[state.offset]);
            if(!r) break;
            if(r!=1) { omni_dsp_meter_io_error(now);return; }
            ++state.offset;++state.tx_bytes;
        }
        if(state.offset==4u) state.phase=DRAIN;
    }
    if(state.phase==DRAIN) {
        int r=io.tx_complete(io.context);
        if(r!=0 && r!=1) { omni_dsp_meter_io_error(now);return; }
        if(r==1) state.phase=WAIT;
    }
    if(state.phase==WAIT) {
        if(state.yielding) {
            ++state.cancellations;finish();clear_frame();
        } else if(state.reply || state.negative) {
            if(state.reply && !state.negative) publish();
            finish();
        }
    }
}
static void put32(uint8_t *p,uint32_t v)
{ for(unsigned i=0;i<4u;++i) p[i]=(uint8_t)(v>>(8u*i)); }
bool omni_dsp_meter_read(unsigned page,uint32_t now,uint8_t out[60])
{
    if(!out || page>2u) return false;
    unsigned index=atomic_load_explicit(&state.published,memory_order_acquire);
    const meter_cache *cache=&state.cache[index];
    uint32_t generation=cache->generation;
    memset(out,0,60);put32(out,1u);put32(out+4,page);
    if(page) {
        put32(out+8,generation);
        if(generation) {
            unsigned offset=page==1u?0u:48u;
            unsigned count=page==1u?48u:28u;
            memcpy(out+12,cache->raw+offset,count);
        }
        return true;
    }
    uint32_t age=generation?now-cache->received_ms:UINT32_MAX;
    uint32_t flags=(uint32_t)(generation!=0u)|
        ((uint32_t)(generation!=0u && age<OMNI_DSP_METER_STALE_MS && !state.fault)<<1)|
        ((uint32_t)omni_dsp_meter_busy()<<2)|((uint32_t)state.fault<<3)|
        ((uint32_t)state.acquired<<4)|((uint32_t)(state.parser.used!=0u)<<5)|
        ((uint32_t)state.yielding<<6);
    const uint32_t words[13]={flags,state.phase,state.cycles,state.timeouts,
        state.tx_bytes,state.rx_bytes,cache->received_ms,age,generation,state.invalid,
        state.parser.malformed+state.parser.expired,state.io_errors,state.cancellations};
    for(unsigned i=0;i<13u;++i) put32(out+8u+4u*i,words[i]);
    return true;
}
