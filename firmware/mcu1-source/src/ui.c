#include "board_clock.h"
#include "ui.h"
#include "display_lpc5528.h"
#include "rotary.h"
#include "audio_probe.h"
#include "volume_scale.h"
#include "charger.h"
#include "charger_adc.h"
#include "headset_battery.h"
#include "controls.h"
#include "control_action.h"
#include "ui_idle.h"
#include "mixer_ui.h"
#include "settings_menu.h"
#include "native_gain_adapter.h"
#include "home_ui.h"
#include "dsp_settings.h"
#include "dsp_meter.h"
#include "mcu2_probe.h"
#include "fsl_device_registers.h"
#include <string.h>
/* 30 Hz leaves room for the 20.5 ms framebuffer transfer at 400 kHz. */
#define UI_FRAME_MS 33u
#define OMNI_UI_EXTERNAL_MS 700u
static volatile uint32_t milliseconds;
static omni_display display;
static omni_rotary rotary;
static uint8_t frame[1024], initialized, phase;
static uint8_t external_frame[1024];
static volatile uint32_t external_ms;
static volatile uint8_t external_pending, external_seen, external_active;
static uint32_t sampled, rendered, steps_positive, steps_negative;
static int16_t shown_db=1;
static uint8_t shown_mute=255;
static unsigned shown_charger=255;
static uint32_t shown_headset_battery=UINT32_MAX;
static uint32_t shown_menu;
static uint32_t shown_ms;
static omni_controls_t buttons;
static uint32_t button_counts[4],button_events[16][3],button_total,button_edges,button_last_edge,button_max_gap;
static uint8_t button_raw;
/* Atomic desired snapshot permits a HID setter to publish without touching
 * display transfers or the main-loop control queue. Defaults retain one minute. */
static volatile uint32_t settings=0x00000501u;
static uint32_t settings_applied=UINT32_MAX,shown_settings=UINT32_MAX;
static uint32_t remote_rejected;
static uint32_t menu_token;
static uint8_t read_buttons(void)
{
    return (uint8_t)((*(volatile uint32_t *)0x4008e100U&1u)|
        ((*(volatile uint32_t *)0x4008e104U>>16)&2u));
}
/* OLED idle blanking (burn-in prevention): blank the panel after this long with no
 * volume change or dial activity; any of those wakes it. */
