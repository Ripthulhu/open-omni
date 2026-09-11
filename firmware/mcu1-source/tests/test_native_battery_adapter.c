#include "native_gain_adapter.h"
#include "headset_battery.h"
#include "headset_query.h"
#include "dsp_meter.h"
#include "dsp_settings.h"
#include "remote_menu.h"
#include "headset_gain.h"
#include "control_uart.h"
#include "fsl_device_registers.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

test_usart test_uart3={USART_STAT_TXIDLE_MASK,USART_FIFOSTAT_TXEMPTY_MASK};
static struct {
    uint8_t current[4],pending[4],frame[8],rx[2048],menu_frames[128][4];
    unsigned used,read,available,opens,closes,commands,sets,queries,settings,meters,tx_budget,mode,links,menus,mode_sets;
    unsigned headset_sets,headset_gets,headset_primes,peer_writes;uint8_t headset_level,peer_state,master_mute;
    int16_t master_db;uint32_t master_revision;
    bool drop_link,drop_gain_reply,override_headset_get;uint8_t headset_get_value;
    uint32_t now,due,last_tx_end,query_start,mode_due;
    uint32_t menu_times[128];
    bool owned,scheduled,ready,rx_error,drop_battery,drop_settings,drop_meters,had_tx,nack_mode_set,mode_scheduled;
} bus={.current={100,37,42,63},.peer_state=3,.master_db=-30*256,.master_revision=1};
static uint8_t target[4]={30,0,40,0};
static void push(const uint8_t *p,unsigned n)
{
    if(bus.read==bus.available) bus.read=bus.available=0;
    assert(bus.available+n<=sizeof(bus.rx));memcpy(bus.rx+bus.available,p,n);bus.available+=n;
}
static int tx(void *context,uint8_t b)
{
    (void)context;assert(bus.owned);
    if(!bus.tx_budget) return 0;
    --bus.tx_budget;
    if(!bus.used && bus.had_tx) assert((uint32_t)(bus.now-bus.last_tx_end)>=20u);
    assert(bus.used<8);bus.frame[bus.used++]=b;
    if(bus.used<2 || bus.used<bus.frame[1]) return 1;
    ++bus.commands;uint8_t op=bus.frame[2];
    if(op==0xe4) {
        assert(bus.used==4 && bus.frame[3]==2);++bus.links;
        if(!bus.drop_link)push((uint8_t[]){0xdb,5,0xe4,3,bus.peer_state},5);
    } else if(op==0xe2) {
        assert(bus.used==5 && bus.frame[3]==2 && (bus.frame[4]==1 || bus.frame[4]==2));
        ++bus.queries;bus.query_start=bus.now;
        if(!bus.drop_battery) {
            if(bus.frame[4]==1) push((uint8_t[]){0xdb,8,0xe2,3,1,15,10,88},8);
            else push((uint8_t[]){0xdb,6,0xe2,3,2,1},6);
        }
    } else if(op==0x91 || op==0x92) {
        uint32_t status[15];assert(omni_native_gain_read(2,status));
        assert(bus.used==4 && bus.mode==2 && status[3] && bus.ready);
        assert((op==0x91 && (bus.frame[3]==9 || bus.frame[3]==10)) ||
               (op==0x92 && (bus.frame[3]==1 || bus.frame[3]==2)));
        assert(bus.menus<128);memcpy(bus.menu_frames[bus.menus],bus.frame,4);
        bus.menu_times[bus.menus++]=bus.now;
        push((uint8_t[]){0xdd,3,op,0},4);
    } else if(op==0x50) {
        assert(bus.used==4 && bus.frame[3]==2);++bus.meters;
        if(!bus.drop_meters) {
            uint8_t r[76]={0xdb,76,0x50,3};
            for(unsigned i=4;i<76;i+=4) { r[i]=0xff;r[i+1]=0xff;r[i+2]=0xda;r[i+3]=0x80; }
            push(r,sizeof(r));
        }
    } else if(op==0xe1 || op==0x20) {
        assert(bus.used==4 && bus.frame[3]==(op==0x20?1:2));
        if(omni_headset_query_active()) ++bus.settings;else {assert(op==0x20);++bus.headset_primes;}
        if(!bus.drop_settings) {
            uint8_t r[46]={0xdb,0,0,0};r[1]=op==0x20?46:13;r[2]=op;r[3]=op==0x20?1:3;
            for(unsigned i=4;i<r[1];++i) r[i]=(uint8_t)i;
            if(op==0x20) {r[4]=bus.headset_level;r[45]=(uint8_t)bus.mode;}
            push(r,r[1]);
            push((uint8_t[]){0xdb,8,0xe2,3,1,15,10,87},8);
        }
    } else if(op==0xd2) {
        assert(bus.used==5u);
        if(bus.frame[3]==1u) {
            uint32_t status[15];assert(omni_native_gain_read(0,status) && (status[2]&2u));
            assert(omni_native_gain_read(2,status) && status[3]);
            assert(omni_native_headset_gain_read(0,status) && (status[3]&2u));
            assert(bus.frame[4]<=56u);bus.headset_level=bus.frame[4];++bus.headset_sets;
            push((uint8_t[]){0xdd,3,0xd2,0},4);
        } else {
            assert(bus.frame[3]==2u && bus.frame[4]==0u);++bus.headset_gets;
            if(bus.override_headset_get) {
                bus.headset_level=bus.headset_get_value;bus.override_headset_get=false;
            }
            push((uint8_t[]){0xdb,6,0xd2,3,0,bus.headset_level},6);
        }
    } else {
        assert(op==0x43 || op==0x47);
        if(bus.frame[3]==1) {
            if(op==0x43) {
                assert(bus.used==5 && bus.frame[4]==2u);++bus.mode_sets;
                if(!bus.nack_mode_set) {bus.mode_due=bus.now+500u;bus.mode_scheduled=true;}
            }
            else {assert(bus.used==8);++bus.sets;
            memcpy(bus.pending,bus.frame+4,4);bus.due=bus.now+200u;bus.scheduled=true;}
        } else {
            assert(bus.frame[3]==2 && bus.used==4);
            if(op==0x43) {if(!bus.drop_gain_reply)push((uint8_t[]){0xdb,5,0x43,3,(uint8_t)bus.mode},5);}
            else { uint8_t r[8]={0xdb,8,0x47,3};memcpy(r+4,bus.current,4);push(r,8); }
        }
        if(!(op==0x43u && bus.frame[3]==2u && bus.drop_gain_reply))
            push((uint8_t[]){0xdd,3,op,(uint8_t)(op==0x43u && bus.frame[3]==1u && bus.nack_mode_set)},4);
    }
    bus.last_tx_end=bus.now;bus.had_tx=true;bus.used=0;return 1;
}
static int rx(void *context,uint8_t *b)
{
    (void)context;assert(bus.owned);if(bus.rx_error) return -1;
    if(bus.read==bus.available) return 0;
    *b=bus.rx[bus.read++];return 1;
}
bool omni_control_uart_start(unsigned port,mcu2_link_io *out)
{
    assert(port==3 && !bus.owned);bus.owned=true;++bus.opens;
    bus.read=bus.available=bus.used=0;bus.had_tx=false;
    *out=(mcu2_link_io){0,tx,rx};return true;
}
void omni_control_uart_stop(unsigned port)
{ assert(port==3 && bus.owned);bus.owned=false;++bus.closes; }
void omni_control_uart_stats(unsigned port,uint32_t out[6])
{ assert(port==3);memset(out,0,6*sizeof(*out)); }
void audio_probe_volume_snapshot(int16_t *db,uint8_t *mute) { *db=bus.master_db;*mute=bus.master_mute; }
void audio_probe_master_snapshot(int16_t *db,uint8_t *mute,uint32_t *revision)
{ audio_probe_volume_snapshot(db,mute);*revision=bus.master_revision; }
int audio_probe_peer_volume(int16_t db,uint8_t mute,uint32_t revision)
{
    if(revision!=bus.master_revision)return -2;
    bus.master_db=db;bus.master_mute=mute;++bus.master_revision;++bus.peer_writes;return 0;
}
void omni_mixer_ui_targets(int16_t db,bool mute,uint8_t levels[4])
{ (void)db;(void)mute;memcpy(levels,target,4); }
void usb_audio_ring_gain_ready(bool ready) { bus.ready=ready; }
static void prepare(uint32_t now,unsigned budget)
{
    bus.now=now;bus.tx_budget=budget;
    if(bus.mode_scheduled && (uint32_t)(now-bus.mode_due)<0x80000000u) {
        bus.mode=2u;bus.mode_scheduled=false;
    }
    if(bus.scheduled && (uint32_t)(now-bus.due)<0x80000000u) {
        memcpy(bus.current,bus.pending,4);bus.scheduled=false;
    }
}
static void tick(uint32_t now) { prepare(now,8);omni_native_gain_service(now,true); }
static uint32_t transactions(void)
{ uint32_t w[15];assert(omni_native_gain_read(0,w));return w[11]; }
static uint32_t query_phase(void)
{ uint32_t w[15];assert(omni_headset_query_status(0,bus.now,w));return w[4]; }
static uint32_t menu_phase(void)
{ uint32_t w[15];assert(omni_native_menu_read(0,w));return w[2]; }
static void stable(uint32_t first,uint32_t last)
{ for(uint32_t n=first;n<last;++n) { tick(n);assert(bus.ready); } }
int main(void)
{
    uint32_t w[15];
    assert(omni_native_gain_release(0));assert(!bus.opens && !bus.closes);
    assert(!omni_native_menu_request(0,true,2,0));
    assert(!omni_native_menu_request(0x80000001u,true,2,0));
    assert(!omni_native_menu_request(0x100u,true,3,0));
    assert(omni_native_menu_request(0x100u,true,2,0));
    for(uint32_t n=1;n<1300;++n) tick(n);
    assert(bus.ready && bus.opens==1 && !bus.closes && bus.queries==2 && bus.sets==1);
    assert(bus.headset_primes==1u && bus.headset_sets==1u && bus.headset_gets==1u && bus.headset_level==26u);
    assert(!memcmp(bus.current,(uint8_t[]){30,37,40,63},4));
    assert(omni_headset_battery_display(1300)==(88u|(2u<<8)));
    assert(bus.menus==2 && menu_phase()==REMOTE_MENU_ACCEPTED);
    assert(!memcmp(bus.menu_frames[0],(uint8_t[]){0xbd,4,0x91,10},4));
    assert(!memcmp(bus.menu_frames[1],(uint8_t[]){0xbd,4,0x92,2},4));
    assert(bus.menu_times[1]-bus.menu_times[0]>=20u);
    stable(1300,1400); /* no periodic close/reopen or gain_ready drop */

    /* Exactly32 bytes ending on a complete frame still leave exhaustion unproven. */
    for(unsigned i=0;i<4;++i) push((uint8_t[]){0xdb,8,0xe2,3,1,15,10,55},8);
    uint32_t before=transactions();target[0]=31;tick(1400);
    assert(transactions()==before && (omni_headset_battery_display(1400)&255u)==55);
    tick(1401);assert(transactions()==before+1);
    /* Battery telemetry interleaves with the real gain parser, never steals its replies. */
    push((uint8_t[]){0xdb,6,0xe2,3,2,2,0xdb,5,0x91,3,1},11);
    stable(1402,1800);assert(bus.current[0]==31);
    assert(omni_headset_battery_display(1800)==(55u|(3u<<8)));

    /* A new logical transaction must wait for a fragmented unsolicited frame. */
    push((uint8_t[]){0xdb,8},2);target[0]=32;before=transactions();tick(1800);
    assert(transactions()==before && omni_headset_battery_frame_pending());
    stable(1801,1805);assert(transactions()==before);
    push((uint8_t[]){0xe2,3,1,15,10,54},6);tick(1805);
    assert(transactions()==before+1);stable(1806,2200);assert(bus.current[0]==32);

    /* Release cooperatively completes a partial gain SET, then closes even
     * though native gain's logical transaction has already released itself. */
    target[0]=33;tick(2200);uint32_t now=2201;
    while(bus.used!=5 || bus.frame[2]!=0x47 || bus.frame[3]!=1 || bus.sets!=3) {
        prepare(now++,1);omni_native_gain_service(bus.now,true);assert(now<2300);
    }
    unsigned closes=bus.closes;bool released=false;
    for(;now<2800;++now) {
        prepare(now,1);released=omni_native_gain_release(now);
        if(released) break;
        assert(bus.closes==closes);
    }
    assert(released && bus.closes==closes+1 && !bus.owned && bus.current[0]==33);
    assert(omni_headset_battery_display(now)==255);
    /* Diagnostic captures/MCU2 probes may now own the shared transport until
     * their caller resumes ordinary service. No UART is retained by gain. */
    for(uint32_t n=now+1;n<now+1300;++n) tick(n);
    assert(bus.ready && bus.opens==2 && bus.queries==4);
    now+=1300;

    /* Due battery polls lose priority to overdue/dirty gain verification. */
    now+=30000;target[0]=34;unsigned queries=bus.queries;tick(now++);
    assert(bus.queries==queries);
    bus.drop_battery=true;
    while(!omni_headset_battery_busy() || bus.queries==queries) { tick(now++);assert(now<44000); }
    assert(bus.current[0]==34 && bus.queries==queries+1 && bus.ready);
    target[0]=35;before=transactions();closes=bus.closes;uint32_t began=now;
    do {
        prepare(now,8);released=omni_native_gain_release(now++);
        if(!released) assert(bus.ready && bus.closes==closes && transactions()==before);
    } while(!released && now-began<300u);
    assert(released && now-began>=248u && bus.closes==closes+1);
    assert(omni_headset_battery_status(0,now,w) && w[11]==1 && !(w[2]&256u));

    bus.drop_battery=false;
    for(uint32_t n=now;n<now+1300;++n) tick(n);
    now+=1300;
    assert(bus.ready && bus.current[0]==35);
    target[0]=36;tick(now++);bus.rx_error=true;tick(now++);
    assert(!bus.owned && !bus.ready && omni_headset_battery_display(now)==255);
    assert(omni_native_gain_read(0,w) && (w[2]&4u) && !(w[2]&8u));
    unsigned fault_opens=bus.opens;
    for(unsigned i=0;i<5u;++i) {tick(now++);assert(!bus.owned && !bus.ready && bus.opens==fault_opens);}
    omni_native_gain_service(now++,false);bus.rx_error=false;
    for(uint32_t n=now;n<now+1300;++n) tick(n);
    assert(bus.owned && bus.ready && bus.current[0]==36);
    now+=1300;

    /* A queued manual read survives initial gain acquisition,then yields the
     * due battery poll before claiming the same physical UART listener. The
     * charge followup also gets its next eligible slot,without starvation. */
    omni_native_gain_service(now++,false);queries=bus.queries;target[0]=37;
    assert(omni_headset_query_request(800,3,now));unsigned settings=bus.settings,links=bus.links;
    while(bus.settings==settings) {
        tick(now++);assert(now<46000);
        if(bus.settings>settings) assert(bus.links>links && bus.ready);
    }
    assert(query_phase()==HEADSET_QUERY_WAIT);
    tick(now++);assert(query_phase()==HEADSET_QUERY_WAIT && bus.ready); /*46byte reply exceeds drain budget */
    tick(now++);assert(query_phase()==HEADSET_QUERY_DONE && bus.ready);
    uint8_t raw[60];assert(omni_headset_query_reply(800,raw) && raw[8]==46 && raw[14]==0x20);
    assert((omni_headset_battery_display(now)&255u)==87);
    while(bus.queries<queries+2 || omni_headset_battery_busy()) {
        tick(now++);assert(now<46000 && bus.ready);
    }

    /* A pending explicit request cannot block a dirty gain transaction. */
    target[0]=38;before=transactions();settings=bus.settings;
    assert(omni_headset_query_request(801,1,now));
    while(transactions()==before) { tick(now++);assert(bus.settings==settings && bus.ready && now<47000); }
    assert(transactions()==before+1);
    while(query_phase()!=HEADSET_QUERY_DONE) { tick(now++);assert(now<47000 && bus.ready); }
    assert(bus.current[0]==38 && bus.settings==settings+1);

    /* A whole32-byte stale backlog at an exact frame boundary must drain and
     * be observed empty before the new query is transmitted. */
    for(unsigned i=0;i<4;++i) push((uint8_t[]){0xdb,8,0xe2,3,1,15,10,55},8);
    settings=bus.settings;assert(omni_headset_query_request(802,1,now));tick(now++);
    assert(bus.settings==settings && query_phase()==HEADSET_QUERY_QUEUED);
    while(query_phase()!=HEADSET_QUERY_DONE) { tick(now++);assert(now<48000 && bus.ready); }
    assert(bus.settings==settings+1);

    /* Missing full-TX reply is a diagnostic timeout; audio remains ready. */
    bus.drop_settings=true;assert(omni_headset_query_request(803,1,now));
    while(omni_headset_query_busy()) { tick(now++);assert(now<49000 && bus.ready); }
    assert(query_phase()==HEADSET_QUERY_TIMEOUT && !omni_headset_query_transport_fault());
    bus.drop_settings=false;

    /* A cooperative diagnostic reservation finishes a partially sent read
     * through TXIDLE before releasing the physical UART. */
    assert(omni_headset_query_request(804,1,now));
    while(query_phase()==HEADSET_QUERY_QUEUED) {
        prepare(now++,2);omni_native_gain_service(bus.now,true);assert(now<49000);
    }
    assert(query_phase()==HEADSET_QUERY_SEND && bus.used==2);
    closes=bus.closes;released=false;
    while(!released) {
        prepare(now,1);released=omni_native_gain_release(now++);
        if(!released) assert(bus.owned && bus.closes==closes);
        assert(now<49000);
    }
    assert(query_phase()==HEADSET_QUERY_CANCELLED && !bus.owned && bus.closes==closes+1);
    assert(omni_headset_query_request(805,1,now));omni_native_gain_service(now++,false);
    assert(query_phase()==HEADSET_QUERY_CANCELLED);

    for(uint32_t n=now;n<now+1300;++n) tick(n);
    now+=1300;settings=bus.settings;
    assert(omni_headset_query_request(806,1,now));uint32_t queued=now;
    while(omni_headset_query_busy()) {
        for(unsigned i=0;i<4;++i) push((uint8_t[]){0xdb,8,0xe2,3,1,15,10,55},8);
        tick(now++);assert(now-queued<=2001u && bus.ready);
    }
    assert(query_phase()==HEADSET_QUERY_TIMEOUT && bus.settings==settings);
    assert(omni_headset_query_status(1,now,w) && w[11]==1);

    /* Periodic76-byte meter replies coexist with all preceding gain/battery/
     * manual-query cases. A missing response leaves the audio owner ready. */
    assert(bus.meters>0);uint8_t meter[60];
    assert(omni_dsp_meter_read(0,now,meter));memcpy(w,meter,sizeof(w));
    assert(w[10]>0);uint32_t timeouts=w[5],io_errors=w[13];
    bus.drop_meters=true;unsigned meters=bus.meters;closes=bus.closes;
    while(bus.meters==meters) { tick(now++);assert(now<53000 && bus.ready); }
    do {
        tick(now++);assert(now<54000 && bus.ready && bus.closes==closes);
        assert(omni_dsp_meter_read(0,now,meter));memcpy(w,meter,sizeof(w));
    } while(w[5]==timeouts);
    assert(!omni_dsp_meter_transport_fault());bus.drop_meters=false;

    /* A diagnostic reservation cannot close UART halfway through BD045002. */
    while(!omni_dsp_meter_busy() || bus.used!=2) {
        prepare(now++,2);omni_native_gain_service(bus.now,true);assert(now<55000 && bus.ready);
    }
    closes=bus.closes;released=false;
    while(!released) {
        prepare(now,1);released=omni_native_gain_release(now++);
        if(!released) assert(bus.owned && bus.closes==closes);
        assert(now<55500);
    }
    assert(!bus.used && !bus.owned && bus.closes==closes+1 && !omni_dsp_meter_transport_fault());
    for(uint32_t n=now;n<now+1300;++n) tick(n);
    now+=1300;assert(bus.ready && bus.owned);
    assert(omni_dsp_meter_read(0,now,meter));memcpy(w,meter,sizeof(w));
    assert((w[2]&3u)==3u && w[13]==io_errors);

    /* Windows may cancel SET_INTERFACE while startup is waiting for this
     * partial meter request. Ordinary service must finish the abandoned
     * reservation and reacquire, not leave all polling permanently yielded. */
    while(!omni_dsp_meter_busy() || bus.used!=2) {
        prepare(now++,2);omni_native_gain_service(bus.now,true);assert(now<56500);
    }
    closes=bus.closes;meters=bus.meters;uint32_t generation=w[10];
    prepare(now,0);assert(!omni_native_gain_release(now++));
    /* Caller abandons release here and returns to the ordinary idle loop. */
    test_uart3.STAT=0;
    for(uint32_t n=now;n<now+5;++n) {
        tick(n);assert(bus.owned && bus.closes==closes && bus.meters==meters+1 && !bus.used);
    }
    now+=5;test_uart3.STAT=USART_STAT_TXIDLE_MASK;
    for(uint32_t n=now;n<now+1300;++n) tick(n);
    now+=1300;
    assert(bus.closes==closes+1 && bus.owned && bus.ready && bus.meters>meters+1);
    assert(omni_dsp_meter_read(0,now,meter));memcpy(w,meter,sizeof(w));
    assert((w[2]&3u)==3u && !(w[2]&64u) && w[13]==io_errors && w[10]>generation);
    /* A volume change during mode SET->GET cannot prevent the current settings
     * owner finishing verification (nor allow gain bytes inside its frame). */
    assert(omni_dsp_settings_request(0x1234u,DSP_SETTING_OUTPUT_MODE,(uint8_t[]){2},1,now));
    uint32_t settings_start=now;
    do {
        tick(now++);assert(now-settings_start<1250u);
        assert(omni_dsp_settings_status(0,w));
    } while(w[4]!=DSP_SETTINGS_VERIFY_QUEUED);
    target[0]=(uint8_t)(target[0]+1u);
    while(omni_dsp_settings_busy()) {tick(now++);assert(now-settings_start<1250u);}
    assert(omni_dsp_settings_status(0,w) && w[4]==DSP_SETTINGS_ACCEPTED);
    for(uint32_t n=now;n<now+750u;++n) tick(n);
    now+=750u;
    assert(bus.ready && bus.current[0]==target[0]);

    /* Menu91 and92 retain exclusive ownership across their20ms gap even
     * while rapid volume changes make the normal gain-settled gate false. */
    unsigned menus=bus.menus;before=transactions();
    assert(omni_native_menu_request(0x101u,false,1,now));uint32_t menu_start=now;
    while(menu_phase()!=REMOTE_MENU_GAP) {tick(now++);assert(now-menu_start<1000u);}
    assert(bus.menus==menus+1);
    assert(!memcmp(bus.menu_frames[menus],(uint8_t[]){0xbd,4,0x91,9},4));
    queries=bus.queries;settings=bus.settings;meters=bus.meters;links=bus.links;
    while(menu_phase()==REMOTE_MENU_GAP || menu_phase()==REMOTE_MENU_WAIT) {
        target[0]=(uint8_t)(20u+(now%30u));tick(now++);
        assert(bus.ready && now-menu_start<1000u);
        assert(transactions()==before && bus.queries==queries && bus.settings==settings &&
               bus.meters==meters && bus.links==links);
    }
    assert(menu_phase()==REMOTE_MENU_ACCEPTED && bus.menus==menus+2);
    assert(!memcmp(bus.menu_frames[menus+1],(uint8_t[]){0xbd,4,0x92,1},4));
    assert(bus.menu_times[menus+1]-bus.menu_times[menus]>=20u);
    for(uint32_t n=now;n<now+750u;++n) tick(n);
    now+=750u;assert(bus.ready && bus.current[0]==target[0]);

    /* A main-loop reservation must finish a partially submitted menu frame
     * and physical TXIDLE before stopping UART; the second frame is cancelled. */
    assert(omni_native_menu_request(0x102u,true,2,now));menus=bus.menus;menu_start=now;
    while(menu_phase()!=REMOTE_MENU_SEND || bus.used!=2u) {
        prepare(now++,2);omni_native_gain_service(bus.now,true);assert(now-menu_start<1000u);
    }
    closes=bus.closes;released=false;
    test_uart3.STAT=0;
    for(unsigned i=0;i<5u;++i) {
        prepare(now,1);assert(!omni_native_gain_release(now++));
        assert(bus.owned && bus.closes==closes);
    }
    assert(bus.used==0u && bus.menus==menus+1u);
    test_uart3.STAT=USART_STAT_TXIDLE_MASK;
    while(!released) {
        prepare(now,1);released=omni_native_gain_release(now++);
        if(!released) assert(bus.owned && bus.closes==closes);
        assert(now-menu_start<1000u);
    }
    assert(!bus.used && !bus.owned && bus.closes==closes+1 && bus.menus==menus+1);
    assert(menu_phase()==REMOTE_MENU_CANCELLED && omni_native_menu_read(0,w) && !(w[3]&4u));
    /* Explicitly closing while offline cancels the saved desired-open intent;
     * startup does not replay the cancelled92 sequence. */
    assert(omni_native_menu_request(0x103u,false,0,now));omni_native_gain_service(now++,false);
    menus=bus.menus;
    for(uint32_t n=now;n<now+750u;++n) tick(n);
    now+=750u;assert(bus.ready && bus.menus==menus);

    /* Continuous dirty gain can expire a queued menu intent without ever
     * sending its first byte; expiration is independent of menu priority. */
    assert(omni_native_menu_request(0x104u,true,2,now));menu_start=now;
    while(menu_phase()==REMOTE_MENU_QUEUED) {
        target[0]=(uint8_t)(20u+(now%30u));tick(now++);assert(now-menu_start<=2001u);
    }
    assert(menu_phase()==REMOTE_MENU_TIMEOUT && bus.menus==menus);

    /* A remote bulk20 snapshot can replace DSP mode2 with Speakers mode1.
     * This is a complete GET43 response, not UART loss: match the recorded
     * failure exactly, then repair once and verify mode plus fresh gain lanes. */
    for(uint32_t n=now;n<now+750u;++n) tick(n);
    now+=750u;assert(bus.ready);
    unsigned mode_sets=bus.mode_sets;closes=bus.closes;bus.mode=1u;
    target[0]=(uint8_t)(target[0]+1u);uint32_t mode_start=now;
    do {
        tick(now++);assert(now-mode_start<1500u);
        assert(omni_native_gain_read(0,w));
    } while(!(w[2]&4u));
    assert(omni_native_gain_read(1,w));
    assert(w[2]==7u && w[3]==5u && w[4]==0u && w[8]==4u && w[9]==5u && !w[11]);
    assert(bus.available-bus.read==4u); /* Previous43 ACK still needs draining. */
    assert(omni_native_gain_read(2,w) && !w[3] && w[11]==3u && w[12]==1u && !w[13] && w[14]==1u);
    /* Schedule that ACK later and hold physical TXIDLE low: neither a stale
     * ACK nor the completed five-byte DB reply can admit the repair SET. */
    bus.available=bus.read;test_uart3.STAT=0;
    for(unsigned i=0;i<10u;++i) {
        tick(now++);assert(bus.mode_sets==mode_sets && !bus.ready);
        assert(omni_native_gain_read(2,w) && w[7]);
    }
    push((uint8_t[]){0xdd,3,0x43,0},4);test_uart3.STAT=USART_STAT_TXIDLE_MASK;
    target[0]=42u;assert(omni_native_menu_request(0x105u,false,1u,now));
    while(menu_phase()!=REMOTE_MENU_ACCEPTED) {
        tick(now++);assert(now-mode_start<1500u && bus.closes==closes);
    }
    assert(bus.ready && bus.mode==2u && bus.current[0]==42u && bus.mode_sets==mode_sets+1u);
    assert(omni_native_gain_read(2,w) && w[3] && w[11]==1u && w[12]==1u && w[13]==1u && w[14]==1u);

    /* A second mode drift on the same physical acquisition must latch, not
     * create a persistent SET43/save/retry loop against a remote writer. */
    mode_sets=bus.mode_sets;bus.mode=1u;target[0]=43u;mode_start=now;
    do {tick(now++);assert(now-mode_start<1500u);assert(omni_native_gain_read(0,w));} while(!(w[2]&4u));
    for(uint32_t n=now;n<now+600u;++n) tick(n);
    now+=600u;
    assert(bus.mode_sets==mode_sets && bus.mode==1u && !bus.ready && bus.closes==closes);
    assert(omni_native_gain_read(2,w) && !w[3] && w[5] && w[11]==5u && w[12]==1u && w[13]==1u);

    /* A real release/acquire reestablishes the normal startup contract. An
     * unsupported mode0 response afterward must not enter the mode1 repair. */
    omni_native_gain_service(now++,false);
    for(uint32_t n=now;n<now+1300u;++n) tick(n);
    now+=1300u;assert(bus.ready);mode_sets=bus.mode_sets;bus.mode=0u;target[0]=44u;mode_start=now;
    do {tick(now++);assert(now-mode_start<1500u);assert(omni_native_gain_read(0,w));} while(!(w[2]&4u));
    for(uint32_t n=now;n<now+600u;++n) tick(n);
    now+=600u;assert(bus.mode_sets==mode_sets && !bus.ready);
    assert(omni_native_gain_read(2,w) && !w[11] && w[12]==1u && w[13]==1u && w[14]==0u);

    /* A NACK to the one allowed repair stays failed without another SET. */
    omni_native_gain_service(now++,false);
    for(uint32_t n=now;n<now+1300u;++n) tick(n);
    now+=1300u;assert(bus.ready);mode_sets=bus.mode_sets;
    bus.mode=1u;bus.nack_mode_set=true;target[0]=45u;mode_start=now;
    do {
        tick(now++);assert(now-mode_start<1500u);
        assert(omni_native_gain_read(2,w));
    } while(!w[5]);
    for(uint32_t n=now;n<now+600u;++n) tick(n);
    now+=600u;
    assert(bus.mode_sets==mode_sets+1u && bus.mode==1u && !bus.ready);
    assert(omni_native_gain_read(2,w) && !w[3] && w[11]==5u && w[12]==2u && w[13]==1u);

    /* Logical GET failure can precede physical TXIDLE. A cooperative release
     * must preserve those outstanding bytes even though no parser owns them,
     * without starting the pending mode repair while the caller is yielding. */
    bus.nack_mode_set=false;omni_native_gain_service(now++,false);
    for(uint32_t n=now;n<now+1300u;++n) tick(n);
    now+=1300u;assert(bus.ready);mode_sets=bus.mode_sets;closes=bus.closes;
    bus.mode=1u;target[0]=46u;mode_start=now;
    do {tick(now++);assert(now-mode_start<1500u);assert(omni_native_gain_read(0,w));} while(!(w[2]&4u));
    assert(omni_native_gain_read(2,w) && w[7]);
    bus.available=bus.read;test_uart3.STAT=0;
    for(unsigned i=0;i<10u;++i) {
        prepare(now,0);assert(!omni_native_gain_release(now++));
        assert(bus.owned && bus.closes==closes && bus.mode_sets==mode_sets && !bus.ready);
    }
    push((uint8_t[]){0xdd,3,0x43,0},4);test_uart3.STAT=USART_STAT_TXIDLE_MASK;
    prepare(now,0);assert(omni_native_gain_release(now++));
    assert(!bus.owned && bus.closes==closes+1u && bus.mode_sets==mode_sets);
    for(uint32_t n=now;n<now+1300u;++n) tick(n);
    now+=1300u;assert(bus.ready && bus.mode==2u && bus.current[0]==46u);

    /* The D2 SET/readback owns its GAP even when a newer Windows revision
     * dirties lineout gain. Latest desired state follows after that GET. */
    bus.master_db=-20*256;++bus.master_revision;target[0]=47u;uint32_t began_headset=now;
    do {tick(now++);assert(now-began_headset<2000u);assert(omni_native_headset_gain_read(0,w));}
        while(w[2]!=HEADSET_GAIN_GAP);
    unsigned headset_gets=bus.headset_gets;before=transactions();
    bus.master_db=-10*256;++bus.master_revision;target[0]=48u;
    while(bus.headset_gets==headset_gets) {
        tick(now++);assert(transactions()==before && now-began_headset<2000u && bus.ready);
    }
    for(uint32_t n=now;n<now+1500u;++n)tick(n);
    now+=1500u;
    assert(bus.headset_level==46u && bus.current[0]==48u && bus.ready);
    assert(omni_native_headset_gain_read(0,w) && w[7]==46u && w[10]==bus.master_revision);

    /* One physical tag0 absolute report updates the common USB state once;
     * source tags1/2 and returned own SET/readback do not apply extra steps. */
    unsigned peer_writes=bus.peer_writes;bus.headset_level=45u;
    push((uint8_t[]){0xdb,6,0xd2,3,0,45,0xdb,6,0xd2,3,1,45,0xdb,6,0xd2,3,2,45},18);
    for(uint32_t n=now;n<now+1000u;++n)tick(n);
    now+=1000u;assert(bus.master_db==-11*256 && !bus.master_mute && bus.peer_writes==peer_writes+1u && bus.ready);
    /* A Windows change during a fragmented physical frame wins the CAS. */
    peer_writes=bus.peer_writes;push((uint8_t[]){0xdb},1);tick(now++);
    bus.master_db=-12*256;++bus.master_revision;push((uint8_t[]){6,0xd2,3,0,43},5);
    for(uint32_t n=now;n<now+1000u;++n)tick(n);
    now+=1000u;assert(bus.master_db==-12*256 && bus.peer_writes==peer_writes && bus.headset_level==44u);

    /* A physical turn during our SET/GET gap updates the same serialized
     * Windows state. A valid competing LL cannot latch INVALID_REPLY. */
    bus.master_db=-15*256;++bus.master_revision;began_headset=now;
    do {tick(now++);assert(now-began_headset<2000u);assert(omni_native_headset_gain_read(0,w));}
        while(w[2]!=HEADSET_GAIN_GAP);
    peer_writes=bus.peer_writes;bus.headset_level=40u;
    push((uint8_t[]){0xdb,6,0xd2,3,0,40},6);
    for(uint32_t n=now;n<now+1000u;++n) {tick(n);assert(bus.ready);}
    now+=1000u;
    assert(bus.master_db==-16*256 && bus.peer_writes==peer_writes+1u && bus.headset_level==40u);
    assert(omni_native_headset_gain_read(0,w) && w[7]==40u && w[10]==bus.master_revision);

    /* A physical change observed only by the owned GET has the same wire
     * framing. With no newer Windows revision its CAS may adopt that value. */
    bus.master_db=-17*256;++bus.master_revision;peer_writes=bus.peer_writes;
    bus.override_headset_get=true;bus.headset_get_value=38u;
    for(uint32_t n=now;n<now+1000u;++n) {tick(n);assert(bus.ready);}
    now+=1000u;
    assert(bus.master_db==-18*256 && bus.peer_writes==peer_writes+1u && bus.headset_level==38u);
    assert(omni_native_headset_gain_read(0,w) && w[7]==38u && (w[14]>>8)>=1u);

    /* An old owned readback that STARTS after a newer Windows change is not
     * allowed to adopt the old value using that newer first-byte revision. */
    bus.master_db=-17*256;++bus.master_revision;peer_writes=bus.peer_writes;
    bus.override_headset_get=true;bus.headset_get_value=37u;headset_gets=bus.headset_gets;
    began_headset=now;
    while(bus.headset_gets==headset_gets) {tick(now++);assert(now-began_headset<2000u);}
    bus.master_db=-20*256;++bus.master_revision;
    for(uint32_t n=now;n<now+1000u;++n) {tick(n);assert(bus.ready);}
    now+=1000u;
    assert(bus.master_db==-20*256 && bus.peer_writes==peer_writes && bus.headset_level==36u);

    /* Fragmented owned conflict captures the older revision at its first
     * byte; the subsequent Windows change rejects the peer update by CAS. */
    bus.master_db=-21*256;++bus.master_revision;peer_writes=bus.peer_writes;
    bus.override_headset_get=true;bus.headset_get_value=33u;headset_gets=bus.headset_gets;
    began_headset=now;
    while(bus.headset_gets==headset_gets) {tick(now++);assert(now-began_headset<2000u);}
    assert(bus.available-bus.read==6u);uint8_t conflict_tail[5];
    memcpy(conflict_tail,bus.rx+bus.available-5u,5u);bus.available-=5u;tick(now++);
    bus.master_db=-22*256;++bus.master_revision;push(conflict_tail,5u);
    for(uint32_t n=now;n<now+1000u;++n) {tick(n);assert(bus.ready);}
    now+=1000u;
    assert(bus.master_db==-22*256 && bus.peer_writes==peer_writes && bus.headset_level==34u);
    assert(omni_native_headset_gain_read(1,w) && !w[6]); /* No malformed reports. */
    bus.master_db=-12*256;++bus.master_revision;
    for(uint32_t n=now;n<now+1000u;++n)tick(n);
    now+=1000u;assert(bus.ready && bus.headset_level==44u);

    /* Known absence and bounded unknown E4 both preserve analog startup;
     * neither sends an unverified wireless SET to a nonexistent peer. */
    omni_native_gain_service(now++,false);bus.peer_state=1u;
    unsigned headset_sets=bus.headset_sets,primes=bus.headset_primes;
    for(uint32_t n=now;n<now+1500u;++n)tick(n);
    now+=1500u;assert(bus.ready && bus.headset_sets==headset_sets && bus.headset_primes==primes);
    omni_native_gain_service(now++,false);bus.drop_link=true;
    for(uint32_t n=now;n<now+1500u;++n)tick(n);
    now+=1500u;assert(bus.ready && bus.headset_sets==headset_sets && bus.headset_primes==primes);
    bus.drop_link=false;bus.peer_state=3u;push((uint8_t[]){0xdb,5,0xe4,3,3},5);
    for(uint32_t n=now;n<now+1500u;++n)tick(n);
    now+=1500u;assert(bus.ready && bus.headset_primes==primes+1u && bus.headset_level==44u);

    /* A no-response gain fault remains latched on duplicate connected reports.
     * Actual absent->connected, fresh bulk20 and mode2 can establish a new
     * peer epoch; only its subsequent complete47 read makes audio ready. */
    bus.drop_gain_reply=true;target[0]=49u;uint32_t failed_at=now;
    do {tick(now++);assert(now-failed_at<1500u);assert(omni_native_gain_read(0,w));}while(!(w[2]&4u));
    mode_sets=bus.mode_sets;headset_sets=bus.headset_sets;
    bus.drop_gain_reply=false;push((uint8_t[]){0xdb,5,0xe4,3,3},5);
    for(uint32_t n=now;n<now+100u;++n)tick(n);
    now+=100u;assert(!bus.ready && bus.mode_sets==mode_sets && bus.headset_sets==headset_sets);
    bus.peer_state=1u;push((uint8_t[]){0xdb,5,0xe4,3,1},5);tick(now++);
    bus.peer_state=3u;push((uint8_t[]){0xdb,5,0xe4,3,3},5);
    for(uint32_t n=now;n<now+1500u;++n)tick(n);
    now+=1500u;
    assert(bus.ready && bus.current[0]==49u && bus.mode_sets==mode_sets+1u && bus.headset_sets==headset_sets+1u);
    assert(omni_native_headset_gain_read(1,w) && w[14]==1u);

    /* Sustained physical/Windows changes keep the latest analog47 tuple
     * dirty for far longer than the D2 admission deadline. Each gain owner
     * must still progress, without withdrawing previously verified audio. */
    uint32_t rapid_start=now,last_headset=now,last_analog=now;
    unsigned rapid_headset=bus.headset_sets;uint32_t rapid_analog=transactions();
    assert(omni_native_headset_gain_read(1,w));uint32_t headset_timeouts=w[5];
    for(unsigned elapsed=0;elapsed<6000u;++elapsed) {
        if(elapsed%80u==0u) {
            bus.master_db=(int16_t)(-20*256+(int)((elapsed/80u)%11u)*256);
            ++bus.master_revision;target[0]=(uint8_t)(30u+(elapsed/80u)%21u);
        }
        if(elapsed%137u==0u) {
            bus.headset_level=(uint8_t)(30u+(elapsed/137u)%15u);
            push((uint8_t[]){0xdb,6,0xd2,3,0,bus.headset_level},6);
        }
        tick(now++);assert(bus.ready);
        if(bus.headset_sets!=rapid_headset) {rapid_headset=bus.headset_sets;last_headset=now;}
        if(transactions()!=rapid_analog) {rapid_analog=transactions();last_analog=now;}
        assert(now-last_headset<1500u && now-last_analog<1500u);
    }
    assert(now-rapid_start==6000u);
    for(uint32_t n=now;n<now+1500u;++n) {tick(n);assert(bus.ready);}
    now+=1500u;
    assert(omni_native_headset_gain_read(0,w) && w[2]==HEADSET_GAIN_DONE && w[10]==bus.master_revision);
    assert(w[7]==(uint32_t)(56+bus.master_db/256));
    assert(omni_native_headset_gain_read(1,w) && w[5]==headset_timeouts);
    puts("Native shared UART: battery/settings/menu/meter arbitration, dirty-gain menu GAP, bounded timeouts, partial-TX handoff and fault recovery passed");
    return 0;
}
