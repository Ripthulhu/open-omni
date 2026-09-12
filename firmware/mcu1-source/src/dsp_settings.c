#include "dsp_settings.h"
#include "interchip.h"
#include <stdatomic.h>
#include <string.h>
#include "dsp_settings_presets.inc"

typedef struct { uint8_t bytes[128],length,flags; uint32_t ms; } cached_value;
static cached_value cache[DSP_SETTING_COUNT];
static cached_value custom_eq[3];
static uint32_t eq_persist_gen;
static struct {
    omni_link_parser parser;
    uint8_t frame[132],value[128],length,value_length,offset,peer_status;
    uint8_t link_state;
    uint32_t token,control,queued_ms,started_ms,finished_ms,drained_ms;
    uint32_t tx_bytes,rx_bytes,ack_count,unrelated,invalid,coalesced,timeouts;
    uint32_t link_ms,link_gen,req_gen;
    volatile omni_dsp_settings_phase phase;
    bool initialized,fault,cancel,ack,negative,bad_reply,frame_eligible;
    bool verifying,matched,link_valid;
} state;

static void ensure_init(void)
{
    if(state.initialized) return;
    (void)omni_link_init(&state.parser,20u);
    state.initialized=true;state.peer_status=255u;
}
static bool minutes(uint8_t value)
{ return value==0u || value==1u || value==5u || value==10u || value==15u || value==30u || value==60u; }
static uint16_t le16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0]|(uint16_t)((uint16_t)p[1]<<8)); }
static bool gain_valid(uint8_t value)
{ return value<=127u || value>=136u; }
static bool eq_control(unsigned control)
{ return control>=DSP_SETTING_EQ_WIRELESS && control<=DSP_SETTING_EQ_BT; }
static const builtin_eq *builtin(unsigned control,unsigned preset)
{
    for(unsigned i=0;i<sizeof(builtins)/sizeof(builtins[0]);++i)
        if(builtins[i].control==control && builtins[i].preset==preset) return &builtins[i];
    return NULL;
}
static bool eq_valid(unsigned control,const uint8_t *value,size_t length)
{
    if(!eq_control(control)) return false;
    if(length!=(control==DSP_SETTING_EQ_WIRELESS?128u:78u)) return false;
    unsigned custom=control==DSP_SETTING_EQ_MIC?8u:4u;
    if(value[0]!=custom) {
        const builtin_eq *p=builtin(control,value[0]);
        /* ROM presets carry their actual coefficients. Never silently apply
         * caller-provided custom data under a built-in preset identity. */
        return p && length==p->length && !memcmp(value,p->blob,length);
    }
    for(unsigned band=0;band<10u;++band) {
        if(control==DSP_SETTING_EQ_WIRELESS) {
            const uint8_t *p=value+68u+band*6u;
            if(le16(p)<20u || le16(p)>20001u || p[2]<1u || p[2]>6u ||
               !gain_valid(p[3]) || le16(p+4)<200u || le16(p+4)>10000u) return false;
        } else if(!gain_valid(value[68u+band])) return false;
    }
    return true;
}
/* Pure builder: validate all input before writing the caller's output. */
static size_t encode(unsigned control,const uint8_t *value,size_t length,uint8_t out[132])
{
    if(!value || !length || control<1u || control>=DSP_SETTING_COUNT) return 0;
    uint8_t p[132]={0xbd,0,0,1};size_t size=0;
    switch(control) {
    case DSP_SETTING_LIMITER:
        if(length!=1u || value[0]>1u) return 0;
        p[2]=0xd2;p[3]=8;p[4]=value[0];size=5;break;
    case DSP_SETTING_MIC_VOLUME:
        if(length!=1u || value[0]<1u || value[0]>10u) return 0;
        p[2]=0xd3;p[4]=2;p[5]=value[0];size=6;break;
    case DSP_SETTING_SIDETONE:
        if(length!=2u || value[0]>1u || value[1]<1u || value[1]>10u) return 0;
        p[2]=0xd4;p[4]=1;p[5]=value[0];p[6]=value[1];size=7;break;
    case DSP_SETTING_MIC_NOISE:
        if(length!=2u || value[0]>1u || value[1]<1u || value[1]>3u) return 0;
        p[2]=0xdb;p[4]=value[0];p[5]=value[1];p[6]=0;size=7;break;
    case DSP_SETTING_ANC_STATE:
        if(length!=1u || value[0]>4u) return 0;
        p[2]=0xd5;p[4]=value[0];size=5;break;
    case DSP_SETTING_ANC_LEVEL:
    case DSP_SETTING_TRANSPARENCY:
        if(length!=1u || value[0]<1u || value[0]>(control==DSP_SETTING_ANC_LEVEL?3u:10u)) return 0;
        p[2]=0xd5;p[4]=control==DSP_SETTING_ANC_LEVEL?0x11:0x10;p[5]=value[0];size=6;break;
    case DSP_SETTING_BT_STARTUP:
    case DSP_SETTING_MIC_LED:
    case DSP_SETTING_BT_CALL:
        if(length!=3u || value[0]>1u || value[1]>10u || value[2]>2u) return 0;
        p[2]=0xe3;p[4]=1;memcpy(p+5,value,3);p[8]=(uint8_t)(control-DSP_SETTING_BT_STARTUP);size=9;break;
    case DSP_SETTING_AUTO_OFF:
        if(length!=1u || !minutes(value[0])) return 0;
        p[2]=0xe3;p[4]=2;p[5]=value[0];size=6;break;
    case DSP_SETTING_EQ_WIRELESS:
    case DSP_SETTING_EQ_MIC:
    case DSP_SETTING_EQ_BT:
        if(!eq_valid(control,value,length)) return 0;
        p[2]=(uint8_t)(0x1bu+2u*(control-DSP_SETTING_EQ_WIRELESS));
        memcpy(p+4,value,length);size=length+4u;break;
    case DSP_SETTING_OUTPUT_MODE:
        if(length!=1u || value[0]<1u || value[0]>2u) return 0;
        p[2]=0x43;p[4]=value[0];size=5;break;
    case DSP_SETTING_HOME_MODE:
        if(length!=1u || value[0]>1u) return 0;
        /* Exact six bytes: the stock dispatcher can ACK malformed shorter
         * frames or a failed remote forward. Do not call this peer readback. */
        p[2]=0xd2;p[3]=9;p[4]=1;p[5]=value[0];size=6;break;
    default:return 0;
    }
    p[1]=(uint8_t)size;memcpy(out,p,size);return size;
}
bool omni_dsp_settings_valid(unsigned control,const uint8_t *value,size_t length)
{ uint8_t frame[132];return encode(control,value,length,frame)!=0u; }