static omni_ui_idle idle;
static uint8_t blanked;
bool omni_ui_settings_set(unsigned timeout,unsigned brightness,unsigned saver,unsigned home)
{
    if(timeout>6u || brightness<1u || brightness>10u || saver>1u || home>1u) return false;
    settings=timeout|(brightness<<8)|(saver<<16)|(home<<24);
    return true;
}
void omni_ui_settings_status(uint32_t out[15])
{
    if(!out) return;
    uint32_t desired=settings;
    memset(out,0,60);out[0]=1;out[1]=desired&255u;out[2]=(desired>>8)&255u;
    out[3]=(desired>>16)&255u;out[4]=(desired>>24)&255u;
    out[5]=settings_applied;out[6]=idle.timeout_ms;out[7]=display.contrast;
    out[8]=remote_rejected;out[9]=buttons.dropped_events;
}
bool omni_ui_remote_frame(const uint8_t *p,size_t n)
{
    if(!p || n<3u || p[0]!=0xdbu || (p[2]!=0x91u && p[2]!=0xd2u)) return false;
    if(initialized && omni_controls_receive(&buttons,p,n)==OMNI_CONTROL_FRAME_QUEUED) return true;
    if(remote_rejected!=UINT32_MAX) ++remote_rejected;
    return false;
}
void SysTick_Handler(void) { ++milliseconds; }
uint32_t omni_ui_milliseconds(void) { return milliseconds; }
void omni_ui_clock_start(void)
{
    SysTick->CTRL=0; SysTick->LOAD=OMNI_CORE_TICKS_PER_MS-1U; SysTick->VAL=0;
    NVIC_SetPriority(SysTick_IRQn,7);
    SysTick->CTRL=SysTick_CTRL_CLKSOURCE_Msk|SysTick_CTRL_TICKINT_Msk|SysTick_CTRL_ENABLE_Msk;
}
static uint8_t read_phase(void)
{
    return (uint8_t)((*(volatile uint8_t *)0x4008c021U & 1U) |
        ((*(volatile uint8_t *)0x4008c012U & 1U)<<1));
}
static uint8_t meter_level(const uint8_t *p)
{
    uint32_t raw=(uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
    int32_t value;memcpy(&value,&raw,4);
    /* Relative display activity from DSP centidB-like readings. Physical
     * input assignment and absolute meter calibration remain separate RE. */
    if(value<=-9600) return 0;
    if(value>=0) return 100;
    return (uint8_t)((value+9600)/96);
}
static void home_link_snapshot(omni_home_view *view)
{
    uint32_t status[15]={0};
    if(omni_mcu2_runtime_read(0,status) && (status[1]&22u)==20u) {
        unsigned mode=(status[12]>>8)&255u;
        unsigned present=status[12]&3u;
        unsigned source=mode==0u && (present&1u)?2u:mode==2u && (present&2u)?3u:0u;
        if(omni_mcu2_runtime_read(1,status) && (status[7]==0u || status[7]==3u))
            view->secondary_source=(uint8_t)source;
    }
    if(omni_headset_battery_status(0,milliseconds,status)) {
        view->link_known=(status[2]&4u)!=0u;view->connected=view->link_known && status[8]==3u;
    }
    uint8_t bt[60];
    if(view->connected && omni_dsp_settings_value(DSP_SETTING_BT_STATE,0,bt)) {
        uint32_t length,flags;memcpy(&length,bt+12,4);memcpy(&flags,bt+16,4);
        view->bluetooth_known=length==1u && (flags&5u)==5u;
        view->bluetooth_state=bt[24];
    }
    if(omni_native_gain_read(0,status)) {
        view->gain_fault=(status[2]&4u)!=0u;view->gain_known=(status[2]&1u)!=0u;view->gain_ready=(status[2]&2u)!=0u && status[5]==status[7] && status[8]==status[10];
    }
    if(view->connected && omni_native_headset_gain_read(0,status)) {
        view->gain_ready=view->gain_ready && (status[3]&8u)!=0u && status[5]==status[7] && status[8]==status[10];
        view->gain_fault=view->gain_fault || (status[3]&192u)!=0u;
    }
}

/* Display ballistics bridge brief DSP floor readings between meter windows.
 * Keep raw diagnostic samples unchanged; sustained silence still decays. */
static struct {
    uint32_t level[8],peak_ms[8],updated_ms;
    bool valid;
} meter_display;
static void meter_display_update(uint8_t levels[8],uint32_t now)
{
    uint32_t elapsed=now-meter_display.updated_ms;
    for(unsigned i=0;i<8u;++i) {
        uint32_t target=(uint32_t)levels[i]*1000u;
        if(!meter_display.valid || target>=meter_display.level[i]) {
            meter_display.level[i]=target;meter_display.peak_ms[i]=now;
        } else {
            uint32_t age=now-meter_display.peak_ms[i];
            uint32_t decay_ms=age>80u?age-80u:0u;
            if(decay_ms>elapsed) decay_ms=elapsed;
            if(decay_ms>1000u) decay_ms=1000u;
            uint32_t drop=decay_ms*120u; /* 120 percentage points/second. */
            uint32_t gap=meter_display.level[i]-target;
            meter_display.level[i]-=drop<gap?drop:gap;
        }
        levels[i]=(uint8_t)(meter_display.level[i]/1000u);
    }
    meter_display.updated_ms=now;meter_display.valid=true;
}
static void home_meter_snapshot(omni_home_view *view)
{
    uint32_t status[15]={0};
    static const char labels[4][7]={"INPUT1","INPUT2","ANALOG","OUTPUT"};
    for(unsigned i=0;i<4u;++i) memcpy(view->input[i].label,labels[i],7);
    uint8_t first[60],second[60];
    if(omni_dsp_meter_read(0,milliseconds,(uint8_t *)status) && (status[2]&3u)==3u &&
       omni_dsp_meter_read(1,milliseconds,first) && omni_dsp_meter_read(2,milliseconds,second)) {
        uint32_t gen1,gen2;memcpy(&gen1,first+8,4);memcpy(&gen2,second+8,4);
        if(gen1 && gen1==status[10] && gen1==gen2) {
            /* First four actual DSP pairs fit page1; no fabricated levels
             * from fader values. Unqualified routes keep their DSP labels. */
            uint8_t levels[8];
            for(unsigned i=0;i<8u;++i) levels[i]=meter_level(first+16u+i*4u);
            meter_display_update(levels,milliseconds);
            for(unsigned i=0;i<4u;++i) {
                uint8_t left=levels[i*2u],right=levels[i*2u+1u];
                view->input[i].known=true;view->input[i].level=left>right?left:right;
                if(i==3u) {view->stereo_known=true;view->left=left;view->right=right;}
            }
            return;
        }
    }
    meter_display.valid=false;
}

static void render(int16_t db,uint8_t mute,uint32_t headset_battery)
{
    if(omni_mixer_ui_render(frame)) return;
    omni_home_view v={0};memcpy(v.source,"USB1",5);
    v.percent=(uint8_t)omni_volume_percent(db);v.db_x256=db;v.muted=mute!=0u;
    v.battery_percent=(uint8_t)headset_battery;v.battery_known=v.battery_percent<=100u;
    v.spare_state=(uint8_t)omni_charger_indicator(milliseconds);
    int spare_mv=omni_charger_adc_millivolts(milliseconds);
    v.spare_millivolts=spare_mv>0 && spare_mv<=5000?(uint16_t)spare_mv:0u;
    v.stereo_view=((settings_applied>>24)&1u)!=0u;
    omni_source_mix mix;omni_source_mix_gains gains;
    omni_mixer_ui_bias_snapshot(&mix,&gains);
    v.bias_mode=mix.bias_mode;v.bias_known=true;v.bias=mix.position;
    v.bias_q14[0]=gains.q14[0];v.bias_q14[1]=gains.q14[1];
    home_link_snapshot(&v);
    uint8_t audio[32];audio_probe_status(audio);
    omni_audio_format format;
    if(audio[24] && audio[25] && audio_probe_playback_format(&format)) {
        v.sample_rate=format.sample_rate;v.sample_bits=format.sample_bits;
    }
    home_meter_snapshot(&v);
    omni_home_ui_render(frame,&v);
}
void omni_ui_poll(void)
{
    uint32_t now=milliseconds;
    int woke=0;
    if(!initialized) {
        omni_settings_menu_bind_native();
        omni_display_io io=omni_display_lpc5528_init();
        *(volatile uint32_t *)0x40001084U=0x5100U;
        *(volatile uint32_t *)0x40001048U=0x5100U;
        *(volatile uint32_t *)0x4008e004U &= ~(1U<<1);
        *(volatile uint32_t *)0x4008e000U &= ~(1U<<18);
        /* Executed stock input configuration: digital input, no pull/invert.
         * Observe the touch controller's output; never drive its electrode. */
        *(volatile uint32_t *)0x40001000U=0x5100U;
        *(volatile uint32_t *)0x400010c4U=0x5100U;
        *(volatile uint32_t *)0x4008e000U &= ~1u;
        *(volatile uint32_t *)0x4008e004U &= ~(1u<<17);
        button_raw=read_buttons();(void)omni_controls_init(&buttons,button_raw,now);
        phase=read_phase(); (void)omni_rotary_init(&rotary,phase,2);
        (void)omni_display_init(&display,io,now); initialized=1; sampled=now;
        omni_ui_idle_init(&idle,now); blanked=0;
    }
    uint32_t desired_settings=settings;
    if(settings_applied!=desired_settings) {
        (void)omni_ui_idle_timeout(&idle,desired_settings&255u);
        settings_applied=desired_settings;woke=1;
    }
    if(now!=sampled) {
        uint32_t gap=now-sampled;if(gap>button_max_gap)button_max_gap=gap;
        uint8_t raw=read_buttons();
        if(raw!=button_raw){++button_edges;button_last_edge=now;button_raw=raw;}
        unsigned stable=(unsigned)buttons.buttons[0].stable | ((unsigned)buttons.buttons[1].stable<<1);
        (void)omni_controls_sample(&buttons,raw,now);
        if(stable!=((unsigned)buttons.buttons[0].stable | ((unsigned)buttons.buttons[1].stable<<1))) woke=1;
        sampled=now; phase=read_phase(); int step=omni_rotary_sample(&rotary,phase);
        if(step>0)++steps_positive;
        if(step<0)++steps_negative;
        /* Preserve the verified bias/navigation polarity. Master volume uses
         * the opposite action so a clockwise turn raises percent and gain. */
        if(step){ if(!omni_mixer_ui_dial(step)) audio_probe_dial(-step); woke=1; }
    }
    omni_control_event_t event;
    while(omni_controls_pop(&buttons,&event)) {
        if((unsigned)event.kind<4u) ++button_counts[event.kind];
        uint32_t *entry=button_events[button_total%16u];
        entry[0]=++button_total;entry[1]=now;
        entry[2]=(uint32_t)event.kind|((uint32_t)event.origin<<8)|((uint32_t)event.raw<<16);
        bool was_open=omni_mixer_ui_open();
        if(event.kind==OMNI_CONTROL_BIAS_0 || event.kind==OMNI_CONTROL_BIAS_1) {
            omni_source_mix mix;omni_mixer_ui_bias_snapshot(&mix,NULL);
            /* Headset sends a distinct D206/07 pair while in source-bias
             * context. Late packets cannot change master volume or menus. */
            if(!was_open && mix.bias_mode)
                (void)omni_mixer_ui_dial(event.kind==OMNI_CONTROL_BIAS_0?-1:1);
        } else if(event.kind==OMNI_CONTROL_DIAL_0 || event.kind==OMNI_CONTROL_DIAL_1) {
            /* Keep raw05/06 menu and bias actions; physical headset master
             * direction is opposite (05 raises volume,06 lowers it). */
            int step=omni_control_headset_step(&event);
            if(!omni_mixer_ui_dial(step)) audio_probe_dial(-step);
        } else (void)omni_mixer_ui_control(&event);
        bool is_open=omni_mixer_ui_open();
        /* Peer enter/exit is already echoed by the serialized RX client.
         * Root transitions only:92 is not inferred from submenu depth. */
        if(was_open!=is_open && event.kind!=OMNI_CONTROL_REMOTE_MENU_ENTER &&
           event.kind!=OMNI_CONTROL_REMOTE_MENU_EXIT) {
            menu_token=(menu_token+1u)&0x7fffffffu;if(!menu_token)menu_token=1u;
            (void)omni_native_menu_request(menu_token,is_open,is_open?2u:1u,now);
        }
        woke=1;
    }
    int16_t db; uint8_t mute; audio_probe_volume_snapshot(&db,&mute);
    unsigned charging=omni_charger_indicator(now);
    uint32_t headset_battery=omni_headset_battery_display(now);
    /* Observe user state independently of display readiness/redraw retries. */
    int ext=external_seen && (uint32_t)(now-external_ms)<OMNI_UI_EXTERNAL_MS;
    int awake=omni_ui_idle_update(&idle,now,db,mute,woke!=0 || ext);
    omni_display_poll(&display,now);
    unsigned brightness=(settings_applied>>8)&255u;
    bool dim=!awake && ((settings_applied>>16)&1u);
    if(dim) brightness=1;
    /* Contrast updates never steal the SPI owner in the middle of pixels. */
    if(display.state==OMNI_DISPLAY_READY && display.contrast!=brightness*23u)
        (void)omni_display_brightness(&display,brightness);
    if(ext) {
        if(display.state==OMNI_DISPLAY_READY && external_pending && omni_display_present(&display,external_frame)) external_pending=0u;
        external_active=1u; return;
    }
    if(external_active) { external_active=0u; blanked=0; shown_db=1; shown_ms=now-UI_FRAME_MS; }
    if(display.state==OMNI_DISPLAY_READY) {
        if(!awake && !dim) {
            if(!blanked) {
                memset(frame,0,sizeof(frame));
                if(omni_display_present(&display,frame)) blanked=1;
            }
        } else if(db!=shown_db || mute!=shown_mute || charging!=shown_charger || headset_battery!=shown_headset_battery || blanked || shown_menu!=omni_mixer_ui_revision() || shown_settings!=settings_applied || (awake && (uint32_t)(now-shown_ms)>=UI_FRAME_MS)) {
            render(db,mute,headset_battery);
            if(omni_display_present(&display,frame)) {
                shown_db=db;shown_mute=mute;shown_charger=charging;shown_headset_battery=headset_battery;shown_menu=omni_mixer_ui_revision();shown_settings=settings_applied;++rendered;blanked=0;shown_ms=now;
            }
        }
    }
}
bool omni_ui_external_chunk(unsigned offset,const uint8_t *data,unsigned count,uint32_t now)
{
    if(!data || count>57u || offset+count>1024u) return false;
    memcpy(external_frame+offset,data,count);
    if(offset+count==1024u) { external_ms=now; external_seen=1u; external_pending=1u; }
    return true;
}
void omni_ui_status(uint8_t out[60])
{
    uint32_t values[]={2,milliseconds,initialized,(uint32_t)display.state,phase,
        steps_positive,steps_negative,rotary.invalid,rendered,omni_volume_percent(shown_db),
        idle.last_activity,blanked,(uint32_t)(milliseconds-idle.last_activity)};
    memset(out,0,60);memcpy(out,values,sizeof(values));
}
void omni_ui_buttons_status(unsigned page,uint32_t out[15])
{
    uint32_t v[15]={1,milliseconds,button_raw,
        (uint32_t)buttons.buttons[0].stable|((uint32_t)buttons.buttons[1].stable<<1),
        button_max_gap,buttons.dropped_events,button_counts[0],button_counts[1],button_counts[2],button_counts[3],
        button_edges,button_last_edge,0,0,button_total};
    if(page>0 && page<=4) {
        memset(v,0,sizeof(v));v[0]=1;v[1]=page;v[2]=button_total;
        memcpy(v+3,button_events[(page-1u)*4u],48);
    }
    memcpy(out,v,sizeof(v));
}
