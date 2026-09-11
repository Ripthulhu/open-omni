"""Compile the real UI renderer/idle code against bounded host display stubs.

No USB, UART, GPIO, SPI or OLED device access. Poll fixtures begin after hardware
initialization and hold GPIO sampling at the current millisecond. Existing
rotary/control modules are linked; physical input behavior is not tested here.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'firmware/mcu1-source'
STUB = r'''
#ifndef TEST_FSL_REGISTERS
#define TEST_FSL_REGISTERS
#include <stdint.h>
typedef struct { volatile uint32_t CTRL,LOAD,VAL; } test_systick;
static test_systick test_tick;
#define SysTick (&test_tick)
#define SysTick_IRQn (-1)
#define SysTick_CTRL_CLKSOURCE_Msk 4u
#define SysTick_CTRL_TICKINT_Msk 2u
#define SysTick_CTRL_ENABLE_Msk 1u
static inline void NVIC_SetPriority(int irq,unsigned priority) {(void)irq;(void)priority;}
#endif
'''
TEST = r'''
#include <assert.h>
#include <stdio.h>
#include "ui.c"

void omni_settings_menu_bind_native(void) {}
static uint8_t bt_raw;static uint32_t bt_flags;
bool omni_dsp_settings_value(unsigned control,unsigned page,uint8_t out[60]) {
 assert(control==DSP_SETTING_BT_STATE && page==0);memset(out,0,60);
 uint32_t h[6]={1,control,0,1,bt_flags,1};memcpy(out,h,24);out[24]=bt_raw;return true;
}
static uint32_t battery=255u,menu_revision,presented;
static unsigned charger;
static bool reject_present,menu_open;
static bool link_known,link_connected,gain_known,gain_ready,headset_ready,headset_fault,audio_known;
static bool meters_available,meters_fresh,meter_mismatch;
static uint32_t meter_generation;
static int32_t meter_values[8];
static omni_source_mix bias;
static uint8_t pixels[1024];
static int16_t host_db=-30*256;
static uint8_t host_mute;
static int dial_sum;
static unsigned mixer_dial_calls;
static unsigned menu_requests,requested_context;
static bool requested_open;
static uint32_t mcu_flags,mcu_mode,mcu_ports,mcu_selection;
uint32_t omni_headset_battery_display(uint32_t now) {(void)now;return battery;}
unsigned omni_charger_indicator(uint32_t now) {(void)now;return charger;}
int omni_charger_adc_millivolts(uint32_t now) {(void)now;return charger?4040:-1;}
bool omni_headset_battery_status(unsigned page,uint32_t now,uint32_t out[15])
{(void)now;assert(page==0);memset(out,0,60);out[2]=link_known?4u:0u;out[8]=link_connected?3u:0u;return true;}
bool omni_native_gain_read(unsigned page,uint32_t out[15])
{assert(page==0);memset(out,0,60);out[2]=(gain_known?1u:0u)|(gain_ready?2u:0u);return true;}
bool omni_native_headset_gain_read(unsigned page,uint32_t out[15])
{assert(page==0);memset(out,0,60);out[3]=(headset_ready?8u:0u)|(headset_fault?128u:0u);return true;}
bool omni_mcu2_runtime_read(unsigned page,uint32_t out[15])
{assert(page<=1);memset(out,0,60);if(!page){out[1]=mcu_flags;out[12]=mcu_ports|(mcu_mode<<8);}else out[7]=mcu_selection;return true;}
void audio_probe_status(uint8_t out[32])
{memset(out,0,32);if(audio_known)out[24]=out[25]=1;}
bool audio_probe_playback_format(omni_audio_format *out)
{assert(omni_audio_format_make(out,48000u,16u,1u));return audio_known;}
void omni_mixer_ui_bias_snapshot(omni_source_mix *state,omni_source_mix_gains *gains)
{if(state)*state=bias;if(gains)assert(omni_source_mix_snapshot(&bias,gains));}
bool omni_dsp_meter_read(unsigned page,uint32_t now,uint8_t out[60])
{
    (void)now;memset(out,0,60);if(!meters_available)return false;
    if(page==0) {
        uint32_t status[15]={0};status[2]=meters_fresh?3u:1u;status[10]=meter_generation;
        memcpy(out,status,60);return true;
    }
    assert(page<=2);uint32_t generation=meter_generation+(page==2 && meter_mismatch?1u:0u);
    memcpy(out+8,&generation,4);
    if(page==1)for(unsigned i=0;i<8;i++) {
        uint32_t raw=(uint32_t)meter_values[i];unsigned pos=16+4*i;
        out[pos]=(uint8_t)(raw>>24);out[pos+1]=(uint8_t)(raw>>16);
        out[pos+2]=(uint8_t)(raw>>8);out[pos+3]=(uint8_t)raw;
    }
    return true;
}
void audio_probe_volume_snapshot(int16_t *db,uint8_t *mute) {*db=host_db;*mute=host_mute;}
void audio_probe_dial(int step) {dial_sum+=step;}
bool omni_mixer_ui_render(uint8_t out[1024]) {if(menu_open)memset(out,0xa5,1024);return menu_open;}
bool omni_mixer_ui_open(void) {return menu_open;}
bool omni_native_menu_request(uint32_t token,bool open,unsigned context,uint32_t now)
{assert(token && token<0x80000000u);(void)now;++menu_requests;requested_open=open;requested_context=context;return true;}
uint32_t omni_mixer_ui_revision(void) {return menu_revision;}
bool omni_mixer_ui_event(omni_control_kind_t kind)
{
    if(kind==OMNI_CONTROL_REMOTE_MENU_ENTER || kind==OMNI_CONTROL_SELECT)menu_open=true;
    else if(kind==OMNI_CONTROL_REMOTE_MENU_EXIT || kind==OMNI_CONTROL_BACK)menu_open=false;
    else if(kind==OMNI_CONTROL_MENU)menu_open=!menu_open;
    else return false;
    ++menu_revision;return true;
}
bool omni_mixer_ui_control(const omni_control_event_t *event)
{
    if(!event)return false;
    if(!menu_open && (event->kind==OMNI_CONTROL_SELECT || event->kind==OMNI_CONTROL_HOME_MODE)) {
        omni_source_mix_toggle(&bias);++menu_revision;return true;
    }
    if(event->kind==OMNI_CONTROL_HOME_MODE)return false;
    return omni_mixer_ui_event(event->kind);
}
bool omni_mixer_ui_dial(int step)
{++mixer_dial_calls;return !menu_open && omni_source_mix_dial(&bias,step);}
omni_display_io omni_display_lpc5528_init(void) {return (omni_display_io){0};}
bool omni_display_init(omni_display *d,omni_display_io io,uint32_t now)
{(void)io;(void)now;d->state=OMNI_DISPLAY_READY;return true;}
void omni_display_poll(omni_display *d,uint32_t now) {(void)d;(void)now;}
bool omni_display_brightness(omni_display *d,unsigned level)
{d->contrast=(uint8_t)(level*23u);return true;}
bool omni_display_present(omni_display *d,const uint8_t out[1024])
{(void)d;if(reject_present)return false;memcpy(pixels,out,1024);++presented;return true;}

static bool pixel(const uint8_t *f,unsigned x,unsigned y)
{return (f[(y/8u)*128u+x]>>(y%8u))&1u;}
static unsigned area(const uint8_t *f,unsigned x,unsigned y,unsigned w,unsigned h)
{unsigned n=0;for(unsigned yy=y;yy<y+h;++yy)for(unsigned xx=x;xx<x+w;++xx)n+=pixel(f,xx,yy);return n;}
static void poll_at(uint32_t now)
{milliseconds=sampled=now;omni_ui_poll();}
static void start(uint32_t now)
{
    initialized=1;display.state=OMNI_DISPLAY_READY;
    shown_db=1;shown_mute=255;shown_charger=255;shown_headset_battery=UINT32_MAX;
    shown_menu=0;shown_ms=0;blanked=0;rendered=presented=0;reject_present=false;menu_open=false;
    battery=255;charger=0;host_db=-30*256;host_mute=0;menu_revision=0;
    settings=0x501u;settings_applied=shown_settings=UINT32_MAX;dial_sum=0;
    menu_requests=0;menu_token=0;remote_rejected=0;mixer_dial_calls=0;
    mcu_flags=mcu_mode=mcu_ports=mcu_selection=0;
    link_known=link_connected=gain_known=gain_ready=headset_ready=headset_fault=audio_known=false;
    meters_available=meters_fresh=meter_mismatch=false;meter_generation=1;
    memset(meter_values,0,sizeof(meter_values));omni_source_mix_init(&bias);
    assert(omni_controls_init(&buttons,0,now));
    omni_ui_idle_init(&idle,now);poll_at(now);
    assert(presented==1 && !blanked && idle.last_activity==now);
}
int main(void)
{
    start(0);
    /* Real128x64 home frame: separators and bottom state are populated. */
    assert(area(pixels,0,9,128,1)==128 && area(pixels,0,54,128,1)==128);
    assert(area(pixels,0,57,36,7)==0 && area(pixels,83,0,15,7)>0);
    assert(pixel(pixels,93,3)); /* Headphone silhouette remains when percentage is unknown. */
    assert(area(pixels,58,0,21,7)==0); /* Spare slot absent. */
    uint8_t baseline[1024];memcpy(baseline,pixels,1024);
    poll_at(99);assert(presented==1); /* No unchanged redraw before100ms. */
    battery=42u|(1u<<8);poll_at(1000);
    assert(presented==2 && idle.last_activity==0 && !blanked);
    assert(area(pixels,93,2,9,3)>0 && area(pixels,98,0,30,7)>0);
    for(unsigned y=0;y<64u;++y)for(unsigned x=0;x<128u;++x)
        if(y>=7u || x<83u) assert(pixel(baseline,x,y)==pixel(pixels,x,y));
    /* Packed headset charging state is cached, but never aliases spare state. */
    battery=42u|(2u<<8);poll_at(2000);
    assert(presented==3 && area(pixels,58,0,21,7)==0 && idle.last_activity==0);
    poll_at(2099);assert(presented==3);
    battery=100u|(3u<<8);poll_at(3000);
    assert(presented==4 && area(pixels,104,0,24,7)>0);
    battery=0u|(1u<<8);poll_at(4000);
    assert(pixel(pixels,93,3) && area(pixels,104,0,24,7)>0);
    reject_present=true;battery=1u|(1u<<8);uint32_t old=shown_headset_battery;
    uint32_t old_ms=shown_ms;
    poll_at(5000);assert(shown_headset_battery==old && shown_ms==old_ms && idle.last_activity==0);
    reject_present=false;poll_at(5001);assert(shown_headset_battery==battery && shown_ms==5001);
    /* Both battery sources may change repeatedly without extending visibility. */
    for(unsigned t=10000;t<60000;t+=10000) {
        battery=(t/1000u)|(2u<<8);charger=2;poll_at(t);assert(idle.last_activity==0);
    }
    assert(area(pixels,58,0,21,7)>0 && area(pixels,83,0,45,7)>0);
    poll_at(60000);assert(blanked && !area(pixels,0,0,128,64));
    uint32_t n=presented;battery=90u|(3u<<8);charger=3;
    poll_at(61000);poll_at(70000);assert(blanked && presented==n && idle.last_activity==0);
    host_db=-29*256;poll_at(70001);
    assert(!blanked && shown_headset_battery==battery && idle.last_activity==70001);
    /* A home status never overwrites the mixer page. */
    menu_open=true;++menu_revision;poll_at(70002);
    for(unsigned i=0;i<1024u;++i)assert(pixels[i]==0xa5);
    battery=255u;poll_at(70003);for(unsigned i=0;i<1024u;++i)assert(pixels[i]==0xa5);
    menu_open=false;++menu_revision;poll_at(70004);assert(area(pixels,83,0,15,7)>0);
    /* Meter snapshots update at100ms, not by waking/extending OLED idle. */
    start(0);memcpy(baseline,pixels,1024);
    meters_available=meters_fresh=true;
    for(unsigned i=0;i<8;i++)meter_values[i]=-4800;
    poll_at(99);assert(presented==1 && !memcmp(pixels,baseline,1024));
    poll_at(100);assert(presented==2 && idle.last_activity==0);
    assert(area(pixels,40,15,20,3)>0 && memcmp(pixels,baseline,1024));
    memcpy(baseline,pixels,1024);meter_values[0]=-9600;meter_values[1]=-9600;++meter_generation;
    poll_at(199);assert(presented==2 && !memcmp(pixels,baseline,1024));
    poll_at(200);assert(presented==3 && idle.last_activity==0);
    assert(!area(pixels,40,15,31,3));
    /* Cross-generation and stale pages render unknown, never mismatched samples. */
    meter_mismatch=true;poll_at(300);assert(area(pixels,53,16,6,1)==6);
    meter_mismatch=false;meters_fresh=false;poll_at(400);assert(area(pixels,53,16,6,1)==6);
    meters_fresh=true;
    for(unsigned t=1000;t<60000;t+=1000) {meter_values[0]=-(int32_t)(t%9600);++meter_generation;poll_at(t);}
    assert(idle.last_activity==0);poll_at(60000);assert(blanked);
    n=presented;++meter_generation;poll_at(60100);assert(blanked && presented==n);
    /* Whole display retries retain render generation/time until accepted. */
    start(0);reject_present=true;poll_at(100);assert(presented==1 && shown_ms==0);
    reject_present=false;poll_at(101);assert(presented==2 && shown_ms==101);
    /* RF and gain readiness are real snapshots; connected wireless cannot
     * show READY until its own gain owner also reports applied state. */
    start(0);memcpy(baseline,pixels,1024);
    link_known=link_connected=gain_known=gain_ready=true;poll_at(100);
    assert(memcmp(pixels,baseline,1024) && idle.last_activity==0);
    memcpy(baseline,pixels,1024);headset_ready=true;poll_at(200);
    assert(memcmp(pixels,baseline,1024) && idle.last_activity==0);
    for(unsigned y=0;y<56u;y++)for(unsigned x=0;x<128u;x++)
        assert(pixel(baseline,x,y)==pixel(pixels,x,y));
    memcpy(baseline,pixels,1024);headset_fault=true;poll_at(300);
    assert(memcmp(pixels,baseline,1024) && pixel(pixels,92,56) && idle.last_activity==0);
    /* Balance names require an observed, ready, present active secondary port.
     * Port detection alone and in-flight/failed switches never claim selection. */
    start(0);omni_source_mix_toggle(&bias);poll_at(100);memcpy(baseline,pixels,1024);
    mcu_ports=3;poll_at(200);assert(!memcmp(pixels,baseline,1024));
    mcu_flags=20;mcu_mode=0;poll_at(300);assert(memcmp(pixels,baseline,1024));
    uint8_t usb2_frame[1024];memcpy(usb2_frame,pixels,1024);
    mcu_mode=2;poll_at(400);assert(memcmp(pixels,usb2_frame,1024));
    mcu_selection=1;poll_at(500);assert(!memcmp(pixels,baseline,1024));
    mcu_selection=2;poll_at(600);assert(!memcmp(pixels,baseline,1024));
    mcu_selection=4;poll_at(700);assert(!memcmp(pixels,baseline,1024));
    mcu_selection=3;mcu_ports=1;poll_at(800);assert(!memcmp(pixels,baseline,1024));
    mcu_ports=3;mcu_flags=16;poll_at(900);assert(!memcmp(pixels,baseline,1024));
    mcu_flags=22;poll_at(1000);assert(!memcmp(pixels,baseline,1024));
    mcu_flags=20;mcu_mode=1;poll_at(1100);assert(!memcmp(pixels,baseline,1024));
    mcu_mode=0;poll_at(1200);assert(!memcmp(pixels,usb2_frame,1024));
    assert(idle.last_activity==0); /* Observations/labels cannot wake the display. */
    /* Timeout arithmetic still works when battery refresh crosses wrap. */
    start(UINT32_MAX-30000u);battery=50u;poll_at(10000u);
    assert(!blanked && idle.last_activity==UINT32_MAX-30000u);
    poll_at(30000u);assert(blanked);
    /* Explicit screen settings wake once; background status still cannot. */
    start(0);assert(omni_ui_settings_set(1,7,1,0));poll_at(1);
    assert(display.contrast==161 && idle.last_activity==1);
    poll_at(60001);assert(!blanked && display.contrast==23 && area(pixels,0,0,128,64)>0);
    battery=48;poll_at(61000);assert(idle.last_activity==1 && display.contrast==23);
    host_db=-28*256;poll_at(61001);assert(display.contrast==161 && idle.last_activity==61001);
    assert(!omni_ui_settings_set(7,7,0,0));assert(!omni_ui_settings_set(1,0,0,0));
    assert(!omni_ui_settings_set(1,7,2,0));assert(!omni_ui_settings_set(1,7,0,2));
    assert(omni_ui_settings_set(0,2,0,1));poll_at(61002);
    assert(area(pixels,0,9,128,1)==128 && area(pixels,0,54,128,1)==128);
    assert(area(pixels,0,27,5,7)>0 && area(pixels,54,13,18,7)>0); /* Stereo L/OUT labels. */
    poll_at(4000000);assert(!blanked && display.contrast==46);
    uint32_t status[15];omni_ui_settings_status(status);
    assert(status[1]==0 && status[2]==2 && status[3]==0 && status[4]==1 && status[6]==0);
    /* Captured remote91 master direction uses its calibrated fallback sign.
     * Menu/bias keep the decoded direction; raw05/06 names imply no mode. */
    const uint8_t raw06[]={0xdb,4,0x91,6},raw05[]={0xdb,4,0x91,5},bad[]={0xdb,4,0xd5,3};
    assert(!omni_ui_remote_frame(bad,4));
    assert(omni_ui_remote_frame(raw06,4));poll_at(4000001);assert(dial_sum==-1);
    assert(omni_ui_remote_frame(raw05,4));poll_at(4000002);assert(dial_sum==0);
    for(unsigned i=0;i<8;i++)assert(omni_ui_remote_frame(raw06,4));
    assert(!omni_ui_remote_frame(raw06,4));poll_at(4000003);assert(dial_sum==-8 && buttons.dropped_events==1);
    const uint8_t enter[]={0xdb,4,0x91,10},exit[]={0xdb,4,0x91,8},back[]={0xdb,4,0x91,7};
    assert(omni_ui_remote_frame(enter,4));poll_at(4000004);assert(menu_open && !menu_requests);
    assert(omni_ui_remote_frame(enter,4));poll_at(4000005);assert(menu_open && !menu_requests);
    assert(omni_ui_remote_frame(exit,4));poll_at(4000006);assert(!menu_open && !menu_requests);
    assert(omni_ui_remote_frame(enter,4));poll_at(4000007);
    assert(omni_ui_remote_frame(back,4));poll_at(4000008);
    assert(!menu_open && menu_requests==1 && !requested_open && requested_context==1);
    buttons.head=0;buttons.count=1;
    buttons.queue[0]=(omni_control_event_t){OMNI_CONTROL_SELECT,OMNI_CONTROL_LOCAL_PUSH,1};
    poll_at(4000009);assert(!menu_open && menu_requests==1 && bias.bias_mode);
    /* Bias endpoint consumes remote dial and cannot spill into master volume. */
    assert(omni_source_mix_configure(&bias,24));
    assert(omni_ui_remote_frame(raw06,4));poll_at(4000009);assert(dial_sum==-8 && bias.position==24);
    buttons.head=0;buttons.count=1;
    buttons.queue[0]=(omni_control_event_t){OMNI_CONTROL_MENU,OMNI_CONTROL_LOCAL_PUSH,1};
    poll_at(4000010);assert(menu_open && menu_requests==2 && requested_open && requested_context==2);
    const uint8_t home_mode[]={0xdb,6,0xd2,9,3,1};
    assert(omni_ui_remote_frame(home_mode,6));poll_at(4000011);
    assert(menu_open && bias.bias_mode && menu_requests==2); /* Home-only peer action. */
    assert(omni_ui_remote_frame(exit,4));poll_at(4000012);
    assert(omni_ui_remote_frame(home_mode,6));poll_at(4000013);
    assert(!menu_open && !bias.bias_mode && menu_requests==2);
    /* Exact four-byte headset source-bias events are context-specific.
     * Late D207/D206 must not reach master volume or menu navigation. */
    const uint8_t bias_down[]={0xdb,4,0xd2,7},bias_up[]={0xdb,4,0xd2,6};
    unsigned calls=mixer_dial_calls;
    assert(omni_ui_remote_frame(bias_down,4));poll_at(4000014);
    assert(bias.position==24 && dial_sum==-8 && mixer_dial_calls==calls);
    assert(omni_ui_remote_frame(bias_up,4));poll_at(4000015);
    assert(bias.position==24 && dial_sum==-8 && mixer_dial_calls==calls);
    assert(omni_ui_remote_frame(home_mode,6));poll_at(4000016);assert(bias.bias_mode);
    assert(omni_source_mix_configure(&bias,12));
    assert(omni_ui_remote_frame(bias_down,4));poll_at(4000017);
    assert(bias.position==11 && dial_sum==-8 && mixer_dial_calls==calls+1u);
    assert(omni_ui_remote_frame(bias_up,4));poll_at(4000018);
    assert(bias.position==12 && dial_sum==-8 && mixer_dial_calls==calls+2u);
    assert(omni_source_mix_configure(&bias,0));
    assert(omni_ui_remote_frame(bias_down,4));poll_at(4000019);
    assert(bias.position==0 && dial_sum==-8);
    assert(omni_source_mix_configure(&bias,24));
    assert(omni_ui_remote_frame(bias_up,4));poll_at(4000020);
    assert(bias.position==24 && dial_sum==-8);
    assert(omni_ui_remote_frame(enter,4));poll_at(4000021);assert(menu_open);
    calls=mixer_dial_calls;uint32_t menu_before=menu_revision;
    assert(omni_ui_remote_frame(bias_down,4));poll_at(4000022);
    assert(menu_open && bias.position==24 && mixer_dial_calls==calls && menu_revision==menu_before);
    assert(omni_ui_remote_frame(bias_up,4));poll_at(4000023);
    assert(menu_open && bias.position==24 && mixer_dial_calls==calls && menu_revision==menu_before);
    assert(dial_sum==-8 && menu_requests==2);
    start(0);link_known=link_connected=true;bt_raw=0x33;bt_flags=5;
    poll_at(100);memcpy(baseline,pixels,1024);
    bt_flags=0;poll_at(200);assert(memcmp(baseline,pixels,128));
    bt_flags=5;link_connected=false;poll_at(300);
    /* A cached Bluetooth ready report is not shown when RF visibility is lost. */
    assert(memcmp(pixels+25,baseline+25,11));
    puts("Real UI home pixels, header batteries, meter refresh/idle, retry, stereo, menu and remote policy passed");
    return 0;
}
'''


def run(cc):
    with tempfile.TemporaryDirectory(prefix='omni-ui-battery-') as tmp:
        directory = Path(tmp)
        (directory/'fsl_device_registers.h').write_text(STUB)
        # The adapter may still be authored in parallel. The UI consumes only
        # this declared packed-view API; no replacement adapter is linked.
        (directory/'headset_battery.h').write_text(
            '#include <stdint.h>\n#include <stdbool.h>\nuint32_t omni_headset_battery_display(uint32_t now);\nbool omni_headset_battery_status(unsigned page,uint32_t now,uint32_t out[15]);\n')
        test = directory/'test.c'
        test.write_text(TEST)
        binary = directory/'test-ui'
        command = [cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   '-I'+str(directory), '-I'+str(SOURCE/'include'), '-I'+str(SOURCE/'src'),
                   str(test), str(SOURCE/'src/controls.c'), str(SOURCE/'src/control_action.c'), str(SOURCE/'src/rotary.c'),
                   str(SOURCE/'src/volume_scale.c'), str(SOURCE/'src/home_ui.c'), str(SOURCE/'src/eq_response.c'), str(SOURCE/'src/source_mix.c'), '-o', str(binary)]
        subprocess.run(command, check=True, capture_output=True, text=True)
        completed = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
        return {'passed': True, 'result': completed.stdout.strip(), 'limits': __doc__}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = run(args.cc)
    rendered = json.dumps(result, indent=2)+'\n'
    if args.output:
        args.output.write_text(rendered, encoding='utf8')
    print(rendered)