size_t omni_dsp_settings_eq_flat(unsigned control,unsigned preset,uint8_t out[128])
{
    if(!out || !eq_control(control)) return 0;
    const builtin_eq *built=builtin(control,preset);
    if(built) { memcpy(out,built->blob,built->length);return built->length; }
    if(preset!=(control==DSP_SETTING_EQ_MIC?8u:4u)) return 0;
    uint8_t value[128]={0};value[0]=(uint8_t)preset;
    memcpy(value+1,"Native",6);memcpy(value+7,"Native flat",11);
    size_t length=control==DSP_SETTING_EQ_WIRELESS?128u:78u;
    if(control==DSP_SETTING_EQ_WIRELESS) {
        static const uint16_t freq[10]={31,62,125,250,500,1000,2000,4000,8000,16000};
        for(unsigned i=0;i<10u;++i) {
            uint8_t *p=value+68u+i*6u;
            p[0]=(uint8_t)freq[i];p[1]=(uint8_t)(freq[i]>>8);
            p[2]=1;p[4]=0xe8;p[5]=3; /* Peak,0dB,Q1.000. */
        }
    }
    memcpy(out,value,length);return length;
}
size_t omni_dsp_settings_eq_preset(unsigned control,unsigned preset,uint8_t out[128])
{ return omni_dsp_settings_eq_flat(control,preset,out); }
bool omni_dsp_settings_busy(void)
{ return state.phase>=DSP_SETTINGS_QUEUED && state.phase<=DSP_SETTINGS_VERIFY_QUEUED; }
bool omni_dsp_settings_active(void)
{ return state.phase>=DSP_SETTINGS_SEND && state.phase<=DSP_SETTINGS_VERIFY_QUEUED; }
bool omni_dsp_settings_transport_fault(void) { return state.fault; }
bool omni_dsp_settings_frame_pending(void) { return state.parser.used!=0u; }
void omni_dsp_settings_expire(uint32_t now)
{ ensure_init();omni_link_expire(&state.parser,now); }
static void publish(omni_dsp_settings_phase phase)
{ atomic_signal_fence(memory_order_release);state.phase=phase; }
static void remember(unsigned control,const uint8_t *value,size_t length,uint8_t flags,uint32_t now)
{
    if(control<1u || control>=DSP_SETTING_COUNT || length>128u) return;
    cached_value *c=&cache[control];
    if(c->length==length && !memcmp(c->bytes,value,length)) flags=(uint8_t)(flags|c->flags);
    memcpy(c->bytes,value,length);c->length=(uint8_t)length;c->flags=flags;c->ms=now;
    if(eq_control(control) && length==(control==DSP_SETTING_EQ_WIRELESS?128u:78u) &&
       value[0]==(control==DSP_SETTING_EQ_MIC?8u:4u) && (flags&1u))
        custom_eq[control-DSP_SETTING_EQ_WIRELESS]=*c;
    if(eq_control(control) && length==(control==DSP_SETTING_EQ_WIRELESS?128u:78u)) ++eq_persist_gen;
    if(control>=DSP_SETTING_BT_STARTUP && control<=DSP_SETTING_BT_CALL && length==3u)
        for(unsigned id=DSP_SETTING_BT_STARTUP;id<=DSP_SETTING_BT_CALL;++id)
            if(id!=control) cache[id]=*c;
}
static void invalidate(unsigned control)
{
    if(control>=DSP_SETTING_BT_STARTUP && control<=DSP_SETTING_BT_CALL) {
        for(unsigned i=DSP_SETTING_BT_STARTUP;i<=DSP_SETTING_BT_CALL;++i) cache[i].flags=0;
    } else if(control<DSP_SETTING_COUNT) cache[control].flags=0;
}
static void finish(omni_dsp_settings_phase phase,uint32_t now)
{
    /* Only re-validate on ACCEPTED when the RF epoch stamped at request start
     * still holds. A disconnect between SET and its late ACK bumped link_gen;
     * that stale ACK must not repopulate the cache the disconnect cleared. The
     * phase/status still report ACCEPTED (the local DD ACK genuinely arrived);
     * only the remote-state cache is withheld. Non-ACCEPTED paths unchanged. */
    if(phase==DSP_SETTINGS_ACCEPTED) {
        if(state.req_gen==state.link_gen)
            remember(state.control,state.value,state.value_length,3u,now);
    }
    else if(state.tx_bytes) invalidate(state.control);
    state.finished_ms=now;publish(phase);
}
bool omni_dsp_settings_request(uint32_t token,unsigned control,const uint8_t *value,size_t length,uint32_t now)
{
    uint8_t frame[132];
    if(!token || !encode(control,value,length,frame)) return false;
    ensure_init();
    if(state.token==token && state.phase!=DSP_SETTINGS_IDLE)
        return state.control==control && state.value_length==length && !memcmp(state.value,value,length);
    if(state.fault || (omni_dsp_settings_busy() &&
       (state.phase!=DSP_SETTINGS_QUEUED || state.control!=control))) return false;
    if(state.phase==DSP_SETTINGS_QUEUED) ++state.coalesced;
    state.token=token;state.control=control;state.queued_ms=now;state.req_gen=state.link_gen;
    state.started_ms=state.finished_ms=state.drained_ms=0;
    state.offset=0;state.length=frame[1];state.value_length=(uint8_t)length;
    memcpy(state.frame,frame,state.length);memcpy(state.value,value,length);
    state.tx_bytes=state.ack_count=state.unrelated=state.invalid=0;
    state.cancel=state.ack=state.negative=state.bad_reply=state.verifying=state.matched=false;
    state.peer_status=255u;publish(DSP_SETTINGS_QUEUED);return true;
}
uint32_t omni_dsp_settings_next_token(void)
{
    /* Same cooperative main-loop context as every caller; no atomics needed.
     * Skip 0 (the request-reject sentinel) and keep bit31 SET so internal
     * tokens never alias host diagnostic tokens (<0x80000000). Wrap in-half. */
    static uint32_t next=0x80000000u;
    if(++next<0x80000001u) next=0x80000001u;
    return next;
}

