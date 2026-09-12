#include "native_gain_adapter.h"
#include "native_gain.h"
#include "mixer_ui.h"
#include "control_uart.h"
#include "audio_probe.h"
#include "usb_audio_ring.h"
#include "headset_battery.h"
#include "headset_query.h"
#include "headset_gain.h"
#include "headset_volume.h"
#include "dsp_meter.h"
#include "dsp_settings.h"
#include "remote_menu.h"
#include "fsl_device_registers.h"
#include <string.h>
static omni_native_gain state;
static omni_remote_menu menu;
static omni_headset_gain headset;
static mcu2_link_io bytes;
static bool uart_opened;
static bool mode_ready,mode_requested,mode_failed;
static bool mode_repair_used,mode_repair_pending,mode_repair_blocked;
static bool peer_recovery_pending,headset_mode_reset;
static uint32_t mode_repairs,mode_repair_successes,last_failed_mode;
static uint32_t mode_token=0x80000000u;
static omni_native_rx_observer observer;
void omni_native_gain_observer(omni_native_rx_observer fn) { observer=fn; }
static bool tx_pending,have_tx;
static uint32_t observed_ms,last_drained_ms,observed_revision,opened_ms;
static uint32_t headset_turn_gain;
static bool headset_turn_used;
static int tx(void *context,uint8_t b)
{
    (void)context;int r=bytes.tx(bytes.context,b);
    if(r==1) { tx_pending=true;have_tx=true; }
    return r;
}
static int rx(void *context,uint8_t *b)
{
    (void)context;int r=bytes.rx(bytes.context,b);
    if(r==1) {
        omni_dsp_settings_observe(*b,observed_ms);
        bool peer_was_absent=headset.have_link && !headset.connected,was_primed=headset.primed;
        omni_headset_gain_byte(&headset,*b,observed_ms,observed_revision);
        if(peer_was_absent && headset.have_link && headset.connected) peer_recovery_pending=true;
        if(!was_primed && headset.primed) headset_mode_reset=true;
        omni_remote_menu_byte(&menu,*b,observed_ms);
        if(observer) observer(*b,observed_ms);
        omni_headset_battery_observe(*b,observed_ms);
        omni_headset_query_observe(*b,observed_ms);
        omni_dsp_meter_observe(*b,observed_ms);
    } else if(r<0) {
        omni_headset_battery_io_error();omni_headset_query_io_error(observed_ms);
        omni_dsp_meter_io_error(observed_ms);omni_dsp_settings_io_error(observed_ms);
        omni_remote_menu_io_error(&menu,observed_ms);
        omni_headset_gain_io_error(&headset,observed_ms);
    }
    return r;
}
static int drained(void *context)
{
    (void)context; uint32_t stats[6]; omni_control_uart_stats(3u,stats);
    if(stats[5]) return -1;
    bool complete=(USART3->FIFOSTAT&USART_FIFOSTAT_TXEMPTY_MASK) &&
        (USART3->STAT&USART_STAT_TXIDLE_MASK);
    if(complete && tx_pending) { last_drained_ms=observed_ms;tx_pending=false; }
    return complete?1:0;
}
static bool open(void *context,omni_dsp_volume_io *io)
{
    (void)context;
    if(!uart_opened) {
        if(!omni_control_uart_start(3u,&bytes)) return false;
        uart_opened=true;tx_pending=have_tx=false;omni_headset_battery_acquire();
        omni_headset_query_acquire();
        omni_dsp_meter_acquire(observed_ms);omni_dsp_settings_acquire();
        omni_remote_menu_acquire(&menu);
        omni_headset_gain_acquire(&headset,observed_ms);opened_ms=observed_ms;
        mode_ready=mode_requested=mode_failed=false;
        mode_repair_used=mode_repair_pending=mode_repair_blocked=false;
        peer_recovery_pending=headset_mode_reset=false;
        headset_turn_used=false;
    }
    *io=(omni_dsp_volume_io){0,tx,rx,drained}; return true;
}
/* Logical gain completion leaves the shared physical listener running. */
static void close(void *context) { (void)context; }
static void close_uart(void)
{
    if(uart_opened) { omni_control_uart_stop(3u);uart_opened=false; }
    omni_headset_battery_release();
    omni_headset_query_release(observed_ms);
    omni_dsp_meter_release(observed_ms);omni_dsp_settings_release(observed_ms);
    omni_remote_menu_release(&menu,observed_ms);
    omni_headset_gain_release(&headset,observed_ms);
    mode_ready=mode_requested=mode_failed=false;
    mode_repair_pending=false;
    peer_recovery_pending=headset_mode_reset=false;
}
static void initialize(void)
{
    if(!state.initialized) {
        (void)omni_native_gain_init(&state,(omni_native_transport){0,open,close});
        omni_headset_battery_init();
        (void)omni_remote_menu_init(&menu);
        (void)omni_headset_gain_init(&headset);
    }
}
static bool gain_settled(uint32_t now,const uint8_t levels[4])
{
    if(!state.ready || state.fault || !state.have_verified || state.owned ||
       state.input_mask!=state.verified_mask ||
       (uint32_t)(now-state.last_verified_ms)>=1000u) return false;
    for(unsigned i=0;i<4u;++i)
        if((state.input_mask&(1u<<i)) && levels[i]!=state.verified_levels[i]) return false;
    return true;
}
static void transport_fault(void)
{
    if(!uart_opened || (!omni_headset_battery_transport_fault() &&
                       !omni_headset_query_transport_fault() && !omni_dsp_meter_transport_fault() && !omni_dsp_settings_transport_fault() && !menu.fault && !headset.fault)) return;
    if(state.owned) {
        omni_gain_trial_cancel(&state.transfer);state.owned=false;
        ++state.cancellations;
    }
    if(!state.fault) ++state.failures;
    state.fault=true;state.ready=false;
    close_uart();
}
static void observe_mode_failure(void)
{
    const omni_gain_trial *t=&state.transfer;
    if(!uart_opened || !mode_ready || !state.fault || state.owned ||
       t->phase!=GAIN_FAILED || t->error!=GAIN_MODE) return;
    last_failed_mode=t->mode;
    /* Only a complete initial GET43 reporting the observed Speakers mode1
     * permits this repair. Never replay after a SET, malformed frame, timeout,
     * NACK, transport fault or an unrecognized mode. One attempt per physical
     * UART acquisition bounds a stale remote bulk20 shadow repeatedly racing
     * this application's explicit mode2 owner. */
    if(t->mode!=1u || t->step!=0u || t->tx_bytes!=4u || t->rx_length[0]!=5u ||
       t->set_may_have_applied || memcmp(t->rx_frame[0],(uint8_t[]){0xdb,5,0x43,3,1},5u)) return;
    mode_ready=false;mode_requested=false;
    if(mode_repair_used) {mode_failed=true;mode_repair_blocked=true;return;}
    mode_repair_used=true;mode_repair_pending=true;mode_failed=false;++mode_repairs;
}
static void service(uint32_t now,bool online)
{
    initialize();observed_ms=now;
    int16_t db; uint8_t muted,ll;audio_probe_master_snapshot(&db,&muted,&observed_revision);
    int16_t hdb;uint8_t hmute;omni_mixer_ui_master(db,muted!=0u,&hdb,&hmute);
    if(omni_headset_volume_step(hdb,hmute!=0u,&ll))
        (void)omni_headset_gain_desire(&headset,ll,observed_revision,now);
    uint8_t levels[4];omni_mixer_ui_targets(db,muted!=0u,levels);
    (void)omni_native_gain_inputs(&state,5u,levels);
    if(!online) {
        omni_native_gain_poll(&state,now,false,db,muted!=0u);
        close_uart();usb_audio_ring_gain_ready(false);return;
    }
    if(!uart_opened) {
        /* A physical I/O failure is not an implicit release/acquire request.
         * Keep it latched until the caller supplies an offline boundary. */
        if(state.fault) {usb_audio_ring_gain_ready(false);return;}
        omni_dsp_volume_io unused;if(!open(0,&unused)) {usb_audio_ring_gain_ready(false);return;}
    }
    omni_headset_battery_expire(now);omni_dsp_settings_expire(now);
    bool idle_exhausted=!uart_opened;
    if(uart_opened && !state.owned) {
        /* A single consumer observes all idle bytes; transaction RX uses the
         * same callback. No concurrent reader can steal a gain reply. */
        for(unsigned budget=0;budget<32u;++budget) {
            uint8_t b;int r=rx(0,&b);
            if(r!=1) { idle_exhausted=r==0;break; }
        }
    }
    /* A complete immediate GET reply can terminate gain inside SEND before
     * its next DRAIN poll. The bytes still need actual TXIDLE observation;
     * do not leave tx_pending permanently blocking the mode-repair boundary.
     * Pending intents do not own UART, but every active client does. */
    if(tx_pending && !state.owned && !omni_headset_battery_busy() && !omni_headset_query_active() &&
       !omni_dsp_settings_active() && !omni_remote_menu_active(&menu) && !omni_dsp_meter_busy() && !omni_headset_gain_active(&headset)) {
        if(drained(0)<0) {
            omni_dsp_settings_io_error(now);transport_fault();
            usb_audio_ring_gain_ready(false);return;
        }
    }
    uint32_t peer_revision,peer_ms;uint8_t peer_ll;
    if(omni_headset_gain_take_remote(&headset,&peer_ll,&peer_revision,&peer_ms)) {
        int16_t peer_db;uint8_t peer_mute;(void)peer_ms;
        if(omni_headset_volume_db(peer_ll,db,&peer_db,&peer_mute) &&
           audio_probe_peer_volume(peer_db,peer_mute,peer_revision)==0) {
            audio_probe_master_snapshot(&db,&muted,&observed_revision);
            omni_mixer_ui_targets(db,muted!=0u,levels);
            (void)omni_native_gain_inputs(&state,5u,levels);
            omni_mixer_ui_master(db,muted!=0u,&hdb,&hmute);
            if(omni_headset_volume_step(hdb,hmute!=0u,&ll))
                (void)omni_headset_gain_desire(&headset,ll,observed_revision,now);
        }
    }
    bool spaced=!have_tx || (!tx_pending && (uint32_t)(now-last_drained_ms)>=20u);
    bool boundary=spaced && idle_exhausted && !omni_headset_gain_frame_pending(&headset) &&
        !omni_headset_battery_frame_pending() && !omni_dsp_settings_frame_pending() && !omni_remote_menu_frame_pending(&menu);
    /* One bounded initial E4 observation distinguishes absent wireless from
     * connected startup. Analog operation may proceed after 300ms without a
     * peer reply; a later actual E4 connect initiates fresh bulk priming. */
    if(!mode_ready && !mode_requested && !state.owned && !omni_headset_gain_active(&headset) &&
       (omni_headset_battery_busy() || (!headset.have_link && (uint32_t)(now-opened_ms)<300u))) {
        omni_headset_battery_poll(now,boundary && !state.yielding,
                                 (omni_headset_battery_io){0,tx,drained});
        transport_fault();
        if(!uart_opened || omni_headset_battery_busy() || !headset.have_link) {
            usb_audio_ring_gain_ready(false);return;
        }
    }
    bool no_other_owner=!state.owned && !omni_headset_battery_busy() && !omni_headset_query_active() &&
        !omni_dsp_settings_active() && !omni_remote_menu_active(&menu) && !omni_dsp_meter_busy();
    if(no_other_owner && headset_mode_reset) {
        /* A complete first bulk may also have arrived via an existing manual
         * query. Its mode side effect still precedes our verified43/47 contract. */
        mode_ready=mode_requested=mode_failed=false;state.ready=false;state.have_verified=false;
        headset_mode_reset=false;
    }
    bool prime_active=omni_headset_gain_active(&headset) && (headset.operation==0u || headset.cancel);
    if(no_other_owner && (prime_active || (headset.have_link && headset.connected && !headset.primed && !headset.blocked))) {
        /* bulk20 can reconcile output-mode shadow. Establish43/47 AFTER it,
         * preserving the shared desired volume and gating PCM meanwhile. */
        mode_ready=mode_requested=mode_failed=false;state.ready=false;state.have_verified=false;
        omni_headset_gain_poll(&headset,now,boundary && !state.yielding,false,(omni_headset_gain_io){0,tx,drained});
        transport_fault();usb_audio_ring_gain_ready(false);return;
    }
    /* Fresh stock defaults to Speakers mode1. Establish and read back mode2
     * before gain/DMA can become ready; retained old-unit settings are not a
     * startup contract. This is the same serialized DSP owner as gain. */
    if(!mode_ready) {
        /* A menu intent can expire during bootstrap, but cannot claim UART
         * before the output-mode SET/readback has completed. */
        omni_remote_menu_poll(&menu,now,false,(omni_remote_menu_io){0,tx,drained});
        if(!mode_requested && !mode_failed && !state.yielding && !omni_dsp_settings_busy()) {
            const uint8_t value=2u;
            mode_token=omni_dsp_settings_next_token();
            mode_requested=omni_dsp_settings_request(mode_token,DSP_SETTING_OUTPUT_MODE,&value,1u,now);
        }
        omni_dsp_settings_poll(now,spaced && idle_exhausted && !omni_dsp_settings_frame_pending(),
                               (omni_dsp_settings_io){0,tx,drained});
        uint32_t result[15];
        if(mode_requested && omni_dsp_settings_status(0,result) && result[2]==mode_token && !omni_dsp_settings_busy()) {
            mode_ready=result[4]==DSP_SETTINGS_ACCEPTED;mode_failed=!mode_ready;
            if(mode_ready && peer_recovery_pending && headset.primed) {
                if(state.fault) ++headset.gain_recoveries;
                state.fault=false;state.ready=false;state.have_verified=false;
                peer_recovery_pending=false;
            }
            if(mode_repair_pending) {
                mode_repair_pending=false;
                if(mode_ready) {
                    ++mode_repair_successes;
                    /* A new gain transaction must read every lane again and
                     * verify the current desired tuple before PCM is released. */
                    state.fault=false;state.ready=false;state.have_verified=false;
                } else mode_repair_blocked=true;
            }
        }
        usb_audio_ring_gain_ready(false);return;
    }
    bool can_query=uart_opened && spaced && idle_exhausted && !state.yielding && gain_settled(now,levels) &&
        !omni_headset_battery_frame_pending() && !omni_dsp_settings_frame_pending() && !omni_remote_menu_frame_pending(&menu);
    /* Runtime D2 is a gain owner, not a background query. Requiring the latest
     * 47 tuple to stay settled starves it during continuous dial/Windows input.
     * After initial47 verification, alternate pending D2 with necessary47 work
     * at complete physical boundaries. Unchanged47 needs no extra transaction. */
    bool headset_turn=uart_opened && boundary && !state.yielding &&
        state.ready && state.have_verified && !state.fault && !state.owned &&
        (!headset_turn_used || headset_turn_gain!=state.verified_transactions || gain_settled(now,levels));
    /* Periodic battery work wins an idle slot before an explicit queued read,
     * so repeated manual reads cannot starve it. An active owner completes. */
    omni_headset_battery_poll(now,can_query && !omni_headset_query_active() && !omni_dsp_meter_busy() && !omni_dsp_settings_active() && !omni_remote_menu_active(&menu) && !omni_headset_gain_active(&headset),
                             (omni_headset_battery_io){0,tx,drained});
    transport_fault();
    bool headset_queued=headset.phase==HEADSET_GAIN_QUEUED || headset.phase==HEADSET_GAIN_DEFERRED;
    omni_headset_gain_poll(&headset,now,(headset_turn || (omni_headset_gain_active(&headset) && boundary)) &&
                          ((headset.have_link && headset.connected) || omni_headset_gain_active(&headset)) &&
                          (headset.primed || omni_headset_gain_active(&headset)) &&
                          uart_opened && !state.owned && !omni_headset_battery_busy() && !omni_headset_query_active() &&
                          !omni_dsp_meter_busy() && !omni_dsp_settings_active() && !omni_remote_menu_active(&menu),true,
                          (omni_headset_gain_io){0,tx,drained});
    if(headset_queued && omni_headset_gain_active(&headset)) {
        headset_turn_used=true;headset_turn_gain=state.verified_transactions;
    }
    transport_fault();
    omni_headset_query_poll(now,can_query && uart_opened && !omni_headset_battery_busy() && !omni_dsp_meter_busy() && !omni_dsp_settings_active() && !omni_remote_menu_active(&menu) && !omni_headset_gain_active(&headset),
                           (omni_headset_query_io){0,tx,drained});
    transport_fault();
    omni_dsp_settings_poll(now,(can_query || (omni_dsp_settings_active() && spaced && idle_exhausted && !omni_dsp_settings_frame_pending())) && uart_opened && !state.owned && !omni_headset_battery_busy() &&
                           !omni_headset_query_active() && !omni_dsp_meter_busy() && !omni_remote_menu_active(&menu) && !omni_headset_gain_active(&headset),
                           (omni_dsp_settings_io){0,tx,drained});
    transport_fault();
    /* A91 ACK followed by92 remains one owner. A volume change during GAP
     * must not require gain to settle before that owner can finish. */
    omni_remote_menu_poll(&menu,now,(can_query || (omni_remote_menu_active(&menu) && spaced && idle_exhausted &&
                          !omni_headset_battery_frame_pending() && !omni_dsp_settings_frame_pending() && !omni_remote_menu_frame_pending(&menu))) &&
                          uart_opened && !state.owned && !omni_headset_battery_busy() && !omni_headset_query_active() &&
                          !omni_dsp_meter_busy() && !omni_dsp_settings_active() && !omni_headset_gain_active(&headset) &&
                          (!headset.connected || omni_headset_gain_ready(&headset) || omni_remote_menu_active(&menu)),(omni_remote_menu_io){0,tx,drained});
    transport_fault();
    omni_dsp_meter_poll(now,can_query && uart_opened && !omni_headset_battery_busy() &&
                       !omni_headset_query_busy() && !omni_dsp_settings_busy() && !omni_remote_menu_busy(&menu) && !omni_headset_gain_busy(&headset),(omni_dsp_meter_io){0,tx,drained});
    transport_fault();
    /* A new gain parser must start at a frame boundary. Preserve an explicit
     * caller's yield while also deferring behind a bounded battery request. */
    bool yielding=state.yielding;
    state.yielding=yielding || omni_headset_battery_busy() || omni_headset_query_active() ||
        omni_dsp_meter_busy() || omni_dsp_settings_active() || omni_remote_menu_active(&menu) || state.owned ||
        !idle_exhausted || (uart_opened && !spaced) || omni_headset_battery_frame_pending() || omni_remote_menu_frame_pending(&menu) ||
        omni_headset_gain_active(&headset) || omni_headset_gain_frame_pending(&headset);
    omni_native_gain_poll(&state,now,online,db,muted!=0u);
    state.yielding=yielding;
    transport_fault();
    observe_mode_failure();
    usb_audio_ring_gain_ready(state.ready && online && (!headset.have_link || !headset.connected || omni_headset_gain_ready(&headset)));
}
void omni_native_gain_service(uint32_t now,bool online)
{
    initialize();
    /* SET_INTERFACE can withdraw startup while a cooperative reservation is
     * still draining. Finish that reservation before resuming normal traffic;
     * otherwise gain and meter polling would stay yielded indefinitely. */
    if(online && state.yielding) { (void)omni_native_gain_release(now);return; }
    service(now,online);
    if(!online) state.yielding=false;
}
bool omni_native_gain_release(uint32_t now)
{
    initialize();
    if(!uart_opened && !state.owned) {
        omni_headset_query_yield(now);omni_dsp_settings_yield(now);omni_dsp_meter_yield(now);
        omni_remote_menu_yield(&menu,now);
        omni_headset_gain_yield(&headset,now);
        state.yielding=false;return true;
    }
    state.yielding=true;
    omni_headset_query_yield(now);
    omni_dsp_meter_yield(now);omni_dsp_settings_yield(now);
    omni_remote_menu_yield(&menu,now);
    omni_headset_gain_yield(&headset,now);
    /* Complete any partial SET/readback before handing the UART to startup. */
    service(now,true);
    if(tx_pending || state.owned || omni_headset_battery_busy() || omni_headset_query_active() || omni_dsp_meter_busy() || omni_dsp_settings_active() || omni_remote_menu_active(&menu) || omni_headset_gain_active(&headset)) return false;
    service(now,false);state.yielding=false;return true;
}
bool omni_native_gain_read(unsigned page,uint32_t out[15])
{
    if(!out || page>2u) return false;
    if(page==2u) {
        for(unsigned i=0;i<15u;++i) out[i]=0;
        out[0]=1u;out[1]=2u;out[2]=uart_opened;out[3]=mode_ready;
        out[4]=mode_requested;out[5]=mode_failed;out[6]=mode_token;
        out[7]=tx_pending;out[8]=have_tx;out[9]=last_drained_ms;
        out[10]=state.yielding;
        out[11]=(uint32_t)mode_repair_used|((uint32_t)mode_repair_pending<<1)|((uint32_t)mode_repair_blocked<<2);
        out[12]=mode_repairs;out[13]=mode_repair_successes;out[14]=last_failed_mode;return true;
    }
    omni_native_gain_status(&state,page,out); return true;
}
bool omni_native_menu_request(uint32_t token,bool open,unsigned context,uint32_t now)
{
    initialize();return omni_remote_menu_request(&menu,token,open,context,now);
}
bool omni_native_menu_read(unsigned page,uint32_t out[15])
{
    initialize();return omni_remote_menu_status(&menu,page,out);
}
bool omni_native_headset_gain_read(unsigned page,uint32_t out[15])
{ initialize();return omni_headset_gain_status(&headset,page,out); }
