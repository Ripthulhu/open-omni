#include "mcu2_runtime.h"
#include <string.h>

static bool elapsed(uint32_t now,uint32_t then,uint32_t interval)
{ return (uint32_t)(now-then)>=interval; }
static void queue(omni_mcu2_runtime *s,omni_mcu2_tx_kind kind,const uint8_t *bytes,uint8_t n)
{
    memcpy(s->prefixes[kind],bytes,n);s->lengths[kind]=n;
    s->pending|=1UL<<(unsigned)kind;
}
bool omni_mcu2_runtime_query(omni_mcu2_runtime *s,omni_mcu2_query query)
{
    if(!s || !s->started || s->fault) return false;
    if(query==OMNI_MCU2_QUERY_VERSION) {
        const uint8_t b[]={0xbc,4,0xe1,2};queue(s,OMNI_MCU2_TX_VERSION,b,sizeof(b));
    } else if(query==OMNI_MCU2_QUERY_DETECT) {
        const uint8_t b[]={0xbc,3,0x87};queue(s,OMNI_MCU2_TX_DETECT,b,sizeof(b));
    } else return false;
    return true;
}
bool omni_mcu2_runtime_gain(omni_mcu2_runtime *s,uint8_t index)
{
    if(!s || !s->started || s->fault || index>12U) return false;
    if(s->have_gain_desired && s->gain_desired==index) return true;
    const uint8_t b[]={0xaa,3,index};
    s->have_gain_desired=true;s->gain_desired=index;++s->gain_generation;
    queue(s,OMNI_MCU2_TX_GAIN,b,sizeof(b));return true;
}
bool omni_mcu2_runtime_init(omni_mcu2_runtime *s,mcu2_link_io io,uint32_t now)
{
    if(!s || !io.tx || !io.rx) return false;
    memset(s,0,sizeof(*s));s->io=io;s->started=true;s->started_ms=now;s->now_ms=now;
    s->last_rx_ms=s->last_frame_ms=now;s->next_detect_ms=now;
    s->trace_first_tx_ms=s->trace_last_tx_ms=UINT32_MAX;
    s->trace_first_rx_ms=s->trace_last_rx_ms=UINT32_MAX;
    (void)omni_mcu2_runtime_query(s,OMNI_MCU2_QUERY_VERSION);
    (void)omni_mcu2_runtime_query(s,OMNI_MCU2_QUERY_DETECT);
    /* Stock AA030C selects coefficient 16383 in MCU2's secondary playback
     * path. This is not the DSP master-volume control or a keepalive. */
    (void)omni_mcu2_runtime_gain(s,12);
    const uint8_t backend[]={0x12,0x63,0};
    queue(s,OMNI_MCU2_TX_BACKEND_QUERY,backend,sizeof(backend));return true;
}
void omni_mcu2_runtime_stop(omni_mcu2_runtime *s)
{
    if(!s) return;
    /* A partially sent fixed envelope cannot be repaired by resetting MCU1's
     * UART alone. Leave a fault latched and never silently resume/retry it. */
    if(s->active && s->tx_used) s->fault=true;
    if(s->selection==OMNI_MCU2_SELECT_QUEUED || s->selection==OMNI_MCU2_SELECT_WAITING)
        s->selection=OMNI_MCU2_SELECT_FAILED;
    s->started=false;s->pending=0;
}
bool omni_mcu2_runtime_lifecycle(omni_mcu2_runtime *s,uint8_t state)
{
    if(!s || !s->started || s->fault || state<1U || state>3U) return false;
    if(s->have_lifecycle && s->lifecycle==state) return true;
    const uint8_t b[]={0xbc,6,0xe4,3,state,0};
    s->have_lifecycle=true;s->lifecycle=state;queue(s,OMNI_MCU2_TX_LIFECYCLE,b,sizeof(b));return true;
}
bool omni_mcu2_runtime_controls(omni_mcu2_runtime *s,uint8_t volume,uint8_t balance,
    uint8_t mic_state,uint8_t mic_percent,uint8_t setting_percent)
{
    if(!s || !s->started || s->fault || volume>100U || balance>100U || mic_state>1U ||
       mic_percent>100U || (setting_percent!=0U && setting_percent!=25U && setting_percent!=50U && setting_percent!=100U)) return false;
    const uint8_t values[]={volume,balance,mic_state,mic_percent,setting_percent};
    if(s->have_controls && !memcmp(values,s->controls,sizeof(values))) return true;
    uint8_t b[]={0xbc,8,0xef,volume,balance,mic_state,mic_percent,setting_percent};
    memcpy(s->controls,values,sizeof(values));s->have_controls=true;
    queue(s,OMNI_MCU2_TX_CONTROLS,b,sizeof(b));return true;
}
bool omni_mcu2_runtime_dsp86(omni_mcu2_runtime *s,uint8_t state)
{
    if(!s || !s->started || s->fault || state>3U) return false;
    if(s->have_dsp86 && s->dsp86==state) return true;
    const uint8_t b[]={0xbc,6,0x86,3,state,0};
    s->have_dsp86=true;s->dsp86=state;
    queue(s,OMNI_MCU2_TX_DSP86,b,sizeof(b));return true;
}
bool omni_mcu2_runtime_select(omni_mcu2_runtime *s,uint8_t side)
{
    if(!s || !s->started || s->fault || side>1U ||
       s->selection==OMNI_MCU2_SELECT_QUEUED || s->selection==OMNI_MCU2_SELECT_WAITING) return false;
    uint32_t detect_age=s->have_detect?(uint32_t)(s->now_ms-s->detect_ms):UINT32_MAX;
    uint32_t state_age=s->have_state?(uint32_t)(s->now_ms-s->state_ms):UINT32_MAX;
    if(detect_age>=OMNI_MCU2_PRESENCE_FRESH_MS && state_age>=OMNI_MCU2_PRESENCE_FRESH_MS) return false;
    /* CB88 is unsolicited and may precede native MCU1 startup. A fresh direct
     * BC0387 reply independently proves presence, but not active side/ready.
     * Prefer the newest presence observation; never manufacture a CB88 state. */
    bool use_detect=detect_age<OMNI_MCU2_PRESENCE_FRESH_MS &&
        (state_age>=OMNI_MCU2_PRESENCE_FRESH_MS || s->latest_presence_is_detect);
    uint8_t ports=use_detect?s->queried_ports:s->detected_ports;
    if(!(ports&(uint8_t)(1U<<side))) return false;
    /* Physical USB2=side0/mode0, USB3=side1/mode2. Selection is a command,
     * followed by a state observation, never an assertion of host enumeration. */
    s->selected_side=side;
    if(state_age<OMNI_MCU2_PRESENCE_FRESH_MS && s->active_mode==(side?2U:0U) && s->ready) {
        s->selection=OMNI_MCU2_SELECT_OBSERVED;return true;
    }
    s->selection=OMNI_MCU2_SELECT_QUEUED;
    const uint8_t b[]={0x12,0x33,side};queue(s,OMNI_MCU2_TX_SELECT,b,sizeof(b));return true;
}
static bool marker(uint8_t value)
{ return value==0xcbU || value==0xccU || value==0xaaU; }
static uint8_t expected(const uint8_t *p)
{
    if(p[0]==0xcbU) {
        switch(p[2]) {
        case 0xe1: return 7;
        case 0x87: case 0x96: return 4;
        case 0x88: return 6;
        case 0x82: return 5;
        default:return 0;
        }
    }
    if(p[0]==0xccU && p[2]==0xd2U) return 6;
    if(p[0]==0xaaU) return 3;
    return 0;
}
static void dispatch(omni_mcu2_runtime *s,const uint8_t *p,uint32_t now)
{
    ++s->rx_frames;s->last_frame_ms=now;
    if(p[0]==0xcbU) {
        switch(p[2]) {
        case 0xe1:
            if(p[3]!=3U) break;
            memcpy(s->version,p+4,3);s->have_version=true;++s->version_generation;
            s->query_replied[0]=s->query_sent[0];return;
        case 0x87:
            if(p[3]==0xa0U || p[3]==0xffU) {
                s->readiness=p[3];s->have_readiness=true;s->ready=p[3]==0xa0U;return;
            }
            if(p[3]<=3U) {
                s->queried_ports=p[3];s->have_detect=true;s->detect_ms=now;
                s->latest_presence_is_detect=true;++s->detect_generation;
                s->query_replied[1]=s->query_sent[1];return;
            }
            break;
        case 0x88:
            if(p[3]>3U || p[4]>2U || p[5]>1U) break;
            s->detected_ports=p[3];s->active_mode=p[4];s->backend_setting=p[5];
            s->have_state=s->have_setting=true;s->state_ms=now;
            s->latest_presence_is_detect=false;++s->state_generation;return;
        case 0x96:
            if(p[3]>1U) break;
            s->backend_setting=p[3];s->have_setting=true;return;
        case 0x82: ++s->ignored_frames;return; /* Retain count, semantics incomplete. */
        default:break;
        }
    } else if(p[0]==0xccU && p[2]==0xd2U) {
        if(p[3]!=1U || p[4]>56U) { ++s->rejected_frames;return; }
        s->secondary_volume=p[4];s->secondary_aux=p[5];++s->controls_generation;return;
    } else if(p[0]==0xaaU) {
        /* Reverse AA is not the same gain setter; avoid inventing a mute. */
        ++s->ignored_frames;return;
    }
    ++s->rejected_frames;
}
static void recover(omni_mcu2_runtime *s)
{
    uint16_t next=1;
    while(next<s->rx_used && !marker(s->rx_frame[next])) ++next;
    s->discarded+=next;
    s->rx_used=(uint16_t)(s->rx_used-next);
    if(s->rx_used) memmove(s->rx_frame,s->rx_frame+next,s->rx_used);
}
static void receive(omni_mcu2_runtime *s,uint8_t byte,uint32_t now)
{
    if(s->rx_used && elapsed(now,s->last_rx_ms,OMNI_MCU2_RUNTIME_GAP_MS)) {
        ++s->gaps;s->discarded+=s->rx_used;s->rx_used=0;
    }
    s->last_rx_ms=now;
    if(!s->rx_used && !marker(byte)) { ++s->discarded;return; }
    s->rx_frame[s->rx_used++]=byte;
    /* Header validation is cheap and rejects arbitrary marker-like noise
     * before waiting for 1KB. Recovered frames consume every padding byte. */
    while(s->rx_used>=3U && (expected(s->rx_frame)==0U ||
          s->rx_frame[1]!=expected(s->rx_frame))) { ++s->rejected_frames;recover(s); }
    if(s->rx_used!=OMNI_MCU2_FRAME_BYTES) return;
    uint8_t n=expected(s->rx_frame);
    bool valid=true;
    for(unsigned i=n;i<OMNI_MCU2_FRAME_BYTES;++i) if(s->rx_frame[i]) { valid=false;break; }
    if(valid) { dispatch(s,s->rx_frame,now);s->rx_used=0; }
    else { ++s->rejected_frames;recover(s); }
}
static void fault(omni_mcu2_runtime *s)
{
    ++s->io_errors;s->fault=true;s->ready=false;
    if(s->selection==OMNI_MCU2_SELECT_QUEUED || s->selection==OMNI_MCU2_SELECT_WAITING)
        s->selection=OMNI_MCU2_SELECT_FAILED;
}
static void complete_tx(omni_mcu2_runtime *s,uint32_t now)
{
    if(s->active==OMNI_MCU2_TX_GAIN) {
        if(!s->tx_idle || !s->tx_idle(s->io.context)) return;
        s->have_gain_sent=true;s->gain_sent=s->active_prefix[2];
        s->gain_sent_generation=s->gain_active_generation;s->gain_sent_ms=now;
    }
    ++s->tx_frames;
    if(s->active==OMNI_MCU2_TX_VERSION) {
        if(s->version_attempts<3U) ++s->version_attempts;
        ++s->query_sent[0];s->version_sent_ms=now;
    }
    if(s->active==OMNI_MCU2_TX_DETECT) ++s->query_sent[1];
    if(s->active==OMNI_MCU2_TX_SELECT) {
        s->selection=OMNI_MCU2_SELECT_WAITING;s->selection_sent_ms=now;
        s->selection_state_generation=s->state_generation;
    }
    s->active=OMNI_MCU2_TX_NONE;s->tx_used=0;
}
void omni_mcu2_runtime_poll(omni_mcu2_runtime *s,uint32_t now)
{
    if(!s || !s->started || s->fault) return;
    s->now_ms=now;
    for(unsigned i=0;i<OMNI_MCU2_RUNTIME_RX_BUDGET;++i) {
        uint8_t byte;int result=s->io.rx(s->io.context,&byte);
        if(result==0) break;
        if(result!=1) { fault(s);return; }
        uint32_t stamp=(uint32_t)(now-s->started_ms);
        if(!s->rx_bytes) s->trace_first_rx_ms=stamp;
        s->trace_last_rx_ms=stamp;++s->rx_bytes;
        if(s->trace_rx_used<OMNI_MCU2_TRACE_RX_BYTES) s->trace_rx[s->trace_rx_used++]=byte;
        else ++s->trace_rx_truncated;
        receive(s,byte,now);
    }
    /* A stale readiness flag is not proof of a currently working peer. The
     * listener remains active so an ordinary late frame can recover status. */
    if(s->have_readiness && elapsed(now,s->last_frame_ms,6000U)) s->ready=false;
    if(!s->have_version && s->version_attempts && s->version_attempts<3U &&
       s->active!=OMNI_MCU2_TX_VERSION && !(s->pending&(1UL<<OMNI_MCU2_TX_VERSION)) &&
       elapsed(now,s->version_sent_ms,OMNI_MCU2_QUERY_TIMEOUT_MS))
        (void)omni_mcu2_runtime_query(s,OMNI_MCU2_QUERY_VERSION);
    if(elapsed(now,s->next_detect_ms,5000U)) {
        (void)omni_mcu2_runtime_query(s,OMNI_MCU2_QUERY_DETECT);s->next_detect_ms=now;
    }
    if(s->selection==OMNI_MCU2_SELECT_WAITING) {
        if(s->have_state && s->state_generation!=s->selection_state_generation && s->ready &&
           s->active_mode==(s->selected_side?2U:0U) &&
           (s->detected_ports&(uint8_t)(1U<<s->selected_side)) &&
           elapsed(now,s->selection_sent_ms,500U)) s->selection=OMNI_MCU2_SELECT_OBSERVED;
        else if(elapsed(now,s->selection_sent_ms,OMNI_MCU2_SELECT_TIMEOUT_MS))
            s->selection=OMNI_MCU2_SELECT_TIMEOUT;
    }
    if(!s->active && s->pending) {
        for(unsigned k=1;k<OMNI_MCU2_TX_COUNT;++k) if(s->pending&(1UL<<k)) {
            s->active=(omni_mcu2_tx_kind)k;s->pending&=~(UINT32_C(1)<<k);
            s->active_length=s->lengths[k];memcpy(s->active_prefix,s->prefixes[k],s->active_length);
            if(s->active==OMNI_MCU2_TX_GAIN) s->gain_active_generation=s->gain_generation;
            s->tx_used=0;s->active_since_ms=now;break;
        }
    }
    if(!s->active) return;
    if(elapsed(now,s->active_since_ms,OMNI_MCU2_QUERY_TIMEOUT_MS)) { fault(s);return; }
    if(s->tx_used==OMNI_MCU2_FRAME_BYTES) { complete_tx(s,now);return; }
    for(unsigned i=0;i<OMNI_MCU2_RUNTIME_TX_BUDGET;++i) {
        uint8_t byte=s->tx_used<s->active_length?s->active_prefix[s->tx_used]:0;
        int result=s->io.tx(s->io.context,byte);
        if(result==0) break;
        if(result!=1) { fault(s);return; }
        uint32_t stamp=(uint32_t)(now-s->started_ms);
        if(!s->tx_bytes) s->trace_first_tx_ms=stamp;
        s->trace_last_tx_ms=stamp;++s->tx_bytes;++s->tx_used;
        if(s->tx_used==OMNI_MCU2_FRAME_BYTES) {
            complete_tx(s,now);break;
        }
    }
}
void omni_mcu2_runtime_status(const omni_mcu2_runtime *s,unsigned page,uint32_t out[15])
{
    memset(out,0,60);out[0]=1;
    if(page==0U) {
        out[1]=(s->started?1U:0U)|(s->fault?2U:0U)|(s->ready?4U:0U)|
            (s->have_version?8U:0U)|(s->have_state?16U:0U)|(s->have_readiness?32U:0U);
        out[2]=s->tx_bytes;out[3]=s->rx_bytes;out[4]=s->tx_frames;out[5]=s->rx_frames;
        out[6]=s->rejected_frames;out[7]=s->ignored_frames;out[8]=s->io_errors;out[9]=s->gaps;
        out[10]=(uint32_t)s->version[0]|((uint32_t)s->version[1]<<8)|((uint32_t)s->version[2]<<16);
        out[11]=s->readiness;
        out[12]=(uint32_t)s->detected_ports|((uint32_t)s->active_mode<<8)|((uint32_t)s->backend_setting<<16);
        out[13]=s->last_frame_ms;out[14]=(uint32_t)s->active;
    } else if(page==1U) {
        out[1]=s->version_generation;out[2]=s->detect_generation;out[3]=s->state_generation;
        out[4]=s->controls_generation;out[5]=s->queried_ports;out[6]=s->lifecycle;
        out[7]=(uint32_t)s->selection;out[8]=s->selected_side;out[9]=s->selection_sent_ms;
        out[10]=(uint32_t)s->secondary_volume|((uint32_t)s->secondary_aux<<8);
        out[11]=s->discarded;out[12]=s->pending;out[13]=s->rx_used;out[14]=s->version_attempts;
    } else if(page==2U) {
        bool active=s->active==OMNI_MCU2_TX_GAIN;
        out[1]=(s->started?1U:0U)|(s->fault?2U:0U)|(s->have_gain_desired?4U:0U)|
            (s->have_gain_sent?8U:0U)|((s->pending&(1UL<<OMNI_MCU2_TX_GAIN))?16U:0U)|
            (active?32U:0U)|((active && s->tx_used==OMNI_MCU2_FRAME_BYTES)?64U:0U);
        out[2]=s->have_gain_desired?s->gain_desired:255U;
        out[3]=s->have_gain_sent?s->gain_sent:255U;
        out[4]=s->gain_generation;out[5]=s->gain_sent_generation;out[6]=s->gain_sent_ms;
        out[7]=active?s->active_prefix[2]:255U;out[8]=s->gain_active_generation;
        out[9]=s->tx_used;out[10]=OMNI_MCU2_QUERY_TIMEOUT_MS;out[11]=s->pending;
        out[12]=s->io_errors;
    }
}