/* Active ANC states 2..4 encode level = 5 - state; states 0/1 (off/transparency)
 * carry no level and must not clobber the last remembered one. One decoder for
 * both the bulk snapshot and the compact D5 report so neither leaves level stale. */
static void remember_anc(uint8_t st,uint32_t now)
{
    remember(DSP_SETTING_ANC_STATE,&st,1,5u,now);
    if(st>=2u && st<=4u) { uint8_t level=(uint8_t)(5u-st);remember(DSP_SETTING_ANC_LEVEL,&level,1,5u,now); }
}

static void observe_db(const uint8_t *p,size_t n,uint32_t now)
{
    if(p[0]!=0xdbu || n<4u) return;
    if(n==5u && p[2]==0xe4u && p[3]==3u) {
        state.link_state=p[4];state.link_ms=now;state.link_valid=true;
        /* Non-connected transition bumps the RF epoch so a SET issued while
         * connected cannot re-validate the cache we clear here when its late
         * ACK finally lands. This clear does NOT cancel the live transaction. */
        if(p[4]!=3u) { ++state.link_gen;for(unsigned i=1;i<DSP_SETTING_COUNT;++i) cache[i].flags=0; }
    } else if(n==5u && p[2]==0x14u && p[3]==3u && p[4]>=0x30u && p[4]<=0x35u) {
        remember(DSP_SETTING_BT_STATE,p+4,1,5u,now);
    } else if(n==6u && p[2]==0xd2u && p[3]==9u && p[4]==3u && p[5]<=1u) {
        /* Also a physical short-click report. Cache only; the separate UI
         * event consumer must deliver every report, including repeated values. */
        remember(DSP_SETTING_HOME_MODE,p+5,1,5u,now);
    } else if(n==6u && p[2]==0xd2u && p[3]==0x0bu && p[4]==3u) {
        /* VP (voice-prompt) level readback. Frame layout (subcmd 0x0B, selector
         * 0x03, 6-byte) confirmed on hardware 2026-09-12: BD 05 D2 0B 02 ->
         * DB 06 D2 0B 03 xx, no pre-NACK. READ ONLY: retain the raw stock byte;
         * range/unit/persistence still unproven so no bound, no writer. Distinct
         * D2 subcommand from master gain (D2/03) and home (D2/09). */
        remember(DSP_SETTING_VP_LEVEL,p+5,1,5u,now);
    } else if(n==6u && p[2]==0xd3u && p[3]==3u && p[4]==1u && p[5]<=1u) {
        remember(DSP_SETTING_MIC_STATE,p+5,1,5u,now);
    } else if(n==6u && p[2]==0xd3u && p[3]==3u && p[4]==2u && p[5]>=1u && p[5]<=10u) {
        remember(DSP_SETTING_MIC_VOLUME,p+5,1,5u,now);
    } else if(n==7u && p[2]==0xd4u && p[3]==1u && p[4]==3u && p[5]<=1u && p[6]>=1u && p[6]<=10u) {
        remember(DSP_SETTING_SIDETONE,p+5,2,5u,now);
    } else if(p[2]==0xd5u && p[3]==3u) {
        if(n==5u && p[4]<=4u) remember_anc(p[4],now);
        else if(n==6u && p[4]==0x10u && p[5]>=1u && p[5]<=10u)
            remember(DSP_SETTING_TRANSPARENCY,p+5,1,5u,now);
        else if(n==6u && p[4]==0x11u && p[5]>=1u && p[5]<=3u)
            remember(DSP_SETTING_ANC_LEVEL,p+5,1,5u,now);
    } else if(n==5u && p[2]==0x43u && p[3]==3u && p[4]>=1u && p[4]<=2u) {
        remember(DSP_SETTING_OUTPUT_MODE,p+4,1,5u,now);
    } else if(n==7u && p[2]==0xdbu && p[3]==3u && p[5]>=1u && p[5]<=3u) {
        /* Compact mic-noise readback; map like bulk20's {enabled!=0,level}.
         * The constructor's 3rd byte is not presumed zero, just not stored. */
        uint8_t noise[2]={(uint8_t)(p[4]!=0u),p[5]};
        remember(DSP_SETTING_MIC_NOISE,noise,2,5u,now);
    } else if(n==9u && p[2]==0xe3u && p[3]==3u && p[4]==1u &&
              p[5]<=1u && p[6]<=10u && p[7]<=2u) {
        /* Selected-field E3 readback: decode only selector1's tuple; the 4th
         * byte stays raw metadata (kept by the query layer), never zero-filled. */
        remember(DSP_SETTING_BT_STARTUP,p+5,3,5u,now);
    } else if(n==46u && p[2]==0x20u && p[3]==1u) {
        /* Stock 0x1e6b0 receives p+2. Full-frame offsets below are verified
         * through its settings consumer and host feature reports. */
        if(p[18]<=1u) remember(DSP_SETTING_LIMITER,p+18,1,5u,now);
        if(p[34]<=1u && p[35]>=1u && p[35]<=10u)
            remember(DSP_SETTING_SIDETONE,p+34,2,5u,now);
        if(p[27]>=1u && p[27]<=3u && p[29]<=3u) {
            uint8_t noise[2]={(uint8_t)(p[29]!=0u),p[27]};
            remember(DSP_SETTING_MIC_NOISE,noise,2,5u,now);
        }
        if(p[12]<=1u && p[20]<=10u && p[13]<=2u) {
            uint8_t siblings[3]={p[12],p[20],p[13]};
            remember(DSP_SETTING_BT_STARTUP,siblings,3,5u,now);
        }
        /* Snapshot EQ fields identify the preset, not its coefficients. */
        if(p[24]<=4u) remember(DSP_SETTING_EQ_WIRELESS,p+24,1,5u,now);
        if(p[26]<=9u) remember(DSP_SETTING_EQ_MIC,p+26,1,5u,now);
        if(p[25]<=4u) remember(DSP_SETTING_EQ_BT,p+25,1,5u,now);
        if(p[5]<=4u) remember_anc(p[5],now);
        if(p[6]>=1u && p[6]<=10u) remember(DSP_SETTING_TRANSPARENCY,p+6,1,5u,now);
        if(p[14]<=1u) remember(DSP_SETTING_MIC_STATE,p+14,1,5u,now);
        if(p[15]>=1u && p[15]<=10u) remember(DSP_SETTING_MIC_VOLUME,p+15,1,5u,now);
        if(minutes(p[22])) remember(DSP_SETTING_AUTO_OFF,p+22,1,5u,now);
        if(p[45]>=1u && p[45]<=2u) remember(DSP_SETTING_OUTPUT_MODE,p+45,1,5u,now);
    }
}
void omni_dsp_settings_observe(uint8_t byte,uint32_t now)
{
    ensure_init();++state.rx_bytes;
    omni_link_expire(&state.parser,now);
    bool eligible=omni_dsp_settings_active() && state.phase!=DSP_SETTINGS_VERIFY_QUEUED && state.offset==state.length;
    if(!state.parser.used) state.frame_eligible=eligible;
    uint32_t ignored=state.parser.ignored;
    const uint8_t *p;size_t n;
    bool complete=omni_link_feed(&state.parser,byte,now,&p,&n);
    if(state.parser.ignored!=ignored && omni_dsp_settings_active()) {
        p=state.parser.bytes;
        if(eligible && state.frame_eligible && p[2]==state.frame[2]) {
            if(p[1]!=3u) { ++state.invalid;state.bad_reply=true;return; }
            ++state.ack_count;state.peer_status=p[3];
            if(p[3]) state.negative=true;else state.ack=true;
        } else ++state.unrelated;
    }
    if(!complete) return;
    observe_db(p,n,now);
    if(!omni_dsp_settings_active()) return;
    if(state.verifying && eligible && state.frame_eligible && p[0]==0xdbu && n>=3u && p[2]==0x43u) {
        if(n!=5u || p[3]!=3u || p[4]!=state.value[0]) { ++state.invalid;state.bad_reply=true; }
        else state.matched=true;
    } else ++state.unrelated;
}
void omni_dsp_settings_io_error(uint32_t now)
{
    ensure_init();state.fault=true;
    state.link_valid=false;
    for(unsigned i=1;i<DSP_SETTING_COUNT;++i) cache[i].flags=0;
    if(omni_dsp_settings_busy()) finish(DSP_SETTINGS_IO_ERROR,now);
}
void omni_dsp_settings_poll(uint32_t now,bool can_start,omni_dsp_settings_io io)
{
    ensure_init();omni_link_expire(&state.parser,now);
    if(!omni_dsp_settings_busy()) return;
    if(state.phase==DSP_SETTINGS_QUEUED) {
        if((uint32_t)(now-state.queued_ms)>=OMNI_DSP_SETTINGS_QUEUE_MS) {
            ++state.timeouts;finish(DSP_SETTINGS_TIMEOUT,now);return;
        }
        if(!can_start || state.parser.used) return;
        state.started_ms=now;publish(DSP_SETTINGS_SEND);
    }
    if(!io.tx || !io.tx_complete) { omni_dsp_settings_io_error(now);return; }
    uint32_t timeout=state.control==DSP_SETTING_OUTPUT_MODE?
        OMNI_DSP_SETTINGS_MODE_TIMEOUT_MS:OMNI_DSP_SETTINGS_TIMEOUT_MS;
    if((uint32_t)(now-state.started_ms)>=timeout) {
        ++state.timeouts;
        if(state.phase==DSP_SETTINGS_SEND || state.phase==DSP_SETTINGS_DRAIN) state.fault=true;
        finish(DSP_SETTINGS_TIMEOUT,now);return;
    }
    if(state.phase==DSP_SETTINGS_VERIFY_QUEUED) {
        if(state.cancel) { finish(DSP_SETTINGS_CANCELLED,now);return; }
        if(!can_start || state.parser.used ||
           (uint32_t)(now-state.drained_ms)<OMNI_DSP_SETTINGS_MODE_SETTLE_MS) return;
        state.frame[0]=0xbd;state.frame[1]=4;state.frame[2]=0x43;state.frame[3]=2;
        state.length=4;state.offset=0;state.ack=state.matched=false;
        state.verifying=true;publish(DSP_SETTINGS_SEND);
    }
    if(state.phase==DSP_SETTINGS_SEND) {
        for(unsigned budget=0;budget<16u && state.offset<state.length;++budget) {
            int r=io.tx(io.context,state.frame[state.offset]);
            if(!r) break;
            if(r!=1) { omni_dsp_settings_io_error(now);return; }
            ++state.offset;++state.tx_bytes;
        }
        if(state.offset==state.length) publish(DSP_SETTINGS_DRAIN);
    }
    if(state.phase==DSP_SETTINGS_DRAIN) {
        int r=io.tx_complete(io.context);
        if(r!=0 && r!=1) { omni_dsp_settings_io_error(now);return; }
        if(r==1) { state.drained_ms=now;publish(DSP_SETTINGS_WAIT); }
    }
    if(state.phase==DSP_SETTINGS_WAIT) {
        if(state.cancel) finish(DSP_SETTINGS_CANCELLED,now);
        else if(state.negative) finish(DSP_SETTINGS_NACK,now);
        else if(state.bad_reply) finish(DSP_SETTINGS_INVALID_REPLY,now);
        else if(state.ack) {
            if(state.control==DSP_SETTING_OUTPUT_MODE && !state.verifying) publish(DSP_SETTINGS_VERIFY_QUEUED);
            else if(!state.verifying || state.matched) finish(DSP_SETTINGS_ACCEPTED,now);
        }
    }
}
void omni_dsp_settings_yield(uint32_t now)
{
    if(!omni_dsp_settings_busy()) return;
    state.cancel=true;
    if(state.phase==DSP_SETTINGS_QUEUED || state.phase==DSP_SETTINGS_VERIFY_QUEUED ||
       (state.phase==DSP_SETTINGS_SEND && !state.offset)) finish(DSP_SETTINGS_CANCELLED,now);
}
void omni_dsp_settings_release(uint32_t now)
{
    ensure_init();
    if(omni_dsp_settings_busy()) finish(DSP_SETTINGS_CANCELLED,now);
    state.parser.used=state.parser.expected=0;state.link_valid=false;
    for(unsigned i=1;i<DSP_SETTING_COUNT;++i) cache[i].flags=0;
}
void omni_dsp_settings_acquire(void)
{ ensure_init();state.fault=false;state.parser.used=state.parser.expected=0; }
bool omni_dsp_settings_link_state(uint8_t *raw,uint32_t *observed_ms)
{
    if(!raw || !observed_ms || !state.link_valid || state.fault) return false;
    *raw=state.link_state;*observed_ms=state.link_ms;return true;
}
bool omni_dsp_settings_status(unsigned page,uint32_t out[15])
{
    if(!out || page>1u) return false;
    uint32_t w[15]={1u,page,state.token,state.control,(uint32_t)state.phase};
    if(!page) {
        w[5]=(uint32_t)omni_dsp_settings_busy()|((uint32_t)omni_dsp_settings_active()<<1)|
             ((uint32_t)state.fault<<2)|((uint32_t)state.cancel<<3)|((uint32_t)state.ack<<4);
        w[6]=state.queued_ms;w[7]=state.started_ms;w[8]=state.finished_ms;
        w[9]=state.tx_bytes;w[10]=state.ack_count;w[11]=state.unrelated;w[12]=state.invalid;
        w[13]=state.initialized?state.peer_status:255u;w[14]=state.coalesced;
    } else {
        w[5]=state.drained_ms;w[6]=state.rx_bytes;w[7]=state.parser.frames;
        w[8]=state.parser.malformed;w[9]=state.parser.expired;w[10]=state.timeouts;
        w[11]=state.length;w[12]=state.value_length;
    }
    memcpy(out,w,sizeof(w));return true;
}
bool omni_dsp_settings_value(unsigned control,unsigned page,uint8_t out[60])
{
    if(!out || control<1u || control>=DSP_SETTING_COUNT || page>3u) return false;
    const cached_value *c=&cache[control];unsigned offset=page*36u;
    uint32_t header[6]={1u,control,page,c->length,c->flags,c->ms};
    memset(out,0,60);memcpy(out,header,sizeof(header));
    if(offset<c->length) {
        unsigned n=(unsigned)c->length-offset;if(n>36u) n=36u;
        memcpy(out+24,c->bytes+offset,n);
    }
    return true;
}

size_t omni_dsp_settings_custom(unsigned control,uint8_t out[128])
{
    if(!eq_control(control) || !out) return 0;
    const cached_value *c=&custom_eq[control-DSP_SETTING_EQ_WIRELESS];
    if(!(c->flags&1u)) return 0;
    memcpy(out,c->bytes,c->length);return c->length;
}
uint32_t omni_dsp_settings_eq_generation(void) { return eq_persist_gen; }
size_t omni_dsp_settings_eq_serialize(uint8_t out[DSP_EQ_NVM_PAYLOAD])
{
    if(!out) return 0;
    memset(out,0,DSP_EQ_NVM_PAYLOAD);
    unsigned off=0;
    for(unsigned ch=0;ch<3u;++ch) {
        unsigned control=DSP_SETTING_EQ_WIRELESS+ch;
        unsigned blen=control==DSP_SETTING_EQ_WIRELESS?128u:78u;
        const cached_value *c=&cache[control];
        if(c->length==blen && (c->flags&1u)) {out[off]=1u;memcpy(out+off+1u,c->bytes,blen);}
        off+=1u+blen;
    }
    return DSP_EQ_NVM_PAYLOAD;
}
void omni_dsp_settings_eq_deserialize(const uint8_t *in,size_t length,uint32_t now)
{
    if(!in || length!=DSP_EQ_NVM_PAYLOAD) return;
    ensure_init();
    unsigned off=0;
    for(unsigned ch=0;ch<3u;++ch) {
        unsigned control=DSP_SETTING_EQ_WIRELESS+ch;
        unsigned blen=control==DSP_SETTING_EQ_WIRELESS?128u:78u;
        if(in[off]==1u) remember(control,in+off+1u,blen,3u,now);
        off+=1u+blen;
    }
}
