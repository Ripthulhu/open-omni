#include "board_clock.h"
#include "usb_device_config.h"
#include "usb.h"
#include "usb_device.h"
#include "usb_device_class.h"
#include "usb_device_hid.h"
#include "fsl_device_registers.h"
#include "boot_ack.h"
#include "audio_probe.h"
#include "microphone.h"
#include "ui.h"
#include "display_lpc5528.h"
#include "charger.h"
#include "charger_adc.h"
#include "usb_guard.h"
#include "mcu2_probe.h"
#include "mcu2_controls.h"
#include "dsp_probe.h"
#include "dsp_capture_probe.h"
#include "audio_clock_snapshot.h"
#include "audio_clock.h"
#include "audio_clock_startup.h"
#include "audio_clock_lpc5528.h"
#include "control_uart.h"
#include "audio_mode.h"
#include "usb_audio_ring.h"
#include "sof_capture.h"
#include "usb_iso_lpc5528.h"
#include "dsp_volume_probe.h"
#include "native_gain_adapter.h"
#include "headset_battery.h"
#include "headset_query.h"
#include "dsp_meter.h"
#include "dsp_settings.h"
#include "interchip.h"
#include "mixer_ui.h"
#include "mixer_control.h"
#include "usb_names.h"
#include <string.h>

extern void board_init(void);
extern void board_usb_detach_wait(void);
extern void watchdog_feed(void);
extern void Fault_Handler(void);
extern uint8_t __image_end[];
extern volatile uint32_t omni_fault;

#ifndef OMNI_BUILD_ID
#define OMNI_BUILD_ID "omni-a-dev"
#endif
static const char build_id[] = OMNI_BUILD_ID;
/* Feature reports stage a complete bounded EQ blob before its one submission.
 * No UART work in EP0. Coverage prevents stale/missing chunk bytes being sent. */
static uint8_t settings_blob[128],settings_covered[128],settings_blob_id,settings_blob_length;
static uint32_t settings_blob_token;
static omni_link_parser runtime_dsp_parser;
static omni_mcu2_controls runtime_controls;
static uint32_t controls_forwarded_generation;
static void runtime_dsp_observe(uint8_t byte,uint32_t now)
{
    const uint8_t *frame;size_t length;
    omni_link_expire(&runtime_dsp_parser,now);
    if(omni_link_feed(&runtime_dsp_parser,byte,now,&frame,&length)) {
        (void)omni_mcu2_controls_observe(&runtime_controls,frame,length,now);
        uint8_t controls[5];
        if(runtime_controls.generation!=controls_forwarded_generation &&
           omni_mcu2_controls_snapshot(&runtime_controls,controls) &&
           omni_mcu2_forward_controls(controls[0],controls[1],controls[2],controls[3],controls[4]))
            controls_forwarded_generation=runtime_controls.generation;
        if(length==5u && frame[0]==0xdbu && frame[2]==0xe4u && frame[3]==3u && frame[4]>=1u && frame[4]<=3u)
            omni_mcu2_forward_lifecycle(frame[4]);
        if(length==5u && frame[0]==0xdbu && frame[2]==0x86u && frame[3]==3u && frame[4]<=3u)
            omni_mcu2_forward_dsp_state86(frame[4]);
        if(frame[0]==0xdbu && ((length==4u && (frame[2]==0x91u || (frame[2]==0xd2u && (frame[3]==6u || frame[3]==7u)))) || (length==6u && frame[2]==0xd2u && frame[3]==9u && frame[4]==3u)))
            omni_ui_remote_frame(frame,length);
    }
}
static usb_device_handle device;
static volatile uint8_t configuration;
static omni_sof_capture_t sof_capture;
static volatile uint32_t sof_requested;
static uint32_t sof_active_selector=1u;
static uint32_t sof_status[15];
/* One recovery preparation per boot. USB callbacks only enqueue; flash work
 * and reset execute in main. State: 0 idle, 1 queued, 2 ready, 3 failed. */
static volatile uint32_t recovery_state, recovery_token, recovery_result;
static volatile int32_t recovery_driver;
static volatile uint8_t recovery_commit;
static uint8_t request[64] __attribute__((aligned(4)));
static uint8_t response[64] __attribute__((aligned(4))) = {1};
/* NATIVE_COMMAND_MAILBOX_BEGIN: one IRQ producer, one main-loop consumer.
 * phase is published last; pending/active entries cannot be overwritten.
 * Coherent inactive-bank publication never waits inside the USB interrupt. */
static struct {
    uint32_t token,control,length,queued_ms;
    uint8_t value[OMNI_DSP_SETTINGS_MAX_VALUE];
    uint32_t terminal[2][15];
    volatile uint32_t phase; /*0 empty,1 pending,2 submitted,3 terminal*/
} settings_mail;
static struct {
    uint32_t side,queued_ms,dispatched_ms,generation;
    volatile uint32_t phase; /*0 empty,1 pending,2 submitted,3 rejected,4 cancelled,5 runtime terminal*/
} select_mail;
typedef struct {
    uint32_t status[2][15],mcu2[2][15],menu[2][15],headset_gain[2][15];
    uint32_t controls[15],mcu2_gain[15],mixer_control[15];
    uint8_t values[DSP_SETTING_COUNT-1u][4][60];
} native_command_snapshot;
static native_command_snapshot native_snapshot[2];
static volatile unsigned native_published;
static uint32_t native_published_ms;
static bool native_have_snapshot;

static bool settings_enqueue(uint32_t token,unsigned control,const uint8_t *value,size_t length,uint32_t now)
{
    if(!token || token>=0x80000000u || !omni_dsp_settings_valid(control,value,length)) return false;
    if(settings_mail.phase && settings_mail.token==token)
        return settings_mail.control==control && settings_mail.length==length && !memcmp(settings_mail.value,value,length);
    if(settings_mail.phase==1u || settings_mail.phase==2u) return false;
    settings_mail.token=token;settings_mail.control=control;settings_mail.length=(uint32_t)length;
    settings_mail.queued_ms=now;memcpy(settings_mail.value,value,length);
    __DMB();settings_mail.phase=1u;return true;
}
static void settings_terminal(unsigned phase,uint32_t now)
{
    memset(settings_mail.terminal,0,sizeof(settings_mail.terminal));
    for(unsigned page=0;page<2u;++page) {
        uint32_t *v=settings_mail.terminal[page];
        v[0]=1;v[1]=page;v[2]=settings_mail.token;v[3]=settings_mail.control;v[4]=phase;
    }
    settings_mail.terminal[0][6]=settings_mail.queued_ms;
    settings_mail.terminal[0][8]=now;settings_mail.terminal[0][13]=255u;
    __DMB();settings_mail.phase=3u;
}
static bool select_enqueue(unsigned side,uint32_t now)
{
    if(side>1u || select_mail.phase==1u) return false;
    if(select_mail.phase==2u) return false; /* main publishes completion */
    select_mail.side=side;select_mail.queued_ms=now;select_mail.dispatched_ms=0;
    ++select_mail.generation;__DMB();select_mail.phase=1u;return true;
}
static void native_commands_service(uint32_t now,bool allowed)
{
    if(settings_mail.phase==1u) {
        __DMB();
        if(!allowed) settings_terminal(DSP_SETTINGS_CANCELLED,now);
        else if((uint32_t)(now-settings_mail.queued_ms)>=2000u) settings_terminal(DSP_SETTINGS_TIMEOUT,now);
        else if(omni_dsp_settings_transport_fault()) settings_terminal(DSP_SETTINGS_IO_ERROR,now);
        else if(!omni_dsp_settings_busy() && omni_dsp_settings_request(settings_mail.token,
                settings_mail.control,settings_mail.value,settings_mail.length,now)) {
            __DMB();settings_mail.phase=2u;
        }
    }
    if(select_mail.phase==1u) {
        __DMB();
        select_mail.dispatched_ms=now;
        uint32_t phase=!allowed?4u:omni_mcu2_select_input((uint8_t)select_mail.side)?2u:3u;
        __DMB();select_mail.phase=phase;
    }
}
static void native_commands_publish(uint32_t now)
{
    /* Publish terminal host evidence before another internal transaction can
     * replace the backend status. Preserve it until the next admitted token. */
    if(settings_mail.phase==2u) {
        uint32_t current[15];
        (void)omni_dsp_settings_status(0,current);
        if(current[2]==settings_mail.token && !(current[5]&1u)) {
            memcpy(settings_mail.terminal[0],current,sizeof(current));
            (void)omni_dsp_settings_status(1,settings_mail.terminal[1]);
            __DMB();settings_mail.phase=3u;
        }
    }
    if(native_have_snapshot && (uint32_t)(now-native_published_ms)<5u) return;
    unsigned next=native_published^1u;
    native_command_snapshot *s=&native_snapshot[next];
    for(unsigned page=0;page<2u;++page) {
        (void)omni_dsp_settings_status(page,s->status[page]);
        (void)omni_mcu2_runtime_read(page,s->mcu2[page]);
        (void)omni_native_menu_read(page,s->menu[page]);
        (void)omni_native_headset_gain_read(page,s->headset_gain[page]);
    }
    for(unsigned control=1;control<DSP_SETTING_COUNT;++control)
        for(unsigned page=0;page<4u;++page)
            (void)omni_dsp_settings_value(control,page,s->values[control-1u][page]);
    omni_mixer_control_status(s->mixer_control);
    (void)omni_mcu2_runtime_read(2u,s->mcu2_gain);
    omni_mcu2_controls_status(&runtime_controls,s->controls);
    native_published_ms=now;__DMB();native_published=next;__DMB();native_have_snapshot=true;
    /* Publish the runtime result first. An IRQ must not replace a submitted
     * selection using an older bank that still reports IDLE. */
    if(select_mail.phase==2u && s->mcu2[1][7]>=3u) {
        __DMB();select_mail.phase=5u; /* runtime terminal, see page1 selection */
    }
}
static bool settings_status_copy(unsigned page,uint32_t out[15])
{
    if(page>1u) return false;
    if(settings_mail.phase==3u) {memcpy(out,settings_mail.terminal[page],60);return true;}
    const uint32_t *v=native_snapshot[native_published].status[page];
    if(settings_mail.phase && (settings_mail.phase==1u || v[2]!=settings_mail.token)) {
        memset(out,0,60);out[0]=1;out[1]=page;out[2]=settings_mail.token;out[3]=settings_mail.control;
        out[4]=DSP_SETTINGS_QUEUED;
        if(!page) {out[5]=1;out[6]=settings_mail.queued_ms;out[13]=255u;}
    } else if(!native_have_snapshot) {memset(out,0,60);out[0]=1;out[1]=page;}
    else memcpy(out,v,60);
    return true;
}
static bool settings_value_copy(unsigned control,unsigned page,uint8_t out[60])
{
    if(control<1u || control>=DSP_SETTING_COUNT || page>3u) return false;
    if(!native_have_snapshot) {
        uint32_t header[3]={1u,control,page};memset(out,0,60);memcpy(out,header,sizeof(header));return true;
    }
    memcpy(out,native_snapshot[native_published].values[control-1u][page],60);return true;
}
static bool mcu2_status_copy(unsigned page,uint32_t out[15])
{
    if(page>4u) return false;
    if(page==4u) {
        if(native_have_snapshot) memcpy(out,native_snapshot[native_published].mcu2_gain,60);
        else {memset(out,0,60);out[0]=1;out[1]=2;}
        return true;
    }
    if(page<2u) {
        if(native_have_snapshot) memcpy(out,native_snapshot[native_published].mcu2[page],60);
        else {memset(out,0,60);out[0]=1;out[1]=page;}
    }
    else if(page==2u) {
        memset(out,0,60);out[0]=1;out[1]=2;out[2]=select_mail.phase;out[3]=select_mail.side;
        out[4]=select_mail.queued_ms;out[5]=select_mail.dispatched_ms;out[6]=select_mail.generation;
    }
    else if(native_have_snapshot) memcpy(out,native_snapshot[native_published].controls,60);
    else {memset(out,0,60);out[0]=1;}
    return true;
}
/* NATIVE_COMMAND_MAILBOX_END */
/* DMA0 self-test storage (opcode 31), in OUR .bss. The descriptor table is
 * 512-byte aligned (SRAMBASE requirement); src/dst are generously oversized vs the
 * copied word count so any end-address-math error stays contained in our RAM. */
static uint32_t omni_dma_desc[64] __attribute__((aligned(512))); /* DMA descriptor table (covers ch0..ch15) */
static uint32_t omni_dma_src[64];
static uint32_t omni_dma_dst[128];                               /* also the opcode-34 RX ring buffer */

/* --- audio_mode (DSP handshake) ops adapter, opcode 35 --- */
static mcu2_link_io omni_am_io;                                  /* filled by omni_control_uart_start(3) */
static int am_write(void *ctx, const uint8_t *data, size_t length)
{
    (void)ctx;
    size_t i = 0;
    for (; i < length; ++i) {
        int r = omni_am_io.tx(omni_am_io.context, data[i]);      /* writes FC3 FIFOWR when TXNOTFULL */
        if (r < 0) return -1;
        if (r == 0) break;                                       /* backpressure: i bytes accepted */
    }
    return (int)i;
}
static omni_audio_mode_io_t am_tx_status(void *ctx)
{
    (void)ctx; /* Physical stop-bit completion, not merely an empty TX FIFO. */
    return omni_control_uart_tx_idle(3)
        ? OMNI_AUDIO_MODE_IO_COMPLETE : OMNI_AUDIO_MODE_IO_PENDING;
}
static bool am_pins_noop(void *ctx, bool enabled) { (void)ctx; (void)enabled; return true; }
static bool am_cfg_begin_noop(void *ctx, uint32_t sr) { (void)ctx; (void)sr; return true; }
static omni_audio_mode_io_t am_cfg_poll_noop(void *ctx) { (void)ctx; return OMNI_AUDIO_MODE_IO_COMPLETE; }

/* --- opcode 37/38/40 (full playback) ops: real I2S pin mux + TX-ring configure --- */
static uint8_t am_src_usb;   /* 0 = stream the built-in test tone; 1 = stream the USB-fed ring */
/* Main-loop playback lifecycle state (defined here so opcode 40's status read can see it). */
static omni_audio_mode_t pb_mode;
static omni_audio_format pb_format;
static omni_link_parser am_rx_parser;
static bool microphone_started;
static uint8_t pb_state;      /* 0 IDLE, 1 HANDSHAKE, 2 RUNNING, 3 FAULT, 4 UART QUIESCING */
static bool pb_stop_fault;
static uint32_t pb_fails;
static uint32_t pb_failure_epoch,pb_failure_ms,pb_failure_mode,pb_failure_ordinal;
static uint32_t pb_failure_uart[6];
static void playback_startup_status(uint32_t out[15]);
static uint32_t pb_clock_attempts;
static volatile uint32_t pb_clock_state, pb_clock_error;
/* Preserve the first startup failure, including the exact compared fields.
 * A partially programmed clock is never blindly retried in this boot. */
static uint32_t pb_clock_first_failure[15];
static bool pb_clock_blocked;
static void remember_clock_failure(const omni_audio_clock_t *clock, bool wrote)
{
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    if (!pb_clock_first_failure[1]) {
        uint32_t v[15]={2u,1u,pb_clock_state,pb_clock_error,
            clock?clock->step:0u,clock?clock->successful_writes:0u,
            clock?clock->mismatch_reg:0u,clock?clock->mismatch_expected:0u,
            clock?clock->mismatch_actual:0u,clock?clock->mismatch_mask:0u,
            clock?clock->initial[OMNI_AC_FRO192M_CTRL]:0u,
            clock?clock->started_ms:0u,clock?clock->phase_ms:0u,
            clock?clock->last_status:0u,0u};
        memcpy(pb_clock_first_failure,v,sizeof(v));
    }
    if (wrote) pb_clock_blocked=true;
    __set_PRIMASK(mask);
}
static bool am_pins_route(void *ctx, bool enabled)
{
    (void)ctx;
    uint32_t v = enabled ? 0x4101u : 0x4100u;                    /* FUNC1 (I2S) vs FUNC0 (GPIO) */
    *(volatile uint32_t *)(uintptr_t)0x40001090u = v;            /* PIO1_4  FC0_SCK  */
    *(volatile uint32_t *)(uintptr_t)0x40001098u = v;            /* PIO1_6  FC0_WS   */
    *(volatile uint32_t *)(uintptr_t)0x400010e0u = v;            /* PIO1_24 FC2 data */
    return true;
}
static bool am_txring_begin(void *ctx, uint32_t sr)
{
    omni_audio_format legacy;
    const omni_audio_format *format=ctx;
    if(!format) {
        if(!omni_audio_format_make(&legacy,sr,16u,0u)) return false;
        format=&legacy;
    }
    if(!omni_audio_format_valid(format) || format->sample_rate!=sr ||
       (!am_src_usb && sr!=48000u)) return false; /* Built-in research tone is 48k only. */
    if(am_src_usb && !usb_audio_ring_start_format(format)) return false;
    /* Runs with external pins isolated. Prepare DMA accounting before enabling
     * either I2S block: internal clock sharing can consume data before pin routing.
     * Keep the recovered isolate/configure/route ordering for the DSP bus. */
    *(volatile uint32_t *)(uintptr_t)0x40000220u = 0x2000u | (1u << 20) | (1u << 11);
    *(volatile uint32_t *)(uintptr_t)0x40000224u = (1u << 11) | (1u << 13);
    *(volatile uint32_t *)(uintptr_t)0x40000228u = 0x8000u;
    *(volatile uint32_t *)(uintptr_t)0x40000140u = 1u << 20;
    *(volatile uint32_t *)(uintptr_t)0x40000144u = (1u << 11) | (1u << 13);
    *(volatile uint32_t *)(uintptr_t)0x400003c4u = 0u;
    *(volatile uint32_t *)(uintptr_t)0x40000320u = 0xffu; *(volatile uint32_t *)(uintptr_t)0x40000328u = 0xffu;
    *(volatile uint32_t *)(uintptr_t)0x400002b0u = 1u; *(volatile uint32_t *)(uintptr_t)0x400002b8u = 1u;
    *(volatile uint32_t *)(uintptr_t)0x40023000u = 0u; *(volatile uint32_t *)(uintptr_t)0x40023080u = 0u;
    *(volatile uint32_t *)(uintptr_t)0x40023048u = 0x101u; *(volatile uint32_t *)(uintptr_t)0x40023040u = 0x101u;
    *(volatile uint32_t *)(uintptr_t)0x40023000u = 1u;
    *(volatile uint32_t *)(uintptr_t)0x40086ff8u = 0x5u; *(volatile uint32_t *)(uintptr_t)0x40086c00u = 0xf0430u;
    *(volatile uint32_t *)(uintptr_t)0x40086c04u = 0x3fu;
    *(volatile uint32_t *)(uintptr_t)0x40086c1cu = sr==96000u?7u:15u;
    *(volatile uint32_t *)(uintptr_t)0x40086e00u = 0x20002u;
    *(volatile uint32_t *)(uintptr_t)0x40088ff8u = 0x4u; *(volatile uint32_t *)(uintptr_t)0x40088c00u = 0x1f0000u;
    *(volatile uint32_t *)(uintptr_t)0x40088c04u = 0x3fu; *(volatile uint32_t *)(uintptr_t)0x40088c1cu = 0x0u;
    /* UM11126 requires TXI2SE0 for stereo DATALEN>24; stock uses it too. */
    *(volatile uint32_t *)(uintptr_t)0x40088e00u = 0x10005u; *(volatile uint32_t *)(uintptr_t)0x40088e08u = 0x401u;
    *(volatile uint32_t *)(uintptr_t)0x40088e04u = 1u; /* clear inherited TXERR before measurement */
    *(volatile uint32_t *)(uintptr_t)0x40082000u = 1u;
    *(volatile uint32_t *)(uintptr_t)0x40082008u = (uint32_t)(uintptr_t)omni_dma_desc;
    /* DMA source: USB uses alternating 1ms blocks refilled by its IRQ; the
     * USB producer never writes DMA-owned memory. Or use the built-in ~1kHz tone
     * (96 32-bit slots = 1 ms, looped). WIDTH32|SRCINC1|DSTINC0|CFGVALID|RELOAD|SETINTA;
     * XFERCOUNT = words-1 in bits[25:16]. */
    uint32_t tx_words = am_src_usb ? usb_audio_ring_dma_words() : 96u;
    uint32_t tx_src   = am_src_usb ? (uint32_t)(uintptr_t)&omni_usb_audio[0]
                                   : (uint32_t)(uintptr_t)&omni_dma_dst[0];
    uint32_t tx_xfercfg = 0x1213u | ((tx_words - 1u) << 16);
    if (!am_src_usb) {
        for (uint32_t k = 0; k < 48u; ++k) {
            uint32_t s = (k < 24u) ? 0x0c000000u : 0xf4000000u; /* +/- ~0.09 FS square, mono */
            omni_dma_dst[2u * k] = s; omni_dma_dst[2u * k + 1u] = s;
        }
        for (uint32_t i = 96u; i < 128u; ++i) omni_dma_dst[i] = 0u;
    }
    omni_dma_desc[44] = tx_xfercfg;
    omni_dma_desc[45] = tx_src + 4u * (tx_words - 1u);         /* srcEnd = buf + width*(count-1) */
    omni_dma_desc[46] = 0x40088e20u;                          /* dstEnd = I2S2 FIFOWR (fixed)   */
    omni_dma_desc[47] = (uint32_t)(uintptr_t)&omni_dma_desc[44];
    if (am_src_usb) {
        usb_audio_ring_dma_setup(&omni_dma_desc[44]);
        if(usb_audio_ring_fault()) return false;
    }
    INPUTMUX->DMA0_REQ_ENA_SET = (1u << 11);
    *(volatile uint32_t *)(uintptr_t)0x400824b0u = 0x10001u;
    *(volatile uint32_t *)(uintptr_t)0x40082058u = 0x800u;
    *(volatile uint32_t *)(uintptr_t)0x40082020u = 0x800u;
    *(volatile uint32_t *)(uintptr_t)0x400824b8u = tx_xfercfg;
    if (am_src_usb) usb_audio_ring_activate(); /* consumer accounting ready BEFORE clock/trigger */
    *(volatile uint32_t *)(uintptr_t)0x40086c00u = 0xf0431u;     /* internal shared clock may already run */
    *(volatile uint32_t *)(uintptr_t)0x40088e00u = 0x11005u;     /* DMATX + TXI2SE0 */
    *(volatile uint32_t *)(uintptr_t)0x40082070u = 0x800u;       /* SETTRIG ch11 (pre-fill FIFO) */
    *(volatile uint32_t *)(uintptr_t)0x40088c00u = 0x1f0001u;    /* I2S2 slave enable */
    return true;
}
static omni_audio_mode_io_t am_txring_poll(void *ctx) { (void)ctx; return OMNI_AUDIO_MODE_IO_COMPLETE; }
static void am_txring_teardown(void)
{
    usb_audio_ring_stop(); /* stop producer+IRQ, disable DMA and wait BUSY before reuse */
    microphone_started=false;
    *(volatile uint32_t *)(uintptr_t)0x40088c00u = 0x1f0000u;    /* I2S2 off */
    *(volatile uint32_t *)(uintptr_t)0x40086c00u = 0xf0430u;     /* I2S0 off */
    *(volatile uint32_t *)(uintptr_t)0x40082028u = 0x800u;       /* ENABLECLR ch11 */
    INPUTMUX->DMA0_REQ_ENA_CLR = (1u << 11);
    *(volatile uint32_t *)(uintptr_t)0x40088e00u = 0x10005u;     /* drop DMATX; keep TXI2SE0 */
    *(volatile uint32_t *)(uintptr_t)0x40023000u = 0u;
    *(volatile uint32_t *)(uintptr_t)0x40023040u = 0u; *(volatile uint32_t *)(uintptr_t)0x40023048u = 0u;
    *(volatile uint32_t *)(uintptr_t)0x40023000u = 1u;           /* share -> dedicated */
    *(volatile uint32_t *)(uintptr_t)0x40001090u = 0x4100u;      /* pins -> GPIO */
    *(volatile uint32_t *)(uintptr_t)0x40001098u = 0x4100u;
    *(volatile uint32_t *)(uintptr_t)0x400010e0u = 0x4100u;
}
/* Private research on the original device's VID/PID; bcdDevice and strings
 * distinguish this application. No allocation claim for redistribution. */
static uint8_t device_desc[] = {18,1,0,2,0xef,2,1,64,0x38,0x10,0x90,0x22,0,0xb0,1,2,3,1};
static uint8_t report_desc[] = {
    0x06,0xc0,0xff,0x09,1,0xa1,1,0x85,1,0x15,0,0x26,0xff,0,
    0x75,8,0x95,63,0x09,1,0xb1,2,0x09,1,0x81,2,0xc0
};
#define config_desc omni_audio_config
static uint8_t string_buf[126] __attribute__((aligned(4)));
static usb_device_endpoint_struct_t endpoints[] = {{0x81,USB_ENDPOINT_INTERRUPT,64,10}};
static usb_device_interface_struct_t interface[] = {{0,{1,endpoints},NULL}};
static usb_device_interfaces_struct_t interfaces[] = {{3,0,0,OMNI_HID_INTERFACE,interface,1}};
static usb_device_interface_list_t lists[] = {{1,interfaces}};
static usb_device_class_struct_t hid_class = {lists,kUSB_DeviceClassTypeHid,1};
static usb_device_endpoint_struct_t ac_endpoints[]={{0x82,USB_ENDPOINT_INTERRUPT,6,4}};
static usb_device_endpoint_struct_t play_endpoints[]={{3,USB_ENDPOINT_ISOCHRONOUS,388,1},
    {0x84,USB_ENDPOINT_ISOCHRONOUS,4,1}}; /* iso OUT data + iso IN async feedback */
static usb_device_endpoint_struct_t play24_endpoints[]={{3,USB_ENDPOINT_ISOCHRONOUS,582,1},
    {0x84,USB_ENDPOINT_ISOCHRONOUS,4,1}};
static usb_device_endpoint_struct_t mic_endpoints[]={{0x83,USB_ENDPOINT_ISOCHRONOUS,98,1}};
static usb_device_interface_struct_t ac_alts[]={{0,{1,ac_endpoints},NULL}};
static usb_device_interface_struct_t play_alts[]={{0,{0,NULL},NULL},
    {1,{2,play_endpoints},NULL},{2,{2,play24_endpoints},NULL}};
static usb_device_interface_struct_t mic_ac_alts[]={{0,{0,NULL},NULL}};
static usb_device_interface_struct_t mic_alts[]={{0,{0,NULL},NULL},{1,{1,mic_endpoints},NULL}};
static usb_device_interfaces_struct_t audio_interfaces[]={
    {1,1,0x20,OMNI_PLAYBACK_AC_INTERFACE,ac_alts,1},
    {1,2,0x20,OMNI_PLAYBACK_AS_INTERFACE,play_alts,3},
    {1,1,0x20,OMNI_MICROPHONE_AC_INTERFACE,mic_ac_alts,1},
    {1,2,0x20,OMNI_MICROPHONE_AS_INTERFACE,mic_alts,2}};
static usb_device_interface_list_t audio_lists[]={{4,audio_interfaces}};
static usb_device_class_struct_t audio_class={audio_lists,kUSB_DeviceClassTypeAudio,1};

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void put32(uint8_t *p, uint32_t x)
{
    for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(x>>(8*i));
}

/* EP0 callbacks serialize bounded reads and enqueue explicit recovery work.
 * No flash-controller operations or reset execute inside the callback. */
static void command(void)
{
    memset(response,0,sizeof(response));
    response[0]=1; response[1]=request[1]; response[2]=request[2];
    /* Legacy bring-up probes overwrite the shared DMA table/clock routing.
     * They cannot run while an audio stream or its teardown owns the bus. */
    if(request[1]>=27u && request[1]<=39u && (pb_state!=0u ||
        audio_probe_alternate(1)!=0 || audio_probe_alternate(OMNI_MICROPHONE_AS_INTERFACE)!=0)) {
        response[3]=2; return;
    }
    switch(request[1]) {
    case 1:
        memcpy(response+4,"OMNI",4);
        response[8]=1; /* protocol */
        response[9]=1; /* MCU1 */
        response[10]=251; /* identity/readback/ack/recovery/UAC2/error trace/UI */
        put32(response+12,0xc000);
        put32(response+16,(uint32_t)__image_end);
        memcpy(response+20,build_id,sizeof(build_id));
        break;
    case 2: {
        uint32_t address=get32(request+4), count=request[8];
        if (count==0 || count>56 || address<0xc000 || address>(uint32_t)__image_end ||
            count>(uint32_t)__image_end-address) { response[3]=2; break; }
        memcpy(response+8,(const void *)address,count);
        response[4]=(uint8_t)count;
        break;
    }
    case 3: put32(response+4,omni_fault); break;
    case 4:
        put32(response+4,boot_ack_status);
        put32(response+8,(uint32_t)boot_ack_driver_status);
        break;
    case 5: /* Prepare: nonzero session token and literal BOOT guard. */
        if (!get32(request+4) || memcmp(request+8,"BOOT",4) ||
            (boot_ack_status != OMNI_ACK_WRITTEN && boot_ack_status != OMNI_ACK_ALREADY_VALID)) {
            response[3]=2; break;
        }
        if (recovery_state) {
            if (recovery_token != get32(request+4)) response[3]=3;
            break; /* Same-token retries never issue a second erase. */
        }
        recovery_token=get32(request+4);
        recovery_state=1;
        break;
    case 6:
        put32(response+4,recovery_state);
        put32(response+8,recovery_token);
        put32(response+12,recovery_result);
        put32(response+16,(uint32_t)recovery_driver);
        break;
    case 7: /* Commit only after the host has observed verified preparation. */
        if (recovery_state!=2 || get32(request+4)!=recovery_token ||
            memcmp(request+8,"BOOT",4)) { response[3]=2; break; }
        recovery_commit=1;
        break;
    case 8: audio_probe_status(response+4); break;
    case 10: audio_probe_request_trace(request[4],response+4); break;
    case 14: omni_usb_guard_status(response+4); break;
    case 16:
        if (memcmp(request+8,"MCU2",4) || recovery_state || omni_dsp_probe_busy() || omni_dsp_capture_probe_busy() ||
            (boot_ack_status!=OMNI_ACK_WRITTEN && boot_ack_status!=OMNI_ACK_ALREADY_VALID) ||
            audio_probe_alternate(1)!=0 || audio_probe_alternate(OMNI_MICROPHONE_AS_INTERFACE)!=0 ||
            !(request[12] == 0U ? omni_mcu2_probe_request(get32(request+4)) :
              omni_mcu2_probe_request_query(get32(request+4), request[12]))) response[3]=2;
        break;
    case 17: omni_mcu2_probe_status(response+4); break;
    case 56: {
        uint32_t values[15];
        if(!omni_mcu2_probe_trace(request[4],values)) { response[3]=2; break; }
        for(unsigned i=0;i<15U;++i) put32(response+4U+4U*i,values[i]);
        break;
    }
    case 18:
        if (memcmp(request+8,"DSP1",4) || recovery_state || omni_dsp_capture_probe_busy() ||
            (boot_ack_status!=OMNI_ACK_WRITTEN && boot_ack_status!=OMNI_ACK_ALREADY_VALID) ||
            audio_probe_alternate(1)!=0 || audio_probe_alternate(OMNI_MICROPHONE_AS_INTERFACE)!=0 ||
            !omni_dsp_probe_request(get32(request+4))) response[3]=2;
        break;
    case 19: omni_dsp_probe_status(response+4); break;
    case 21: omni_dsp_probe_trace(response+4); break;
    case 22:
        if (memcmp(request+8,"DSPC",4) || configuration!=1 || recovery_state ||
            omni_dsp_probe_busy() || omni_mcu2_probe_busy() ||
            (boot_ack_status!=OMNI_ACK_WRITTEN && boot_ack_status!=OMNI_ACK_ALREADY_VALID) ||
            audio_probe_alternate(1)!=0 || audio_probe_alternate(OMNI_MICROPHONE_AS_INTERFACE)!=0 ||
            !omni_dsp_capture_probe_request(get32(request+4),get32(request+12))) response[3]=2;
        break;
    case 23:
        if (!omni_dsp_capture_probe_status(request[4],response+4)) response[3]=2;
        break;
    case 24:
        if (!omni_dsp_capture_probe_record(get32(request+4),request[8],request[9],response+4)) response[3]=2;
        break;
    case 25: {
        uint32_t snapshot[OMNI_AUDIO_CLOCK_SNAPSHOT_WORDS];
        if (request[4]>3 || !omni_audio_clock_snapshot(snapshot)) { response[3]=2; break; }
        put32(response+4,1); put32(response+8,request[4]); put32(response+12,snapshot[1]);
        for (unsigned i=0;i<10U;++i) put32(response+16+4U*i,snapshot[10U*request[4]+i]);
        break;
    }
    case 26: omni_dsp_probe_reply(response+4); break;
    case 20: {
        uint32_t snapshot[20];
        if (request[5]>1 || !omni_control_uart_snapshot(request[4],snapshot)) { response[3]=2; break; }
        put32(response+4,1); put32(response+8,request[4]); put32(response+12,request[5]);
        for (unsigned i=0;i<10U;++i) put32(response+16+4U*i,snapshot[10U*request[5]+i]);
        break;
    }
    case 15:
        if (audio_probe_endpoint_snapshot(request[4],request[5],response+4)!=0) response[3]=2;
        break;
    case 11: audio_probe_error_trace(request[4],response+4); break;
    case 12: audio_probe_clock_snapshot(response+4); break;
    case 13: omni_ui_status(response+4); break;
    case 9: {
        uint16_t raw=(uint16_t)((uint16_t)request[4]|((uint16_t)request[5]<<8));
        int16_t db=(int16_t)(raw>=0x8000U?(int32_t)raw-65536:(int32_t)raw);
        if (audio_probe_local(db,request[6])!=0) response[3]=2;
        break;
    }
    case 27: { /* XO32M 16 MHz crystal bring-up probe. Powers + configures ONLY
                * the crystal oscillator with the recovered stock config and
                * polls XO_READY. Touches no PLL0, no clock selector, no I2S/DMA,
                * no audio -- CPU/USB stay on the FRO and the XO is never routed
                * anywhere, so nothing depends on it. Read-mostly diagnostic. */
        volatile uint32_t *const pdrun_clr  = (volatile uint32_t *)(uintptr_t)0x400200c8u; /* PMC.PDRUNCFGCLR0 */
        volatile uint32_t *const clock_ctrl = (volatile uint32_t *)(uintptr_t)0x40000a18u; /* SYSCON.CLOCK_CTRL */
        volatile uint32_t *const xo_ctrl    = (volatile uint32_t *)(uintptr_t)0x40013020u; /* ANACTRL.XO32M_CTRL */
        volatile const uint32_t *const xo_status  = (volatile const uint32_t *)(uintptr_t)0x40013024u;
        volatile const uint32_t *const pdruncfg0  = (volatile const uint32_t *)(uintptr_t)0x400200b8u;
        volatile const uint32_t *const ahbclk2    = (volatile const uint32_t *)(uintptr_t)0x40000208u;
        volatile const uint32_t *const preset2    = (volatile const uint32_t *)(uintptr_t)0x40000108u;
        if (((*ahbclk2 >> 27) & 1u) == 0u || ((*preset2 >> 27) & 1u) != 0u) { response[3] = 2; break; } /* ANACTRL not clocked */
        uint32_t before_pdrun = *pdruncfg0, before_clk = *clock_ctrl, before_xo = *xo_ctrl, before_st = *xo_status;
        uint32_t mask = __get_PRIMASK(); __disable_irq();
        *pdrun_clr = 0x100u;              /* power up XO32M (bit 8): CLR reg, non-destructive */
        *pdrun_clr = 0x100000u;           /* power up XO32M LDO (bit 20)                      */
        *clock_ctrl = before_clk | 0x20u; /* set stock's XO bit, preserve the rest           */
        *xo_ctrl = 0x01c3459au;           /* recovered stock XO32M_CTRL                       */
        __set_PRIMASK(mask);
        uint32_t iters = 0, ready = 0;
        for (; iters < OMNI_CPU_GUARD_ITERATIONS(20000u); ++iters) { if ((*xo_status & 1u) != 0u) { ready = 1; break; } }
        put32(response + 4, before_pdrun);
        put32(response + 8, before_clk);
        put32(response + 12, before_xo);
        put32(response + 16, before_st);
        put32(response + 20, *xo_ctrl);    /* after: XO32M_CTRL readback */
        put32(response + 24, *xo_status);  /* after: XO32M_STATUS        */
        put32(response + 28, iters);
        put32(response + 32, *clock_ctrl); /* after: CLOCK_CTRL          */
        put32(response + 36, ready);
        break;
    }
    case 28: { /* PLL0 lock probe. Ensures the XO is up (as in opcode 27), then
                * configures PLL0 with the recovered stock values and polls
                * PLL0STAT lock. STOPS before any PLL0DIV/MCLKDIV/MCLKSEL/selector
                * write, so PLL0 locks but is NOT routed anywhere -- CPU/USB stay
                * on the FRO and nothing depends on the PLL. Lock can take up to
                * ~20 ms, so a settle re-read (opcode 25 PLL0STAT) is authoritative. */
        volatile uint32_t *const pdrun_set  = (volatile uint32_t *)(uintptr_t)0x400200c0u; /* PMC.PDRUNCFGSET0 */
        volatile uint32_t *const pdrun_clr  = (volatile uint32_t *)(uintptr_t)0x400200c8u; /* PMC.PDRUNCFGCLR0 */
        volatile uint32_t *const clock_ctrl = (volatile uint32_t *)(uintptr_t)0x40000a18u;
        volatile uint32_t *const xo_ctrl    = (volatile uint32_t *)(uintptr_t)0x40013020u;
        volatile const uint32_t *const xo_status = (volatile const uint32_t *)(uintptr_t)0x40013024u;
        volatile uint32_t *const pll0sel    = (volatile uint32_t *)(uintptr_t)0x40000290u; /* SYSCON.PLL0CLKSEL */
        volatile uint32_t *const pll0ctrl   = (volatile uint32_t *)(uintptr_t)0x40000580u;
        volatile uint32_t *const pll0ndec   = (volatile uint32_t *)(uintptr_t)0x40000588u;
        volatile uint32_t *const pll0pdec   = (volatile uint32_t *)(uintptr_t)0x4000058cu;
        volatile uint32_t *const pll0sscg0  = (volatile uint32_t *)(uintptr_t)0x40000590u;
        volatile uint32_t *const pll0sscg1  = (volatile uint32_t *)(uintptr_t)0x40000594u;
        volatile const uint32_t *const pll0stat = (volatile const uint32_t *)(uintptr_t)0x40000584u;
        volatile const uint32_t *const ahbclk2  = (volatile const uint32_t *)(uintptr_t)0x40000208u;
        volatile const uint32_t *const preset2  = (volatile const uint32_t *)(uintptr_t)0x40000108u;
        if (((*ahbclk2 >> 27) & 1u) == 0u || ((*preset2 >> 27) & 1u) != 0u) { response[3] = 2; break; }
        uint32_t before_stat = *pll0stat;
        uint32_t mask = __get_PRIMASK(); __disable_irq();
        *pdrun_clr = 0x100u; *pdrun_clr = 0x100000u; /* XO power up */
        *clock_ctrl = *clock_ctrl | 0x20u;
        *xo_ctrl = 0x01c3459au;                      /* XO config */
        __set_PRIMASK(mask);
        uint32_t xo_iters = 0, xo_ready = 0;
        for (; xo_iters < OMNI_CPU_GUARD_ITERATIONS(20000u); ++xo_iters) { if ((*xo_status & 1u) != 0u) { xo_ready = 1; break; } }
        put32(response + 4, xo_ready);
        put32(response + 8, xo_iters);
        put32(response + 12, before_stat);
        if (!xo_ready) { put32(response + 16, *pll0stat); put32(response + 24, 0); break; } /* no source */
        mask = __get_PRIMASK(); __disable_irq();
        *pll0sel = 1u;                               /* PLL0 input = XO32M */
        *pdrun_set = 0x200u; *pdrun_set = 0x800000u; /* power down PLL0 for config */
        *pll0ctrl = 0x00207ca0u;
        *pll0ndec = 0x19u; *pll0ndec = 0x119u;       /* NDEC + NREQ latch */
        *pll0pdec = 0x5u;  *pll0pdec = 0x25u;        /* PDEC + PREQ latch */
        *pll0sscg0 = 0u;
        *pll0sscg1 = 0x100c0000u; *pll0sscg1 = 0x140c0002u; /* SSCG1 + MD latch */
        *pdrun_clr = 0x200u; *pdrun_clr = 0x800000u; /* power up PLL0 */
        __set_PRIMASK(mask);
        uint32_t lock_iters = 0, locked = 0;
        for (; lock_iters < OMNI_CPU_GUARD_ITERATIONS(50000u); ++lock_iters) { if ((*pll0stat & 1u) != 0u) { locked = 1; break; } }
        put32(response + 16, *pll0stat);   /* PLL0STAT after (immediate) */
        put32(response + 20, lock_iters);
        put32(response + 24, locked);
        put32(response + 28, *pll0ctrl);   /* readbacks */
        put32(response + 32, *pll0ndec);
        put32(response + 36, *pll0pdec);
        break;
    }
    case 29: { /* LOCAL_READY bring-up via the SHIPPABLE audio-clock sequencer.
                * Binds the REAL omni_audio_clock_lpc5528 backend (its ops + guard +
                * recovered address table) to the omni_audio_clock state machine and
                * drives it init->begin->poll to LOCAL_READY: XO -> PLL0 49.152 MHz ->
                * MCLK 24.576 MHz, all OWNED INTERNALLY. This opcode hand-writes NO
                * clock register: every store is emitted by the sequencer. MCLKIO stays
                * an input, FC0/FC2 stay off PLL0, MAINCLKSEL is never touched -- so
                * CPU/USB remain on the FRO and PLL0/MCLK are locked but routed nowhere.
                * PRECONDITION (enforced by the sequencer's preflight; fails safe with 0
                * writes if unmet): ANACTRL clocked, and XO32M_CTRL already configured to
                * the board-validated value -- run opcode 27 first in the same power
                * session (it left XO32M_CTRL=0x01C3459A and asserted XO_READY).
                *
                * TIME BASE: the free-running SysTick down-counter, NOT the ms tick var.
                * This runs in the USB SetReport callback inside USB0_IRQHandler
                * (priority 3); SysTick_Handler (priority 7) cannot preempt it, so the
                * omni_ui_milliseconds() variable would be FROZEN here and the sequencer
                * (which needs >=2 ms of real elapsed time to accept PLL lock, and a
                * 500 ms total timeout) could never progress or time out -> infinite USB
                * stall. But the SysTick VAL register is a hardware down-counter that
                * decrements every CPU cycle regardless of ISR context or whether its
                * interrupt fires (ui.c already enabled it: LOAD=95999, ENABLE set).
                * Unlike DWT->CYCCNT it needs no TRCENA / CoreSight software-lock unlock
                * and is not gated by debug authentication, so it is reliable on a sealed
                * production part (CYCCNT would not tick here). CPU/AHB = FRO 96 MHz
                * (SystemCoreClock; board.c kFRO_HF_to_MAIN_CLK, AHB divisor1).
                * The configured reload period LOAD+1 is one millisecond. We
                * accumulate per-iteration VAL deltas (wrap-safe across the reload) into a
                * monotonic ms count; no single poll iteration approaches 1 ms, so no
                * reload is ever missed. BOUND: the loop exits on the sequencer's own
                * 500 ms total timeout (once now_ms is confirmed advancing); a large
                * iteration cap is an unreachable backstop, and watchdog_feed() runs each
                * iteration so the bounded window cannot trip the WWDT. No
                * I2S/DMA/audio/flash/selector/routing writes. */
        volatile const uint32_t *const ahbclk2 = (volatile const uint32_t *)(uintptr_t)0x40000208u; /* SYSCON.AHBCLKCTRL2 */
        volatile const uint32_t *const preset2 = (volatile const uint32_t *)(uintptr_t)0x40000108u; /* SYSCON.PRESETCTRL2 */
        if (((*ahbclk2 >> 27) & 1u) == 0u || ((*preset2 >> 27) & 1u) != 0u) { response[3] = 2; break; } /* ANACTRL not accessible */

        /* CONFIRM the SysTick counter is enabled and advancing before trusting it. */
        const uint32_t systick_period = SysTick->LOAD + 1u; /* cycles per configured 1 ms reload */
        if ((SysTick->CTRL & SysTick_CTRL_ENABLE_Msk) == 0u || systick_period < 2u) {
            response[3] = 2; put32(response + 60, 0xdead0000u); break; /* no usable timebase */
        }
        uint32_t probe_a = SysTick->VAL;
        for (volatile uint32_t spin = 0; spin < 64u; ++spin) { /* burn a few cycles */ }
        uint32_t probe_b = SysTick->VAL;
        if (probe_a == probe_b) { /* not advancing: refuse rather than risk a stall */
            response[3] = 2; put32(response + 60, 0xdead0001u); break;
        }

        omni_audio_clock_lpc5528_t backend;
        backend.mmio_read = 0; backend.mmio_write = 0; backend.user = 0; /* NULL -> real volatile MMIO */
        omni_audio_clock_ops_t ops = omni_audio_clock_lpc5528_ops(&backend);
        omni_audio_clock_t clk;
        uint32_t init_ok = omni_audio_clock_init(&clk, &ops) ? 1u : 0u;
        uint32_t begin_ok = 0u;
        if (init_ok)
            begin_ok = omni_audio_clock_begin(&clk, OMNI_AUDIO_CLOCK_REFERENCE_HZ,
                                              0x01c3459au, true, 0u) ? 1u : 0u;
        uint32_t iters = 0u;
        uint32_t now_ms = 0u;       /* monotonic elapsed ms, accumulated from VAL deltas */
        uint32_t rem_cycles = 0u;   /* sub-ms cycle remainder                            */
        uint32_t prev_val = SysTick->VAL;
        if (begin_ok) {
            for (; iters < OMNI_CPU_GUARD_ITERATIONS(2000000u); ++iters) {
                if (clk.state >= OMNI_AUDIO_CLOCK_LOCAL_READY) break; /* LOCAL_READY / FAILED / CANCELED */
                uint32_t cur_val = SysTick->VAL; /* down-counter: normally decreasing */
                uint32_t delta = (prev_val >= cur_val) ? (prev_val - cur_val)
                                                       : (prev_val + systick_period - cur_val);
                prev_val = cur_val;
                rem_cycles += delta;
                while (rem_cycles >= systick_period) { rem_cycles -= systick_period; ++now_ms; }
                omni_audio_clock_poll(&clk, now_ms);
                watchdog_feed();
            }
        }
        uint32_t elapsed_ms = now_ms;

        /* Read-only integrity readbacks so the host confirms the end state directly. */
        volatile const uint32_t *const xo_status = (volatile const uint32_t *)(uintptr_t)0x40013024u; /* ANACTRL.XO32M_STATUS */
        volatile const uint32_t *const pll0stat  = (volatile const uint32_t *)(uintptr_t)0x40000584u; /* SYSCON.PLL0STAT      */
        volatile const uint32_t *const mclksel   = (volatile const uint32_t *)(uintptr_t)0x400002e0u; /* SYSCON.MCLKCLKSEL    */
        volatile const uint32_t *const mclkio    = (volatile const uint32_t *)(uintptr_t)0x40000420u; /* SYSCON.MCLKIO        */
        volatile const uint32_t *const fc0sel    = (volatile const uint32_t *)(uintptr_t)0x400002b0u; /* SYSCON.FCCLKSEL0     */
        volatile const uint32_t *const fc2sel    = (volatile const uint32_t *)(uintptr_t)0x400002b8u; /* SYSCON.FCCLKSEL2     */
        volatile const uint32_t *const mainclka  = (volatile const uint32_t *)(uintptr_t)0x40000280u; /* SYSCON.MAINCLKSELA   */

        put32(response + 4,  (clk.state == OMNI_AUDIO_CLOCK_LOCAL_READY) ? 1u : 0u);
        put32(response + 8,  (uint32_t)clk.state);
        put32(response + 12, (uint32_t)clk.error);
        put32(response + 16, clk.successful_writes);
        put32(response + 20, iters);
        put32(response + 24, elapsed_ms);
        put32(response + 28, clk.last_status);   /* last PLL0STAT/divider word the sequencer read */
        put32(response + 32, *xo_status);        /* bit0 = XO_READY                               */
        put32(response + 36, *pll0stat);         /* bit0 = PLL0 LOCK                              */
        put32(response + 40, *mclksel);          /* 1 = MCLK sourced from PLL0 (owned internally) */
        put32(response + 44, *mclkio);           /* bit0 = 0 -> MCLKIO still input (UNROUTED)     */
        put32(response + 48, *fc0sel);           /* &7 != 1 -> FC0 not on PLL0                    */
        put32(response + 52, *fc2sel);           /* &7 != 1 -> FC2 not on PLL0                    */
        put32(response + 56, *mainclka);         /* &7 == 0 -> CPU still on FRO12 (untouched)     */
        put32(response + 60, (begin_ok << 1) | init_ok);
        break;
    }
    case 30: { /* I2S + DMA0 programming-MODEL probe (NOT a stream).
                * Programs I2S0/FC0 (RX ch4) + I2S2/FC2 (TX ch11) + the DMA0 channel
                * CFG registers with the emulation-recovered stock values
                * (WIRELESS-AUDIO-DMA-2026-09-09.md) and reads them back, to prove the
                * register model holds on real silicon. Deliberately SAFE / no data
                * movement: it does NOT write DMA SRAMBASE, does NOT ENABLESET a channel,
                * and does NOT submit an XFERCFG (no CFGVALID) -- so the DMA controller
                * never fetches a descriptor from OUR RAM and no transfer can start. It
                * does NOT touch FCLKSEL/MCLKIO/IOCON, so no audio clock is routed to the
                * flexcomm or any pin (I2S may toggle BCLK internally off its leftover
                * function clock; nothing leaves the chip). No DSP interaction. This
                * makes NO electrical/rate/DSP claim -- only "the writes stick". */
        uint32_t w = 0u;
        #define OMNI_W(addr, val) do { *(volatile uint32_t *)(uintptr_t)(addr) = (uint32_t)(val); ++w; } while (0)
        /* 1. Clock + un-reset DMA0, FLEXCOMM0 (I2S0), FLEXCOMM2 (I2S2). */
        OMNI_W(0x40000220u, 1u << 20);              /* AHBCLKCTRLSET0: DMA0            */
        OMNI_W(0x40000224u, (1u << 11) | (1u << 13)); /* AHBCLKCTRLSET1: FC0 + FC2     */
        OMNI_W(0x40000140u, 1u << 20);              /* PRESETCTRLCLR0: DMA0 un-reset  */
        OMNI_W(0x40000144u, (1u << 11) | (1u << 13)); /* PRESETCTRLCLR1: FC0 + FC2     */
        /* 2. I2S0 / FC0 (RX) -- recovered rows 1-6 then MAINENABLE (row 14). */
        OMNI_W(0x40086ff8u, 0x5u);        /* PSELID  = I2S (PERSEL 5)                 */
        OMNI_W(0x40086c00u, 0xf0430u);    /* CFG1 (master, no MAINENABLE yet)         */
        OMNI_W(0x40086c04u, 0x3fu);       /* CFG2                                     */
        OMNI_W(0x40086c1cu, 0xfu);        /* DIV (48 kHz frame; magnitude from clock) */
        OMNI_W(0x40086e00u, 0x20002u);    /* FIFOCFG: RX FIFO on + RX DMA request     */
        OMNI_W(0x40086e08u, 0x40002u);    /* FIFOTRIG                                 */
        OMNI_W(0x40086c00u, 0xf0431u);    /* CFG1 |= MAINENABLE (final)               */
        /* 3. I2S2 / FC2 (TX) -- recovered rows 15-20 then MAINENABLE (row 26). */
        OMNI_W(0x40088ff8u, 0x4u);        /* PSELID  = I2S (PERSEL 4)                 */
        OMNI_W(0x40088c00u, 0x1f0000u);   /* CFG1 (slave, no MAINENABLE yet)          */
        OMNI_W(0x40088c04u, 0x3fu);       /* CFG2                                     */
        OMNI_W(0x40088c1cu, 0x0u);        /* DIV (slave branch, 0)                    */
        OMNI_W(0x40088e00u, 0x10005u);    /* FIFOCFG: TX FIFO on + TX DMA request     */
        OMNI_W(0x40088e08u, 0x401u);      /* FIFOTRIG                                 */
        OMNI_W(0x40088c00u, 0x1f0001u);   /* CFG1 |= MAINENABLE (final)               */
        /* 4. DMA0 controller + channel CFG only (NO SRAMBASE / ENABLESET / XFERCFG). */
        OMNI_W(0x40082000u, 0x1u);        /* CTRL = ENABLE (controller)               */
        OMNI_W(0x40082440u, 0x20001u);    /* CH4_CFG:  PERIPHREQEN + priority 2 (RX)  */
        OMNI_W(0x400824b0u, 0x10001u);    /* CH11_CFG: PERIPHREQEN + priority 1 (TX)  */
        #undef OMNI_W
        watchdog_feed();

        /* Read-only integrity + model readbacks. */
        volatile const uint32_t *const mainclka = (volatile const uint32_t *)(uintptr_t)0x40000280u; /* MAINCLKSELA */
        volatile const uint32_t *const usb0sel  = (volatile const uint32_t *)(uintptr_t)0x400002a8u; /* USB0CLKSEL  */
        volatile const uint32_t *const mclkio   = (volatile const uint32_t *)(uintptr_t)0x40000420u; /* MCLKIO      */

        put32(response + 4,  w);                                              /* bus writes issued (expect 21) */
        put32(response + 8,  *(volatile const uint32_t *)(uintptr_t)0x40086c00u); /* I2S0 CFG1  (expect 0xf0431) */
        put32(response + 12, *(volatile const uint32_t *)(uintptr_t)0x40086c04u); /* I2S0 CFG2  (0x3f)          */
        put32(response + 16, *(volatile const uint32_t *)(uintptr_t)0x40086e00u); /* I2S0 FIFOCFG (0x20002)     */
        put32(response + 20, *(volatile const uint32_t *)(uintptr_t)0x40086e04u); /* I2S0 FIFOSTAT             */
        put32(response + 24, *(volatile const uint32_t *)(uintptr_t)0x40086ff8u); /* I2S0 PSELID (0x5)         */
        put32(response + 28, *(volatile const uint32_t *)(uintptr_t)0x40088c00u); /* I2S2 CFG1  (0x1f0001)     */
        put32(response + 32, *(volatile const uint32_t *)(uintptr_t)0x40088e00u); /* I2S2 FIFOCFG (0x10005)    */
        put32(response + 36, *(volatile const uint32_t *)(uintptr_t)0x40088e04u); /* I2S2 FIFOSTAT             */
        put32(response + 40, *(volatile const uint32_t *)(uintptr_t)0x40082000u); /* DMA CTRL (bit0 ENABLE)    */
        put32(response + 44, *(volatile const uint32_t *)(uintptr_t)0x40082440u); /* DMA CH4_CFG  (0x20001)    */
        put32(response + 48, *(volatile const uint32_t *)(uintptr_t)0x400824b0u); /* DMA CH11_CFG (0x10001)    */
        put32(response + 52, *(volatile const uint32_t *)(uintptr_t)0x40082030u); /* DMA ACTIVE0 (expect 0)    */
        put32(response + 56, ((*mclkio & 1u)) | ((*mainclka & 7u) << 1) | ((*usb0sel & 7u) << 4)); /* CPU/USB/route invariant */
        put32(response + 60, *(volatile const uint32_t *)(uintptr_t)0x40082038u); /* DMA BUSY0 (expect 0)      */
        break;
    }
    case 31: { /* DMA0 ENGINE proof: a self-verifying memory-to-memory transfer.
                * Proves the DMA controller fetches a descriptor from OUR RAM and
                * advances -- the load-bearing question for the wireless-audio path --
                * WITHOUT the fragile I2S master-clock/FIFO handshake or any clock
                * routing (that is opcode 32). Uses channel 0 (unused by the audio
                * path), software-triggered, copying N 32-bit words src->dst inside our
                * own oversized .bss buffers. Success = dst matches src byte-for-byte
                * (far stronger than a status bit). Bounded + watchdog-fed; tears the
                * channel back down. Touches only DMA0 (clock/reset + registers); no
                * I2S/FCLKSEL/MCLKIO/IOCON/PLL, so CPU/USB are undisturbed. */
        const uint32_t N = 16u;
        for (uint32_t i = 0; i < 64u; ++i) { omni_dma_src[i] = 0xA5000000u ^ (i * 0x01010101u); omni_dma_dst[i] = 0u; }

        *(volatile uint32_t *)(uintptr_t)0x40000220u = 1u << 20;  /* AHBCLKCTRLSET0: DMA0 clock */
        *(volatile uint32_t *)(uintptr_t)0x40000140u = 1u << 20;  /* PRESETCTRLCLR0: DMA0 un-reset */

        /* XFERCFG: 32-bit width, src+dst increment by 1 width, count N, single-shot,
         * software-triggered, clear-trigger-on-done. Layout verified against the
         * recovered stock XFERCFG values. */
        uint32_t xfercfg = 0x1u | 0x4u | 0x8u | (2u << 8) | (1u << 12) | (1u << 14) | ((N - 1u) << 16);
        omni_dma_desc[0] = xfercfg;
        omni_dma_desc[1] = (uint32_t)(uintptr_t)&omni_dma_src[N - 1u]; /* srcEnd = last word */
        omni_dma_desc[2] = (uint32_t)(uintptr_t)&omni_dma_dst[N - 1u]; /* dstEnd = last word */
        omni_dma_desc[3] = 0u;                                        /* no link (no reload) */

        *(volatile uint32_t *)(uintptr_t)0x40082000u = 1u;                          /* CTRL: controller ENABLE */
        *(volatile uint32_t *)(uintptr_t)0x40082008u = (uint32_t)(uintptr_t)omni_dma_desc; /* SRAMBASE */
        *(volatile uint32_t *)(uintptr_t)0x40082400u = 0u;                          /* CH0_CFG: no HW trig, prio 0 */
        *(volatile uint32_t *)(uintptr_t)0x40082020u = 1u;                          /* ENABLESET0: enable ch0 */
        *(volatile uint32_t *)(uintptr_t)0x40082408u = xfercfg;                     /* CH0_XFERCFG: CFGVALID+SWTRIG -> run */

        uint32_t spins = 0u, active_seen = 0u;
        for (; spins < OMNI_CPU_GUARD_ITERATIONS(200000u); ++spins) {
            uint32_t active = *(volatile const uint32_t *)(uintptr_t)0x40082030u & 1u; /* ACTIVE0 ch0 */
            uint32_t busy   = *(volatile const uint32_t *)(uintptr_t)0x40082038u & 1u; /* BUSY0 ch0   */
            if (active) active_seen = 1u;
            if (!active && !busy && spins > 8u) break;
            watchdog_feed();
        }

        uint32_t match = 1u, first_bad = 0xffffffffu;
        for (uint32_t i = 0; i < N; ++i)
            if (omni_dma_dst[i] != omni_dma_src[i]) { match = 0u; if (first_bad == 0xffffffffu) first_bad = i; }

        uint32_t xfercfg_after = *(volatile const uint32_t *)(uintptr_t)0x40082408u;
        uint32_t intstat       = *(volatile const uint32_t *)(uintptr_t)0x40082004u; /* INTSTAT (bit1 = ERRINT) */
        uint32_t active_final  = *(volatile const uint32_t *)(uintptr_t)0x40082030u;
        uint32_t busy_final    = *(volatile const uint32_t *)(uintptr_t)0x40082038u;

        *(volatile uint32_t *)(uintptr_t)0x40082028u = 1u; /* ENABLECLR0: disable ch0 (teardown) */

        put32(response + 4,  match);                    /* 1 => dst == src for all N words (ENGINE PROVEN) */
        put32(response + 8,  first_bad);                /* first mismatched index, or 0xffffffff           */
        put32(response + 12, spins);                    /* poll iterations to completion                    */
        put32(response + 16, active_seen);              /* 1 => ACTIVE0 observed high at least once          */
        put32(response + 20, xfercfg_after);            /* channel XFERCFG after (XFERCOUNT should be 0)      */
        put32(response + 24, (xfercfg_after >> 16) & 0x3ffu); /* residual XFERCOUNT (expect 0)               */
        put32(response + 28, intstat);                  /* DMA INTSTAT (bit1 ERRINT must be 0)               */
        put32(response + 32, active_final);             /* ACTIVE0 final (expect 0)                          */
        put32(response + 36, busy_final);               /* BUSY0 final (expect 0)                            */
        put32(response + 40, omni_dma_src[0]);          /* sample first src word                             */
        put32(response + 44, omni_dma_dst[0]);          /* sample first dst word (should equal src[0])       */
        put32(response + 48, omni_dma_src[N - 1u]);     /* sample last src word                              */
        put32(response + 52, omni_dma_dst[N - 1u]);     /* sample last dst word (should equal src[N-1])      */
        put32(response + 56, (uint32_t)(uintptr_t)omni_dma_desc); /* descriptor table addr (in our RAM)      */
        put32(response + 60, N);                        /* word count                                        */
        break;
    }
    case 32: { /* Audio-clock ROUTE + I2S0 MASTER clock proof via TX-FIFO DRAIN.
                * With the clock tree at LOCAL_READY (opcode 29: PLL0 locked, MCLK from
                * PLL0), routes the FC0 function clock to PLL0 (FCCLKSEL0 = 1 =
                * PLL0_DIV_to_FLEXCOMM0) and ensures PLL0DIV runs (divide-by-1), then runs
                * I2S0 as a MASTER (CFG1 0xf0430, MSTSLVCFG=NormalMaster) with its TX FIFO
                * PRELOADED. Once MAINENABLE is set the master self-generates BCLK/WS off
                * PLL0 and shifts the preloaded words out, so the TX FIFO level DRAINS --
                * an unambiguous "the serial clock is running" proof, independent of any
                * input framing. The SDA pin is NOT muxed (IOCON untouched) so nothing
                * leaves the chip; no DMA; no DSP. Reads back PLL0DIV/FRG for diagnosis.
                * Restores FCCLKSEL0 and disables I2S0 on exit. Only FC0 + I2S0 touched,
                * so CPU/USB stay on the FRO and MCLKIO stays unrouted. */
        volatile const uint32_t *const pll0stat = (volatile const uint32_t *)(uintptr_t)0x40000584u;
        if ((*pll0stat & 1u) == 0u) { response[3] = 2; put32(response + 60, 0xdead0002u); break; } /* need PLL0 lock */

        *(volatile uint32_t *)(uintptr_t)0x40000224u = 1u << 11;  /* AHBCLKCTRLSET1: FC0 clock   */
        *(volatile uint32_t *)(uintptr_t)0x40000144u = 1u << 11;  /* PRESETCTRLCLR1: FC0 un-reset */

        volatile uint32_t *const pll0div = (volatile uint32_t *)(uintptr_t)0x400003c4u; /* PLL0CLKDIV */
        const uint32_t pll0div_before = *pll0div;
        *pll0div = 0u;                                          /* divide-by-1, HALT=0 -> PLL0_DIV runs */

        /* The per-flexcomm fractional-rate generator sits in the function-clock path
         * and its DIV field must be 0xFF to pass the clock (formula assumes DIV=255).
         * If FRG0 is left at reset it kills the FC0 function clock -> no BCLK. Set it
         * to pass-through (DIV=0xFF, MULT=0) before routing. */
        volatile uint32_t *const frg0 = (volatile uint32_t *)(uintptr_t)0x40000320u; /* FLEXFRG0CTRL */
        const uint32_t frg0_before = *frg0;
        *frg0 = 0xffu;                                          /* DIV=255, MULT=0 -> divide-by-1 */

        volatile uint32_t *const fc0sel = (volatile uint32_t *)(uintptr_t)0x400002b0u; /* FCCLKSEL0 */
        const uint32_t fc0sel_saved = *fc0sel;
        *fc0sel = 1u;                                            /* route FC0 function clock to PLL0_DIV */

        *(volatile uint32_t *)(uintptr_t)0x40086ff8u = 0x5u;     /* PSELID = I2S                    */
        *(volatile uint32_t *)(uintptr_t)0x40086c00u = 0xf0430u; /* CFG1 master, no MAINENABLE      */
        *(volatile uint32_t *)(uintptr_t)0x40086c04u = 0x3fu;    /* CFG2 (64-bit frame)             */
        *(volatile uint32_t *)(uintptr_t)0x40086c1cu = 0xfu;     /* DIV: PLL0/16 -> ~48 kHz frame   */
        *(volatile uint32_t *)(uintptr_t)0x40086e00u = 0x10001u; /* FIFOCFG: ENABLETX + flush       */

        /* Preload the TX FIFO (writes to FIFOWR fill it while the master is stopped). */
        uint32_t preload = 0u;
        for (uint32_t k = 0; k < 8u; ++k) {
            uint32_t fs = *(volatile const uint32_t *)(uintptr_t)0x40086e04u;
            if ((fs & 0x20u) == 0u) break;                      /* TXNOTFULL (bit5) clear -> full */
            *(volatile uint32_t *)(uintptr_t)0x40086e20u = 0xC5A50000u | k; /* FIFOWR */
            ++preload;
        }
        const uint32_t fifostat_before = *(volatile const uint32_t *)(uintptr_t)0x40086e04u;
        const uint32_t txlvl_before = (fifostat_before >> 8) & 0x1fu;

        *(volatile uint32_t *)(uintptr_t)0x40086c00u = 0xf0431u; /* CFG1 |= MAINENABLE -> master runs */

        uint32_t spins = 0u, txerr_seen = 0u, txempty_after = 0u, txlvl_min = 0x1fu;
        for (; spins < OMNI_CPU_GUARD_ITERATIONS(600000u); ++spins) {
            uint32_t fs = *(volatile const uint32_t *)(uintptr_t)0x40086e04u;
            uint32_t lvl = (fs >> 8) & 0x1fu;
            if (lvl < txlvl_min) txlvl_min = lvl;
            if (fs & 0x1u)  txerr_seen = 1u;    /* TXERR (underrun after drain -> clocked) */
            if (fs & 0x10u) txempty_after = 1u; /* TXEMPTY                                 */
            watchdog_feed();
            if (txerr_seen) break;
        }
        const uint32_t fifostat_after = *(volatile const uint32_t *)(uintptr_t)0x40086e04u;
        const uint32_t txlvl_after = (fifostat_after >> 8) & 0x1fu;

        /* Teardown: disable I2S0, flush TX FIFO, restore FCCLKSEL0 + PLL0DIV. */
        *(volatile uint32_t *)(uintptr_t)0x40086c00u = 0xf0430u; /* clear MAINENABLE */
        *(volatile uint32_t *)(uintptr_t)0x40086e00u = 0x10001u; /* flush TX FIFO    */
        *fc0sel = fc0sel_saved;                                  /* restore FC0 clock source */
        *frg0 = frg0_before;                                     /* restore FRG0 */
        *pll0div = pll0div_before;                               /* restore PLL0DIV */

        volatile const uint32_t *const mainclka = (volatile const uint32_t *)(uintptr_t)0x40000280u; /* MAINCLKSELA */
        volatile const uint32_t *const usb0sel  = (volatile const uint32_t *)(uintptr_t)0x400002a8u; /* USB0CLKSEL  */
        volatile const uint32_t *const mclkio   = (volatile const uint32_t *)(uintptr_t)0x40000420u; /* MCLKIO      */

        uint32_t drained = (txlvl_before > 0u && (txlvl_min < txlvl_before || txempty_after || txerr_seen)) ? 1u : 0u;
        put32(response + 4,  drained);        /* 1 => TX FIFO drained => MASTER CLOCK RAN */
        put32(response + 8,  txlvl_before);
        put32(response + 12, txlvl_after);
        put32(response + 16, txlvl_min);
        put32(response + 20, txerr_seen);
        put32(response + 24, txempty_after);
        put32(response + 28, spins);
        put32(response + 32, fifostat_before);
        put32(response + 36, fifostat_after);
        put32(response + 40, frg0_before);    /* FLEXFRG0CTRL before (reset value kills the FC0 clock) */
        put32(response + 44, fc0sel_saved);   /* FCCLKSEL0 before (expect 3)    */
        put32(response + 48, *pll0stat);      /* PLL0 still locked              */
        put32(response + 52, (*mainclka & 7u) | ((*fc0sel & 7u) << 4)); /* CPU on FRO12 (0) + FC0SEL restored */
        put32(response + 56, ((*mclkio & 1u)) | ((*usb0sel & 7u) << 1)); /* MCLKIO unrouted + USB0SEL */
        put32(response + 60, preload);        /* words preloaded into TX FIFO */
        break;
    }
    case 33: { /* I2S0 master clock with the SCK/WS pins MUXED (crosses "no pins").
                * Tests the opcode-32 hypothesis that the flexcomm gates the I2S master
                * clock generator on the SCK/WS pins being assigned to the I2S function.
                * Uses the exact stock pin mux recovered from FUN_000228d4: PIO1_4=FC0_SCK,
                * PIO1_6=FC0_WS, PIO1_5=FC0 RX-data, all IOCON value 0x4101 (FUNC1). With
                * the clock tree at LOCAL_READY, routes FC0 to PLL0_DIV (as opcode 32) and
                * enables I2S0 as a master RX; if the master now runs, its RX FIFO fills
                * (RXLVL rises / RXERR overflow). NOTE: SCK+WS are OUTPUTS, so the bit
                * clock is now present on PIO1_4/PIO1_6 (the pins that reach the DSP) --
                * this is the deliberate, user-approved step past "nothing leaves the chip".
                * Restores the IOCON pins to their prior values and FCCLKSEL0/PLL0DIV, and
                * disables I2S0, on exit. */
        volatile const uint32_t *const pll0stat = (volatile const uint32_t *)(uintptr_t)0x40000584u;
        if ((*pll0stat & 1u) == 0u) { response[3] = 2; put32(response + 60, 0xdead0002u); break; } /* need PLL0 lock */

        *(volatile uint32_t *)(uintptr_t)0x40000220u = 0x2000u;   /* AHBCLKCTRLSET0: IOCON clock  */
        *(volatile uint32_t *)(uintptr_t)0x40000224u = 1u << 11;  /* AHBCLKCTRLSET1: FC0 clock    */
        *(volatile uint32_t *)(uintptr_t)0x40000144u = 1u << 11;  /* PRESETCTRLCLR1: FC0 un-reset */

        volatile uint32_t *const pll0div = (volatile uint32_t *)(uintptr_t)0x400003c4u;
        const uint32_t pll0div_before = *pll0div;
        *pll0div = 0u;
        volatile uint32_t *const frg0 = (volatile uint32_t *)(uintptr_t)0x40000320u;
        const uint32_t frg0_before = *frg0;
        *frg0 = 0xffu;
        volatile uint32_t *const fc0sel = (volatile uint32_t *)(uintptr_t)0x400002b0u;
        const uint32_t fc0sel_saved = *fc0sel;
        *fc0sel = 1u;                                             /* FC0 <- PLL0_DIV */

        /* Mux the recovered stock I2S0 pins (FUN_000228d4): save prior IOCON, set FUNC1. */
        volatile uint32_t *const io_sck  = (volatile uint32_t *)(uintptr_t)0x40001090u; /* PIO1_4  FC0_SCK  */
        volatile uint32_t *const io_ws   = (volatile uint32_t *)(uintptr_t)0x40001098u; /* PIO1_6  FC0_WS   */
        volatile uint32_t *const io_data = (volatile uint32_t *)(uintptr_t)0x40001094u; /* PIO1_5  FC0 data */
        const uint32_t io_sck_before = *io_sck, io_ws_before = *io_ws, io_data_before = *io_data;
        *io_sck = 0x4101u; *io_ws = 0x4101u; *io_data = 0x4101u;

        *(volatile uint32_t *)(uintptr_t)0x40086ff8u = 0x5u;     /* PSELID = I2S            */
        *(volatile uint32_t *)(uintptr_t)0x40086c00u = 0xf0430u; /* CFG1 master, no enable  */
        *(volatile uint32_t *)(uintptr_t)0x40086c04u = 0x3fu;    /* CFG2 (64-bit frame)     */
        *(volatile uint32_t *)(uintptr_t)0x40086c1cu = 0xfu;     /* DIV: PLL0/16 -> ~48 kHz */
        *(volatile uint32_t *)(uintptr_t)0x40086e00u = 0x20002u; /* FIFOCFG: ENABLERX+flush */

        const uint32_t fifostat_before = *(volatile const uint32_t *)(uintptr_t)0x40086e04u;
        const uint32_t rxlvl_before = (fifostat_before >> 16) & 0x1fu;

        *(volatile uint32_t *)(uintptr_t)0x40086c00u = 0xf0431u; /* CFG1 |= MAINENABLE */

        uint32_t spins = 0u, rxerr_seen = 0u, rxne_seen = 0u, rxlvl_max = 0u;
        for (; spins < OMNI_CPU_GUARD_ITERATIONS(600000u); ++spins) {
            uint32_t fs = *(volatile const uint32_t *)(uintptr_t)0x40086e04u;
            uint32_t lvl = (fs >> 16) & 0x1fu;
            if (lvl > rxlvl_max) rxlvl_max = lvl;
            if (fs & 0x2u)  rxerr_seen = 1u;   /* RXERR overflow -> clocking */
            if (fs & 0x40u) rxne_seen = 1u;    /* RXNOTEMPTY */
            watchdog_feed();
            if (rxerr_seen) break;
        }
        const uint32_t fifostat_after = *(volatile const uint32_t *)(uintptr_t)0x40086e04u;
        const uint32_t rxlvl_after = (fifostat_after >> 16) & 0x1fu;

        /* Teardown: disable I2S0, flush, restore IOCON pins + FC0 clock source + PLL0DIV. */
        *(volatile uint32_t *)(uintptr_t)0x40086c00u = 0xf0430u;
        *(volatile uint32_t *)(uintptr_t)0x40086e00u = 0x20002u;
        *io_sck = io_sck_before; *io_ws = io_ws_before; *io_data = io_data_before;
        *fc0sel = fc0sel_saved;
        *frg0 = frg0_before;
        *pll0div = pll0div_before;

        volatile const uint32_t *const mainclka = (volatile const uint32_t *)(uintptr_t)0x40000280u;
        volatile const uint32_t *const usb0sel  = (volatile const uint32_t *)(uintptr_t)0x400002a8u;
        volatile const uint32_t *const mclkio   = (volatile const uint32_t *)(uintptr_t)0x40000420u;

        put32(response + 4,  (rxlvl_max > 0u || rxerr_seen || rxne_seen) ? 1u : 0u); /* MASTER CLOCK RAN */
        put32(response + 8,  rxlvl_before);
        put32(response + 12, rxlvl_after);
        put32(response + 16, rxlvl_max);
        put32(response + 20, rxerr_seen);
        put32(response + 24, rxne_seen);
        put32(response + 28, spins);
        put32(response + 32, fifostat_before);
        put32(response + 36, fifostat_after);
        put32(response + 40, io_sck_before);   /* PIO1_4 IOCON before mux */
        put32(response + 44, fc0sel_saved);
        put32(response + 48, *pll0stat);
        put32(response + 52, (*mainclka & 7u) | ((*fc0sel & 7u) << 4)); /* CPU on FRO12 + FC0SEL restored */
        put32(response + 56, ((*mclkio & 1u)) | ((*usb0sel & 7u) << 1));
        put32(response + 60, *io_sck);         /* PIO1_4 IOCON after restore (== before) */
        break;
    }
    case 34: { /* PERIPHERAL-PACED DMA RING on the I2S0 MASTER RX path (ch4).
                * The integration proof: does an auto-reload DMA ring advance when PACED
                * by the I2S FIFO's own DMA request (not software-triggered like opcode 31)?
                * Uses I2S0 as MASTER (self-clocks off PLL0, proven opcode 33) so it needs
                * NO FC0->FC2 clock share (that is only for the FC2 slave TX path). The
                * master generates BCLK/WS, its RX FIFO fills each frame and raises a DMA
                * request; DMA0 ch4 (PERIPHREQEN) drains the FIFORD into OUR ring buffer
                * and, on RELOAD, restarts from the same self-linking descriptor -> a
                * continuous ring. Advance is proven by INTA completions per 1 ms block +
                * XFERCOUNT reloading, with no ERRINT. Writes only our RAM + FC0/I2S0/DMA0;
                * muxes SCK/WS/RXdata (bit clock reaches the pin, user-approved). All
                * IOCON/clock/DMA writes restored on exit; MCU2/DSP/bootloader untouched. */
        volatile const uint32_t *const pll0stat = (volatile const uint32_t *)(uintptr_t)0x40000584u;
        if ((*pll0stat & 1u) == 0u) { response[3] = 2; put32(response + 60, 0xdead0002u); break; } /* need PLL0 lock */

        *(volatile uint32_t *)(uintptr_t)0x40000220u = 0x2000u | (1u << 20) | (1u << 11); /* AHBCLKCTRLSET0: IOCON + DMA0 + INPUTMUX */
        *(volatile uint32_t *)(uintptr_t)0x40000224u = 1u << 11;  /* AHBCLKCTRLSET1: FC0 clock    */
        *(volatile uint32_t *)(uintptr_t)0x40000140u = 1u << 20;  /* PRESETCTRLCLR0: DMA0 un-reset */
        *(volatile uint32_t *)(uintptr_t)0x40000144u = 1u << 11;  /* PRESETCTRLCLR1: FC0 un-reset  */

        volatile uint32_t *const pll0div = (volatile uint32_t *)(uintptr_t)0x400003c4u;
        const uint32_t pll0div_before = *pll0div; *pll0div = 0u;
        volatile uint32_t *const frg0 = (volatile uint32_t *)(uintptr_t)0x40000320u;
        const uint32_t frg0_before = *frg0; *frg0 = 0xffu;
        volatile uint32_t *const fc0sel = (volatile uint32_t *)(uintptr_t)0x400002b0u;
        const uint32_t fc0sel_saved = *fc0sel; *fc0sel = 1u; /* FC0 <- PLL0_DIV */

        volatile uint32_t *const io_sck  = (volatile uint32_t *)(uintptr_t)0x40001090u;
        volatile uint32_t *const io_ws   = (volatile uint32_t *)(uintptr_t)0x40001098u;
        volatile uint32_t *const io_data = (volatile uint32_t *)(uintptr_t)0x40001094u;
        const uint32_t io_sck_before = *io_sck, io_ws_before = *io_ws, io_data_before = *io_data;
        *io_sck = 0x4101u; *io_ws = 0x4101u; *io_data = 0x4101u;

        /* DMA0 controller + our descriptor table. */
        *(volatile uint32_t *)(uintptr_t)0x40082000u = 1u;                          /* CTRL ENABLE */
        *(volatile uint32_t *)(uintptr_t)0x40082008u = (uint32_t)(uintptr_t)omni_dma_desc; /* SRAMBASE */

        /* Head (== reload) descriptor for ch4 at SRAMBASE + 4*0x10 (word 16), self-linking. */
        const uint32_t rx_count = 48u;                                              /* 48 samples / 1 ms @ 48 kHz */
        const uint32_t rx_xfercfg = 0x2f4113u; /* CFGVALID|RELOAD|SETINTA|WIDTH16|SRCINC0|DSTINC1|XFERCOUNT(47) */
        for (uint32_t i = 0; i < 128u; ++i) omni_dma_dst[i] = 0u;                   /* clear the ring buffer */
        volatile uint32_t *const d = &omni_dma_desc[16];                            /* ch4 descriptor slot */
        d[0] = rx_xfercfg;                                                          /* reload xfercfg          */
        d[1] = 0x40086e30u;                                                         /* srcEnd = I2S0 FIFORD (SRCINC0 -> fixed) */
        d[2] = (uint32_t)(uintptr_t)&omni_dma_dst[0] + 2u * (rx_count - 1u);        /* dstEnd = buf + width*(count-1) */
        d[3] = (uint32_t)(uintptr_t)&omni_dma_desc[16];                             /* next = self (continuous ring)  */

        /* I2S0 master RX with FIFO DMA request. */
        *(volatile uint32_t *)(uintptr_t)0x40086ff8u = 0x5u;     /* PSELID = I2S */
        *(volatile uint32_t *)(uintptr_t)0x40086c00u = 0xf0430u; /* CFG1 master, no enable */
        *(volatile uint32_t *)(uintptr_t)0x40086c04u = 0x3fu;    /* CFG2 */
        *(volatile uint32_t *)(uintptr_t)0x40086c1cu = 0xfu;     /* DIV: PLL0/16 -> ~48 kHz */
        *(volatile uint32_t *)(uintptr_t)0x40086e00u = 0x22002u; /* FIFOCFG: ENABLERX + DMARX(bit13) + flush */
        *(volatile uint32_t *)(uintptr_t)0x40086e08u = 0x40002u; /* FIFOTRIG */

        /* Connect the Flexcomm0-RX DMA request to ch4 in the INPUTMUX. On LPC55 the
         * peripheral DMA request is gated HERE (DMA0_REQ_ENA), separate from
         * FIFOCFG.DMARX and the channel's PERIPHREQEN. Without it the RX FIFO fills and
         * overflows but the request never reaches the armed channel (observed: RXERR=1,
         * RXLVL=8, XFERCOUNT stuck). Channel 4 = Flexcomm0 RX per the SDK request map. */
        INPUTMUX->DMA0_REQ_ENA_SET = (1u << 4);

        /* Arm ch4: priority+PERIPHREQEN, enable, write XFERCFG (CFGVALID+RELOAD; paced by the FIFO request). */
        *(volatile uint32_t *)(uintptr_t)0x40082440u = 0x20001u; /* CH4_CFG: PERIPHREQEN + prio2 */
        *(volatile uint32_t *)(uintptr_t)0x40082058u = 0x10u;    /* INTA0: clear any stale ch4 flag */
        *(volatile uint32_t *)(uintptr_t)0x40082020u = 0x10u;    /* ENABLESET0: enable ch4 */
        *(volatile uint32_t *)(uintptr_t)0x40082448u = rx_xfercfg; /* CH4_XFERCFG: arm (CFGVALID+RELOAD) */
        *(volatile uint32_t *)(uintptr_t)0x40082070u = 0x10u;    /* SETTRIG0: trigger ch4 once -> starts the descriptor; the FIFO DMA request then paces each transfer and RELOAD sustains the ring (UM 21.5.1: a channel must be triggered before requests initiate transfers) */

        *(volatile uint32_t *)(uintptr_t)0x40086c00u = 0xf0431u; /* I2S0 MAINENABLE -> master runs, FIFO->DMA */

        /* Observe advance over a bounded window: count INTA completions (per block),
         * watch XFERCOUNT move, catch ERRINT. */
        uint32_t spins = 0u, inta_blocks = 0u, reloads = 0u, err_seen = 0u, active_seen = 0u;
        uint32_t xc_prev = (*(volatile const uint32_t *)(uintptr_t)0x40082448u >> 16) & 0x3ffu;
        for (; spins < OMNI_CPU_GUARD_ITERATIONS(400000u); ++spins) {
            uint32_t inta = *(volatile const uint32_t *)(uintptr_t)0x40082058u;
            if (inta & 0x10u) { inta_blocks++; *(volatile uint32_t *)(uintptr_t)0x40082058u = 0x10u; } /* count + clear INTA */
            uint32_t xc = (*(volatile const uint32_t *)(uintptr_t)0x40082448u >> 16) & 0x3ffu;
            if (xc > xc_prev) reloads++;   /* count-up jump = the ring reloaded a block (primary advance signal) */
            xc_prev = xc;
            if (*(volatile const uint32_t *)(uintptr_t)0x40082004u & 0x2u) err_seen = 1u; /* INTSTAT ACTIVEERRINT */
            if (*(volatile const uint32_t *)(uintptr_t)0x40082030u & 0x10u) active_seen = 1u; /* ACTIVE0 ch4 */
            watchdog_feed();
            if (inta_blocks >= 20u || reloads >= 20u) break; /* enough evidence of sustained advance */
        }

        uint32_t active_final = *(volatile const uint32_t *)(uintptr_t)0x40082030u;
        uint32_t busy_final   = *(volatile const uint32_t *)(uintptr_t)0x40082038u;
        uint32_t xfercfg_final= *(volatile const uint32_t *)(uintptr_t)0x40082448u;
        uint32_t errint       = *(volatile const uint32_t *)(uintptr_t)0x40082040u; /* ERRINT0 */
        uint32_t i2s_fifostat = *(volatile const uint32_t *)(uintptr_t)0x40086e04u; /* I2S0 FIFOSTAT: RXLVL/RXERR */

        /* Teardown: stop I2S0, disable ch4, clear DMARX, restore pins + clock source. */
        *(volatile uint32_t *)(uintptr_t)0x40086c00u = 0xf0430u; /* MAINENABLE off */
        *(volatile uint32_t *)(uintptr_t)0x40082028u = 0x10u;    /* ENABLECLR0 ch4 */
        INPUTMUX->DMA0_REQ_ENA_CLR = (1u << 4);                  /* disconnect the FC0-RX request */
        *(volatile uint32_t *)(uintptr_t)0x40086e00u = 0x22002u; /* keep flush; DMARX cleared implicitly by teardown below */
        *(volatile uint32_t *)(uintptr_t)0x40086e00u = 0x20002u; /* FIFOCFG: drop DMARX (leave ENABLERX+flush) */
        *io_sck = io_sck_before; *io_ws = io_ws_before; *io_data = io_data_before;
        *fc0sel = fc0sel_saved; *frg0 = frg0_before; *pll0div = pll0div_before;

        put32(response + 4,  (inta_blocks > 0u || reloads > 0u) ? 1u : 0u); /* RING ADVANCED (peripheral-paced) */
        put32(response + 8,  inta_blocks);        /* INTA completions counted (per 1 ms block)  */
        put32(response + 12, reloads);            /* XFERCOUNT reload events (primary advance signal) */
        put32(response + 16, spins);
        put32(response + 20, active_seen);        /* ACTIVE0 ch4 seen high                        */
        put32(response + 24, err_seen);           /* INTSTAT ACTIVEERRINT seen                    */
        put32(response + 28, errint);             /* ERRINT0 (bit4 = ch4 error; expect 0)         */
        put32(response + 32, active_final);       /* ACTIVE0 final                                */
        put32(response + 36, busy_final);         /* BUSY0 final                                  */
        put32(response + 40, xfercfg_final);      /* CH4_XFERCFG final (CFGVALID stays set on a reload ring) */
        put32(response + 44, (xfercfg_final >> 16) & 0x3ffu); /* final XFERCOUNT                   */
        put32(response + 48, omni_dma_dst[0]);    /* first ring sample captured                    */
        put32(response + 52, i2s_fifostat);       /* I2S0 FIFOSTAT: RXLVL[20:16], RXERR bit1, RXFULL bit7 */
        put32(response + 56, (uint32_t)(uintptr_t)omni_dma_desc); /* SRAMBASE (our RAM)             */
        put32(response + 60, rx_count);
        break;
    }
    case 35: { /* DSP audio-accept handshake (OPEN-LOOP) via the shippable audio_mode
                * sequencer. Wires the real MCU1<->DSP UART3 (FC3, via control_uart) to
                * audio_mode's write/tx_status, with NO-OP pins/configure -- so it emits
                * the four byte-exact stock frames (STOP BD065101 0400, GATE BD068801
                * 0000, RESUME BD058801 00, RATE BD065101 01 <rate>) in the stock order
                * and timing (50ms initial, 20ms interframe, 75ms quiet/resume) and
                * confirms each drained. NO DSP reply is awaited (open-loop; there is no
                * accept-ACK -- the only DSP reply on the wire is the unrelated E1
                * version frame). Non-destructive: replays byte-exact stock frames on the
                * existing DSP UART; MCU2/DSP config untouched, no I2S/DMA/clock touched.
                * SysTick VAL timebase (as opcode 29). */
        const uint32_t systick_period = SysTick->LOAD + 1u;
        if ((SysTick->CTRL & SysTick_CTRL_ENABLE_Msk) == 0u) { response[3] = 2; put32(response + 60, 0xdead0000u); break; }

        if (!omni_control_uart_start(3, &omni_am_io)) { response[3] = 2; put32(response + 60, 0xdead0003u); break; } /* UART3 start failed */

        omni_audio_mode_ops_t ops;
        ops.context = 0; ops.write = am_write; ops.tx_status = am_tx_status;
        ops.pins = am_pins_noop; ops.configure_begin = am_cfg_begin_noop; ops.configure_poll = am_cfg_poll_noop;
        omni_audio_mode_t mode;
        uint32_t init_ok = omni_audio_mode_init(&mode, &ops) ? 1u : 0u;
        uint32_t begin_ok = 0u;
        if (init_ok) begin_ok = omni_audio_mode_begin(&mode, 48000u, 0u) ? 1u : 0u;

        uint32_t now_ms = 0u, rem = 0u, prev = SysTick->VAL, iters = 0u;
        if (begin_ok) {
            for (; iters < OMNI_CPU_GUARD_ITERATIONS(3000000u); ++iters) {
                if (mode.state >= OMNI_AUDIO_MODE_LOCAL_COMPLETE) break; /* LOCAL_COMPLETE / FAILED / CANCELED */
                uint32_t cur = SysTick->VAL;
                uint32_t d = (prev >= cur) ? (prev - cur) : (prev + systick_period - cur);
                prev = cur; rem += d; while (rem >= systick_period) { rem -= systick_period; ++now_ms; }
                omni_audio_mode_poll(&mode, now_ms);
                watchdog_feed();
            }
        }
        uint32_t stats[6]; omni_control_uart_stats(3, stats); /* transmitted, received, ... */
        omni_control_uart_stop(3);

        put32(response + 4,  (mode.state == OMNI_AUDIO_MODE_LOCAL_COMPLETE) ? 1u : 0u); /* handshake emitted OK */
        put32(response + 8,  (uint32_t)mode.state);
        put32(response + 12, (uint32_t)mode.error);
        put32(response + 16, mode.transmitted_bytes);  /* expect 23 = 6+6+5+6 */
        put32(response + 20, iters);
        put32(response + 24, now_ms);                  /* elapsed ms (expect ~260+) */
        put32(response + 28, (begin_ok << 1) | init_ok);
        put32(response + 32, stats[0]);                /* UART TX bytes  */
        put32(response + 36, stats[1]);                /* UART RX bytes (any DSP chatter -- open-loop, not an ACK) */
        put32(response + 40, stats[2]);
        put32(response + 44, stats[3]);
        put32(response + 48, (uint32_t)mode.pins_isolated);
        put32(response + 60, 0u);
        break;
    }
    case 36: { /* TX/PLAYBACK peripheral-paced DMA ring: I2S0 master -> FC0->FC2 clock
                * SHARE -> I2S2 slave TX -> DMA0 ch11 auto-reload ring (our RAM). The
                * playback data path. I2S2/FC2 is a SLAVE (no own BCLK/WS); it gets them
                * internally from the I2S0/FC0 master via the SYSCTL shared-signal-set
                * (0x40023000), recovered + quadruple-verified from stock. If the share
                * works, the slave TX FIFO drains off the shared clock and ch11's ring
                * advances (INTA/XFERCOUNT reloads). Reuses the proven op34 arm pattern
                * (INPUTMUX DMA0_REQ_ENA + SETTRIG). All writes reversible; our RAM +
                * FC0/FC2/I2S/DMA0-ch11/SYSCTL only; MCU2/DSP/bootloader untouched. */
        volatile const uint32_t *const pll0stat = (volatile const uint32_t *)(uintptr_t)0x40000584u;
        if ((*pll0stat & 1u) == 0u) { response[3] = 2; put32(response + 60, 0xdead0002u); break; } /* need PLL0 lock */

        *(volatile uint32_t *)(uintptr_t)0x40000220u = 0x2000u | (1u << 20) | (1u << 11); /* SET0: IOCON+DMA0+INPUTMUX */
        *(volatile uint32_t *)(uintptr_t)0x40000224u = (1u << 11) | (1u << 13);           /* SET1: FC0 + FC2 */
        *(volatile uint32_t *)(uintptr_t)0x40000228u = 0x8000u;                            /* SET2: SYSCTL clock */
        *(volatile uint32_t *)(uintptr_t)0x40000140u = 1u << 20;                           /* CLR0: DMA0 un-reset */
        *(volatile uint32_t *)(uintptr_t)0x40000144u = (1u << 11) | (1u << 13);            /* CLR1: FC0 + FC2 un-reset */

        volatile uint32_t *const pll0div = (volatile uint32_t *)(uintptr_t)0x400003c4u;
        const uint32_t pll0div_before = *pll0div; *pll0div = 0u;
        volatile uint32_t *const frg0 = (volatile uint32_t *)(uintptr_t)0x40000320u;
        volatile uint32_t *const frg2 = (volatile uint32_t *)(uintptr_t)0x40000328u;
        const uint32_t frg0_before = *frg0, frg2_before = *frg2; *frg0 = 0xffu; *frg2 = 0xffu;
        volatile uint32_t *const fc0sel = (volatile uint32_t *)(uintptr_t)0x400002b0u;
        volatile uint32_t *const fc2sel = (volatile uint32_t *)(uintptr_t)0x400002b8u;
        const uint32_t fc0sel_saved = *fc0sel, fc2sel_saved = *fc2sel; *fc0sel = 1u; *fc2sel = 1u; /* FC0/FC2 <- PLL0 */

        /* FC0->FC2 I2S clock share (SYSCTL 0x40023000), stock-verified minimal form. */
        volatile uint32_t *const lck = (volatile uint32_t *)(uintptr_t)0x40023000u;
        volatile uint32_t *const shared0 = (volatile uint32_t *)(uintptr_t)0x40023080u; /* SHAREDCTRLSET0 */
        volatile uint32_t *const fc0ctrl = (volatile uint32_t *)(uintptr_t)0x40023040u; /* FC0CTRLSEL */
        volatile uint32_t *const fc2ctrl = (volatile uint32_t *)(uintptr_t)0x40023048u; /* FC2CTRLSEL */
        *lck = 0u; *shared0 = 0u; *fc2ctrl = 0x101u; *fc0ctrl = 0x101u; *lck = 1u; /* SCK+WS: FC0 source, FC0+FC2 consume set0 */

        /* Mux the master SCK/WS (outputs) + I2S2 TX data (output). */
        volatile uint32_t *const io_sck    = (volatile uint32_t *)(uintptr_t)0x40001090u; /* PIO1_4  FC0_SCK  */
        volatile uint32_t *const io_ws     = (volatile uint32_t *)(uintptr_t)0x40001098u; /* PIO1_6  FC0_WS   */
        volatile uint32_t *const io_txdata = (volatile uint32_t *)(uintptr_t)0x400010e0u; /* PIO1_24 FC2 data */
        const uint32_t io_sck_before = *io_sck, io_ws_before = *io_ws, io_txdata_before = *io_txdata;
        *io_sck = 0x4101u; *io_ws = 0x4101u; *io_txdata = 0x4101u;

        /* I2S0 = clock MASTER (as opcode 33/34). */
        *(volatile uint32_t *)(uintptr_t)0x40086ff8u = 0x5u;
        *(volatile uint32_t *)(uintptr_t)0x40086c00u = 0xf0430u; /* CFG1 master, no enable */
        *(volatile uint32_t *)(uintptr_t)0x40086c04u = 0x3fu;
        *(volatile uint32_t *)(uintptr_t)0x40086c1cu = 0xfu;     /* DIV: PLL0/16 */
        *(volatile uint32_t *)(uintptr_t)0x40086e00u = 0x20002u; /* RX FIFO on (unused; keeps the master clocking) */

        /* I2S2 = slave TX (recovered config). */
        *(volatile uint32_t *)(uintptr_t)0x40088ff8u = 0x4u;
        *(volatile uint32_t *)(uintptr_t)0x40088c00u = 0x1f0000u; /* CFG1 slave, no enable */
        *(volatile uint32_t *)(uintptr_t)0x40088c04u = 0x3fu;
        *(volatile uint32_t *)(uintptr_t)0x40088c1cu = 0x0u;      /* DIV 0 (slave) */
        *(volatile uint32_t *)(uintptr_t)0x40088e00u = 0x10001u;  /* FIFOCFG: ENABLETX + flush */
        *(volatile uint32_t *)(uintptr_t)0x40088e08u = 0x401u;    /* FIFOTRIG */

        /* DMA0 controller + our TX ring (buffer = omni_dma_dst, primed with a test tone). */
        *(volatile uint32_t *)(uintptr_t)0x40082000u = 1u;                                 /* CTRL ENABLE */
        *(volatile uint32_t *)(uintptr_t)0x40082008u = (uint32_t)(uintptr_t)omni_dma_desc; /* SRAMBASE */
        const uint32_t tx_count = 96u;                                                     /* 48 stereo 32-bit frames / 1 ms @ 48 kHz */
        const uint32_t tx_xfercfg = 0x5f1213u; /* CFGVALID|RELOAD|SETINTA|WIDTH32|SRCINC1|DSTINC0|XFERCOUNT95 */
        for (uint32_t i = 0; i < 128u; ++i) omni_dma_dst[i] = 0x7fff8000u ^ (i * 0x00010001u); /* non-zero L/R test pattern */
        volatile uint32_t *const dt = &omni_dma_desc[44];                                  /* ch11 descriptor slot (11*0x10) */
        dt[0] = tx_xfercfg;
        dt[1] = (uint32_t)(uintptr_t)&omni_dma_dst[0] + 4u * (tx_count - 1u);              /* srcEnd = buf + width*(count-1) */
        dt[2] = 0x40088e20u;                                                               /* dstEnd = I2S2 FIFOWR (DSTINC0 -> fixed) */
        dt[3] = (uint32_t)(uintptr_t)&omni_dma_desc[44];                                   /* next = self */

        INPUTMUX->DMA0_REQ_ENA_SET = (1u << 11);                 /* connect Flexcomm2-TX request to ch11 */
        *(volatile uint32_t *)(uintptr_t)0x400824b0u = 0x10001u; /* CH11_CFG: PERIPHREQEN + prio1 */
        *(volatile uint32_t *)(uintptr_t)0x40082058u = 0x800u;   /* INTA0: clear stale ch11 flag */
        *(volatile uint32_t *)(uintptr_t)0x40082020u = 0x800u;   /* ENABLESET0: enable ch11 */
        *(volatile uint32_t *)(uintptr_t)0x400824b8u = tx_xfercfg; /* CH11_XFERCFG: arm */

        *(volatile uint32_t *)(uintptr_t)0x40086c00u = 0xf0431u; /* I2S0 MASTER enable -> BCLK/WS (shared to FC2) */
        *(volatile uint32_t *)(uintptr_t)0x40088e00u = 0x11001u; /* I2S2 FIFOCFG |= DMATX(bit12) -> FIFO requests DMA */
        *(volatile uint32_t *)(uintptr_t)0x40082070u = 0x800u;   /* SETTRIG0 ch11 -> DMA pre-fills the TX FIFO */
        *(volatile uint32_t *)(uintptr_t)0x40088c00u = 0x1f0001u;/* I2S2 SLAVE enable -> shifts out, FIFO drains, ring refills */

        uint32_t spins = 0u, inta_blocks = 0u, reloads = 0u, err_seen = 0u, active_seen = 0u;
        uint32_t xc_prev = (*(volatile const uint32_t *)(uintptr_t)0x400824b8u >> 16) & 0x3ffu;
        for (; spins < OMNI_CPU_GUARD_ITERATIONS(400000u); ++spins) {
            uint32_t inta = *(volatile const uint32_t *)(uintptr_t)0x40082058u;
            if (inta & 0x800u) { inta_blocks++; *(volatile uint32_t *)(uintptr_t)0x40082058u = 0x800u; }
            uint32_t xc = (*(volatile const uint32_t *)(uintptr_t)0x400824b8u >> 16) & 0x3ffu;
            if (xc > xc_prev) reloads++;
            xc_prev = xc;
            if (*(volatile const uint32_t *)(uintptr_t)0x40082004u & 0x2u) err_seen = 1u;
            if (*(volatile const uint32_t *)(uintptr_t)0x40082030u & 0x800u) active_seen = 1u; /* ACTIVE0 ch11 */
            watchdog_feed();
            if (inta_blocks >= 20u || reloads >= 20u) break;
        }

        uint32_t active_final = *(volatile const uint32_t *)(uintptr_t)0x40082030u;
        uint32_t xfercfg_final= *(volatile const uint32_t *)(uintptr_t)0x400824b8u;
        uint32_t errint       = *(volatile const uint32_t *)(uintptr_t)0x40082040u;
        uint32_t i2s2_fifostat= *(volatile const uint32_t *)(uintptr_t)0x40088e04u; /* TXLVL[12:8], TXERR bit0 */
        uint32_t fc2ctrl_rb   = *fc2ctrl, shared0_rb = *shared0;

        /* Teardown: disable I2S, disable ch11, restore share + pins + clock selects. */
        *(volatile uint32_t *)(uintptr_t)0x40088c00u = 0x1f0000u; /* I2S2 MAINENABLE off */
        *(volatile uint32_t *)(uintptr_t)0x40086c00u = 0xf0430u;  /* I2S0 MAINENABLE off */
        *(volatile uint32_t *)(uintptr_t)0x40082028u = 0x800u;    /* ENABLECLR0 ch11 */
        INPUTMUX->DMA0_REQ_ENA_CLR = (1u << 11);
        *(volatile uint32_t *)(uintptr_t)0x40088e00u = 0x10001u;  /* drop DMATX */
        *lck = 0u; *fc0ctrl = 0u; *fc2ctrl = 0u; *lck = 1u;       /* clock share -> dedicated */
        *io_sck = io_sck_before; *io_ws = io_ws_before; *io_txdata = io_txdata_before;
        *fc0sel = fc0sel_saved; *fc2sel = fc2sel_saved; *frg0 = frg0_before; *frg2 = frg2_before; *pll0div = pll0div_before;

        put32(response + 4,  (inta_blocks > 0u || reloads > 0u) ? 1u : 0u); /* TX RING ADVANCED off shared clock */
        put32(response + 8,  inta_blocks);
        put32(response + 12, reloads);
        put32(response + 16, spins);
        put32(response + 20, active_seen);
        put32(response + 24, err_seen);
        put32(response + 28, errint);
        put32(response + 32, active_final);
        put32(response + 36, i2s2_fifostat);      /* TXLVL/TXERR: drain => shared clock delivered */
        put32(response + 40, xfercfg_final);
        put32(response + 44, (xfercfg_final >> 16) & 0x3ffu);
        put32(response + 48, fc2ctrl_rb);         /* FC2CTRLSEL readback (expect 0x101)  */
        put32(response + 52, shared0_rb);         /* SHAREDCTRLSET0 readback (expect 0)   */
        put32(response + 56, (uint32_t)(uintptr_t)omni_dma_desc);
        put32(response + 60, tx_count);
        break;
    }
    case 37: { /* FULL PLAYBACK sequence: the shippable audio_mode sequencer with REAL
                * pins + TX-ring configure. Runs the exact stock flow end-to-end: DSP
                * STOP/GATE (UART3) -> isolate I2S pins -> configure+arm the I2S/DMA TX
                * ring (clock share + master/slave + ch11) -> route pins (master clocks,
                * ring streams to the DSP bus) -> DSP RESUME/RATE. After LOCAL_COMPLETE
                * the TX ring is streaming; we measure ch11 advance to prove it. AUDIBLE
                * output is hardware-gated (needs the live DSP + paired headset); this
                * proves the full MCU1-side sequence runs and the ring streams. All
                * writes reversible; MCU2/DSP config/bootloader untouched. */
        am_src_usb = 0u;                        /* proof uses the built-in tone */
        volatile const uint32_t *const pll0stat = (volatile const uint32_t *)(uintptr_t)0x40000584u;
        if ((*pll0stat & 1u) == 0u) { response[3] = 2; put32(response + 60, 0xdead0002u); break; } /* need PLL0 lock */
        const uint32_t systick_period = SysTick->LOAD + 1u;
        if ((SysTick->CTRL & SysTick_CTRL_ENABLE_Msk) == 0u) { response[3] = 2; put32(response + 60, 0xdead0000u); break; }
        if (!omni_control_uart_start(3, &omni_am_io)) { response[3] = 2; put32(response + 60, 0xdead0003u); break; }

        omni_audio_mode_ops_t ops;
        ops.context = 0; ops.write = am_write; ops.tx_status = am_tx_status;
        ops.pins = am_pins_route; ops.configure_begin = am_txring_begin; ops.configure_poll = am_txring_poll;
        omni_audio_mode_t mode;
        uint32_t init_ok = omni_audio_mode_init(&mode, &ops) ? 1u : 0u;
        uint32_t begin_ok = init_ok && omni_audio_mode_begin(&mode, 48000u, 0u) ? 1u : 0u;

        uint32_t now_ms = 0u, rem = 0u, prev = SysTick->VAL, iters = 0u;
        if (begin_ok) {
            for (; iters < OMNI_CPU_GUARD_ITERATIONS(3000000u); ++iters) {
                if (mode.state >= OMNI_AUDIO_MODE_LOCAL_COMPLETE) break;
                uint32_t cur = SysTick->VAL;
                uint32_t d = (prev >= cur) ? (prev - cur) : (prev + systick_period - cur);
                prev = cur; rem += d; while (rem >= systick_period) { rem -= systick_period; ++now_ms; }
                omni_audio_mode_poll(&mode, now_ms);
                watchdog_feed();
            }
        }

        /* The ring has been streaming since CONNECT (pins routed). Measure its advance. */
        uint32_t rspins = 0u, inta_blocks = 0u, reloads = 0u;
        uint32_t xc_prev = (*(volatile const uint32_t *)(uintptr_t)0x400824b8u >> 16) & 0x3ffu;
        for (; rspins < OMNI_CPU_GUARD_ITERATIONS(200000u); ++rspins) {
            uint32_t inta = *(volatile const uint32_t *)(uintptr_t)0x40082058u;
            if (inta & 0x800u) { inta_blocks++; *(volatile uint32_t *)(uintptr_t)0x40082058u = 0x800u; }
            uint32_t xc = (*(volatile const uint32_t *)(uintptr_t)0x400824b8u >> 16) & 0x3ffu;
            if (xc > xc_prev) reloads++;
            xc_prev = xc;
            watchdog_feed();
            if (inta_blocks >= 20u || reloads >= 20u) break;
        }
        uint32_t i2s2_fifostat = *(volatile const uint32_t *)(uintptr_t)0x40088e04u;
        uint32_t errint        = *(volatile const uint32_t *)(uintptr_t)0x40082040u;
        uint32_t active_ch11   = *(volatile const uint32_t *)(uintptr_t)0x40082030u;
        uint32_t fc2ctrl_rb    = *(volatile const uint32_t *)(uintptr_t)0x40023048u;

        uint32_t stats[6]; omni_control_uart_stats(3, stats);
        am_txring_teardown();
        omni_control_uart_stop(3);

        put32(response + 4,  (mode.state == OMNI_AUDIO_MODE_LOCAL_COMPLETE) ? 1u : 0u); /* full sequence completed */
        put32(response + 8,  (uint32_t)mode.state);
        put32(response + 12, (uint32_t)mode.error);
        put32(response + 16, mode.transmitted_bytes);  /* DSP frames sent (expect 23) */
        put32(response + 20, now_ms);                  /* sequencer elapsed ms */
        put32(response + 24, inta_blocks);             /* TX ring blocks streamed */
        put32(response + 28, reloads);                 /* TX ring XFERCOUNT reloads */
        put32(response + 32, i2s2_fifostat);           /* TXLVL/TXERR (drain => streaming to the bus) */
        put32(response + 36, (active_ch11 & 0x800u) | ((errint >> 11) & 1u ? 0x40000000u : 0u)); /* ch11 active + err */
        put32(response + 40, stats[1]);                /* UART3 RX bytes during handshake (DSP chatter) */
        put32(response + 44, (begin_ok << 1) | init_ok);
        put32(response + 48, fc2ctrl_rb);              /* FC2CTRLSEL (expect 0x101) */
        put32(response + 52, rspins);
        put32(response + 60, 0u);
        break;
    }
    case 38: { /* START CONTINUOUS PLAYBACK and HOLD (for the ANALOG LINE OUT test).
                * Same full sequence as opcode 37 (audio_mode: DSP handshake + pins + TX
                * ring), but priming a ~1 kHz square tone and, after LOCAL_COMPLETE,
                * LEAVING the ring streaming (no teardown) so the tone plays continuously.
                * Plug speakers/headphones into the base's analog out and listen -- if the
                * DSP routes our I2S stream to analog, you hear the tone (from-source audio
                * proven end-to-end, no wireless/MCU2 needed). Stop with opcode 39.
                * WARNING: start with the base volume LOW. */
        am_src_usb = 0u;                        /* stream the built-in test tone */
        volatile const uint32_t *const pll0stat = (volatile const uint32_t *)(uintptr_t)0x40000584u;
        if ((*pll0stat & 1u) == 0u) { response[3] = 2; put32(response + 60, 0xdead0002u); break; }
        const uint32_t systick_period = SysTick->LOAD + 1u;
        if ((SysTick->CTRL & SysTick_CTRL_ENABLE_Msk) == 0u) { response[3] = 2; put32(response + 60, 0xdead0000u); break; }
        if (!omni_control_uart_start(3, &omni_am_io)) { response[3] = 2; put32(response + 60, 0xdead0003u); break; }

        omni_audio_mode_ops_t ops;
        ops.context = 0; ops.write = am_write; ops.tx_status = am_tx_status;
        ops.pins = am_pins_route; ops.configure_begin = am_txring_begin; ops.configure_poll = am_txring_poll;
        omni_audio_mode_t mode;
        uint32_t init_ok = omni_audio_mode_init(&mode, &ops) ? 1u : 0u;
        uint32_t begin_ok = init_ok && omni_audio_mode_begin(&mode, 48000u, 0u) ? 1u : 0u;
        uint32_t now_ms = 0u, rem = 0u, prev = SysTick->VAL, iters = 0u;
        if (begin_ok) {
            for (; iters < OMNI_CPU_GUARD_ITERATIONS(3000000u); ++iters) {
                if (mode.state >= OMNI_AUDIO_MODE_LOCAL_COMPLETE) break;
                uint32_t cur = SysTick->VAL;
                uint32_t d = (prev >= cur) ? (prev - cur) : (prev + systick_period - cur);
                prev = cur; rem += d; while (rem >= systick_period) { rem -= systick_period; ++now_ms; }
                omni_audio_mode_poll(&mode, now_ms);
                watchdog_feed();
            }
        }
        /* brief advance check, then LEAVE the ring streaming (no teardown). */
        uint32_t rspins = 0u, inta_blocks = 0u, reloads = 0u;
        uint32_t xc_prev = (*(volatile const uint32_t *)(uintptr_t)0x400824b8u >> 16) & 0x3ffu;
        for (; rspins < OMNI_CPU_GUARD_ITERATIONS(100000u); ++rspins) {
            if (*(volatile const uint32_t *)(uintptr_t)0x40082058u & 0x800u) { inta_blocks++; *(volatile uint32_t *)(uintptr_t)0x40082058u = 0x800u; }
            uint32_t xc = (*(volatile const uint32_t *)(uintptr_t)0x400824b8u >> 16) & 0x3ffu;
            if (xc > xc_prev) reloads++;
            xc_prev = xc;
            watchdog_feed();
            if (inta_blocks >= 10u || reloads >= 10u) break;
        }
        uint32_t i2s2_fifostat = *(volatile const uint32_t *)(uintptr_t)0x40088e04u;
        omni_control_uart_stop(3); /* handshake done; free the DSP UART. Ring keeps streaming. */

        put32(response + 4,  (mode.state == OMNI_AUDIO_MODE_LOCAL_COMPLETE) ? 1u : 0u);
        put32(response + 8,  (uint32_t)mode.state);
        put32(response + 12, (uint32_t)mode.error);
        put32(response + 16, mode.transmitted_bytes);
        put32(response + 20, now_ms);
        put32(response + 24, inta_blocks);
        put32(response + 28, reloads);
        put32(response + 32, i2s2_fifostat);
        put32(response + 36, 1u);                   /* STREAMING NOW ACTIVE (held) -- listen on analog out */
        put32(response + 60, 0u);
        break;
    }
    case 39: { /* STOP the held playback started by opcode 38/40 (teardown the ring). */
        usb_audio_ring_stop();
        am_txring_teardown();
        put32(response + 4, 1u);
        put32(response + 8, usb_audio_ring_corrections()); /* drift corrections this session */
        break;
    }
    case 40: { /* USB-audio playback STATUS (lightweight, non-blocking -- safe to poll
                * while streaming). Playback is auto-managed by the main-loop lifecycle
                * (starts when Windows opens the stream, stops when it closes); this just
                * reports health. pb_state: 0 idle, 1 bringing up, 2 running. */
        put32(response + 4,  pb_state);
        put32(response + 8,  (uint32_t)pb_mode.state);
        put32(response + 12, (uint32_t)pb_mode.error);
        put32(response + 16, pb_fails);
        put32(response + 20, usb_audio_ring_frames_written()); /* advancing => PC audio arriving */
        put32(response + 24, usb_audio_ring_corrections());    /* drift drop/dup (should stay ~0 with feedback) */
        put32(response + 28, usb_audio_ring_feedback());       /* current 10.14 feedback value */
        put32(response + 32, *(volatile const uint32_t *)(uintptr_t)0x40088e04u); /* I2S2 FIFOSTAT (drain) */
        put32(response + 36, (uint32_t)audio_probe_alternate(1)); /* playback stream active */
        put32(response + 40, usb_audio_ring_write_index());
        put32(response + 44, usb_audio_ring_dups());           /* failsafe inserts (near-empty) */
        put32(response + 48, usb_audio_ring_drops());          /* failsafe drops (near-full) */
        put32(response + 52, usb_audio_ring_step());           /* resampler Q16 step (65536=unity) */
        put32(response + 56, usb_audio_ring_frames_out());     /* accepted USB frames, NOT DMA consumption */
        put32(response + 60, (usb_audio_ring_gap_max() << 16) | (usb_audio_ring_gap_now() & 0xffffu)); /* peak | live fill */
        break;
    }
    case 41: { /* TEST: force the reported UAC2 feedback value (10.14 in request[4..7];
                * 0 restores auto). Lets a host tool prove whether Windows honors feedback
                * by watching the delivered-frame rate follow a forced 47.0 / 49.0. */
        uint32_t v = get32(request + 4);
        if(!usb_audio_ring_set_force(v)) { response[3]=2; break; }
        put32(response + 4, v);
        put32(response + 8, usb_audio_ring_frames_written());
        break;
    }
    case 42: { /* TEST/diagnostic: force the PLL fractional multiplier (PLL0SSCG0 MD) in
                * request[4..7]; 0 restores the clock-recovery servo. Reports the live MD so a
                * host tool can confirm the servo is steering and see where it settled. */
        uint32_t v = get32(request + 4);
        usb_audio_ring_set_md_force(v);
        put32(response + 4, v);
        put32(response + 8, *(volatile const uint32_t *)(uintptr_t)0x40000590u); /* live PLL0SSCG0 */
        put32(response + 12, usb_audio_ring_gap_now());
        break;
    }
    case 43: { /* ABI2 TEST: request[4..7]=Kp MD/frame, [8..11]=Ki MD/(frame*second),
                * [12]=reset integral. 0 leaves a gain unchanged. Reports gains + state. */
        usb_audio_ring_set_gains((int32_t)get32(request + 4), (int32_t)get32(request + 8), request[12]);
        put32(response + 4,  (uint32_t)usb_audio_ring_kp());
        put32(response + 8,  (uint32_t)usb_audio_ring_ki_q8());
        put32(response + 12, (uint32_t)usb_audio_ring_servo_i());
        put32(response + 16, usb_audio_ring_gap_now());
        put32(response + 20, usb_audio_ring_dups());
        put32(response + 24, usb_audio_ring_drops());
        put32(response + 28, *(volatile const uint32_t *)(uintptr_t)0x40000590u); /* live MD */
        break;
    }
    case 44: { /* Audio pipeline ABI2: coherent queue/DMA or playback-clock status. */
        uint32_t values[15];
        if (request[4]==0) usb_audio_ring_status(values);
        else if (request[4]==1u) {
            uint32_t v[15]={2u,pb_state,(uint32_t)pb_mode.state,(uint32_t)pb_mode.error,
                pb_fails,pb_clock_state,pb_clock_error,omni_ui_milliseconds(),
                (uint32_t)audio_probe_alternate(1),configuration,
                (uint32_t)usb_audio_ring_kp(),(uint32_t)usb_audio_ring_ki_q8(),
                (uint32_t)usb_audio_ring_servo_i(),usb_audio_ring_frames_written(),
                usb_audio_ring_frames_out()};
            memcpy(values,v,sizeof(values));
        } else if (request[4]==2u) {
            memcpy(values,pb_clock_first_failure,sizeof(values));
            values[0]=2u; values[14]=pb_clock_attempts;
        } else if (request[4]==3u) {
            usb_audio_ring_delivery_status(values);
        } else if (request[4]==4u) {
            /* Bounded read-only I2S/framing context; no status clear or clock write. */
            if(pb_state!=2u) { response[3]=2; break; }
            static const uintptr_t addresses[]={0x40086c08u,0x40088c08u,
                0x40086c00u,0x40086c04u,0x40088c00u,0x40088c04u,
                0x40088e00u,0x40088e04u,0x40000420u,0x4000105cu,
                0x40000590u,0x40000280u,0x40000380u,0x40013010u};
            values[0]=2u;
            for(unsigned i=0;i<14u;++i) values[i+1u]=*(volatile const uint32_t *)addresses[i];
        } else { response[3]=2; break; }
        for(unsigned i=0;i<15u;++i) put32(response+4u+4u*i,values[i]);
        break;
    }
    case 45: { /* Read frozen/circular raw USB/DMA history; no tracing reset. */
        uint32_t values[15]; usb_audio_ring_trace(request[4],values);
        for(unsigned i=0;i<15u;++i) put32(response+4u+4u*i,values[i]);
        break;
    }
    case 46: { /* Bounded PLL0/FRO96 measurement against USB SOF; main-loop only. */
        uint32_t values[15];
        if(request[4]==0u) memcpy(values,sof_status,sizeof(values));
        else if(request[4]==1u) {
            uint32_t selector=request[5]==3u ? 3u : 1u;
            if((request[5]!=0u && request[5]!=3u) || memcmp(request+8,"SOF1",4) ||
               recovery_state || configuration!=1u ||
               (boot_ack_status!=OMNI_ACK_WRITTEN && boot_ack_status!=OMNI_ACK_ALREADY_VALID) ||
               (selector==1u && pb_state!=2u) ||
               sof_requested || sof_capture.state==OMNI_SOF_RUNNING) { response[3]=2; break; }
            sof_requested=selector;
            memcpy(values,sof_status,sizeof(values));
        } else if(request[4]==2u && request[5]<OMNI_SOF_CAPTURE_SAMPLES) {
            omni_sof_capture_trace(&sof_capture,request[5],values);
        } else if(request[4]==3u) {
            memset(values,0,sizeof(values));
            values[0]=2u; values[1]=(1u<<1)|(1u<<3);
            values[2]=OMNI_SOF_CAPTURE_SAMPLES; values[3]=OMNI_SOF_CAPTURE_TIMEOUT_MS;
            values[4]=0x40013010u;
        } else { response[3]=2; break; }
        values[14]=sof_requested;
        for(unsigned i=0;i<15u;++i) put32(response+4u+4u*i,values[i]);
        break;
    }
    case 47: {
        uint32_t values[15];
        if(!omni_dsp_volume_probe_status(request[4],values)) { response[3]=2; break; }
        for(unsigned i=0;i<15u;++i) put32(response+4u+4u*i,values[i]);
        break;
    }
    case 48: {
        uint32_t values[15];
        bool valid=request[5]==0u ? omni_usb_iso_status(request[4],values) :
            (request[5]==1u && omni_usb_iso_failure_status(request[4],values));
        if(!valid) { response[3]=2; break; }
        for(unsigned i=0;i<15u;++i) put32(response+4u+4u*i,values[i]);
        break;
    }
    case 49: { /* Historical trial pages; native controller now owns writes. */
        uint32_t values[15];
        if(request[4]!=0u) { response[3]=2; break; }
        if(!omni_dsp_gain_trial_status(request[5],values)) { response[3]=2; break; }
        for(unsigned i=0;i<15u;++i) put32(response+4u+4u*i,values[i]);
        break;
    }
    case 54: {
        uint32_t values[15];int16_t db;uint8_t muted;audio_probe_volume_snapshot(&db,&muted);
        omni_mixer_ui_status(db,muted!=0u,values);memcpy(response+4,values,60);break;
    }
    case 53: {
        if(request[4]>4u) {response[3]=2;break;}
        uint32_t values[15];omni_ui_buttons_status(request[4],values);
        for(unsigned i=0;i<15u;++i) put32(response+4u+4u*i,values[i]);
        break;
    }
    case 57: {
        uint32_t values[15];
        if(!omni_headset_battery_status(request[4],omni_ui_milliseconds(),values)) { response[3]=2; break; }
        for(unsigned i=0;i<15u;++i) put32(response+4u+4u*i,values[i]);
        break;
    }
    case 58: /* Queue one whitelisted DSP GET. No UART work in the HID callback. */
        if(configuration!=1u || recovery_state || memcmp(request+12,"DSPQ",4) ||
           (boot_ack_status!=OMNI_ACK_WRITTEN && boot_ack_status!=OMNI_ACK_ALREADY_VALID) ||
           omni_dsp_probe_busy() || omni_dsp_capture_probe_busy() ||
           omni_dsp_volume_probe_busy() ||
           !omni_headset_query_request(get32(request+8),request[4],omni_ui_milliseconds())) response[3]=2;
        break;
    case 59: {
        if(request[4]==2u) {
            if(!omni_headset_query_reply(get32(request+8),response+4)) response[3]=2;
        } else {
            uint32_t values[15];
            if(!omni_headset_query_status(request[4],omni_ui_milliseconds(),values)) { response[3]=2; break; }
            for(unsigned i=0;i<15u;++i) put32(response+4u+4u*i,values[i]);
        }
        break;
    }
    case 75: {
        uint32_t values[15];omni_microphone_status(values);memcpy(response+4,values,60);break;
    }
    case 73: /* Desired source-bias tuple, applied by main; read coherent evidence. */
        if(request[4]>1u || (request[4] && (configuration!=1u || recovery_state ||
           !omni_mixer_control_request(request[5],request[6])))) {response[3]=2;break;}
        if(native_have_snapshot) memcpy(response+4,native_snapshot[native_published].mixer_control,60);
        else put32(response+4,1u);
        break;
    case 72: /* Coherent, read-only wireless master gain transaction status. */
        if(request[4]>1u) {response[3]=2;break;}
        if(native_have_snapshot) memcpy(response+4,native_snapshot[native_published].headset_gain[request[4]],60);
        else {put32(response+4,1u);put32(response+8,request[4]);}
        break;
    case 71: /* Coherent, read-only remote menu transaction status. */
        if(request[4]>1u) {response[3]=2;break;}
        if(native_have_snapshot) memcpy(response+4,native_snapshot[native_published].menu[request[4]],60);
        else {put32(response+4,1u);put32(response+8,request[4]);}
        break;
    case 61: /* Native bounded settings SET; token, ID, length, value. */
        if(configuration!=1u || recovery_state || request[9]>54u ||
           (boot_ack_status!=OMNI_ACK_WRITTEN && boot_ack_status!=OMNI_ACK_ALREADY_VALID) ||
           (request[8]==DSP_SETTING_OUTPUT_MODE && (request[9]!=1u || request[10]!=2u)) ||
           !settings_enqueue(get32(request+4),request[8],request+10,request[9],omni_ui_milliseconds())) response[3]=2;
        break;
    case 62: {
        uint32_t values[15];
        if(!settings_status_copy(request[4],values)) {response[3]=2;break;}
        memcpy(response+4,values,60);break;
    }
    case 63:
        if(!settings_value_copy(request[4],request[5],response+4)) response[3]=2;
        break;
    case 64: { /* EQ staging: token,ID,total,offset,count,data[52]. */
        uint32_t token=get32(request+4);unsigned total=request[9],offset=request[10],count=request[11];
        if(!token || request[8]<DSP_SETTING_EQ_WIRELESS || request[8]>DSP_SETTING_EQ_BT ||
           total!=(request[8]==DSP_SETTING_EQ_WIRELESS?128u:78u) || !count || count>52u || offset+count>total) {response[3]=2;break;}
        if(token!=settings_blob_token || request[8]!=settings_blob_id || total!=settings_blob_length) {
            if(offset) {response[3]=2;break;}
            memset(settings_covered,0,sizeof(settings_covered));settings_blob_token=token;
            settings_blob_id=request[8];settings_blob_length=(uint8_t)total;
        }
        memcpy(settings_blob+offset,request+12,count);memset(settings_covered+offset,1,count);break;
    }
    case 65: {
        if(configuration!=1u || recovery_state || get32(request+4)!=settings_blob_token || !settings_blob_token ||
           (boot_ack_status!=OMNI_ACK_WRITTEN && boot_ack_status!=OMNI_ACK_ALREADY_VALID)) {response[3]=2;break;}
        bool complete=true;for(unsigned i=0;i<settings_blob_length;++i) if(!settings_covered[i]) complete=false;
        if(!complete || !settings_enqueue(settings_blob_token,settings_blob_id,settings_blob,settings_blob_length,omni_ui_milliseconds())) response[3]=2;
        break;
    }
    case 66: {
        uint32_t values[15];if(!mcu2_status_copy(request[4],values)) {response[3]=2;break;}
        memcpy(response+4,values,60);break;
    }
    case 67:
        if(configuration!=1u || recovery_state || !select_enqueue(request[4],omni_ui_milliseconds())) response[3]=2;
        break;
    case 68: {
        uint32_t values[15];
        if(request[4]>1u || (request[4] && (request[9] || !omni_ui_settings_set(request[5],request[6],request[7],request[8])))) {response[3]=2;break;}
        omni_ui_settings_status(values);memcpy(response+4,values,60);break;
    }
    case 69: {
        if(request[7]>1u || (request[6]&~3u) || (request[7] && !omni_mixer_ui_configure(request[4],request[5],(request[6]&1u)!=0u,(request[6]&2u)!=0u))) {response[3]=2;break;}
        uint32_t values[15];int16_t db;uint8_t muted;audio_probe_volume_snapshot(&db,&muted);
        omni_mixer_ui_status(db,muted!=0u,values);memcpy(response+4,values,60);break;
    }
    case 70: {
        uint8_t value[OMNI_DSP_SETTINGS_MAX_VALUE];
        size_t n=omni_dsp_settings_eq_flat(request[8],request[9],value);
        if(configuration!=1u || recovery_state || !n ||
           (boot_ack_status!=OMNI_ACK_WRITTEN && boot_ack_status!=OMNI_ACK_ALREADY_VALID) ||
           !settings_enqueue(get32(request+4),request[8],value,n,omni_ui_milliseconds())) response[3]=2;
        break;
    }
    case 60:
        if(!omni_dsp_meter_read(request[4],omni_ui_milliseconds(),response+4)) response[3]=2;
        break;
    case 52: {
        uint32_t values[15]; omni_charger_adc_status(values);
        for(unsigned i=0;i<15u;++i) put32(response+4u+4u*i,values[i]);
        break;
    }
    case 51: {
        uint32_t values[15]; omni_charger_status(values);
        for(unsigned i=0;i<15u;++i) put32(response+4u+4u*i,values[i]);
        break;
    }
    case 50: {
        uint32_t values[15];
        if(!omni_native_gain_read(request[4],values)) { response[3]=2; break; }
        for(unsigned i=0;i<15u;++i) put32(response+4u+4u*i,values[i]);
        break;
    }
    case 76: /* Host OLED framebuffer chunk: offset LE16 [4..5], count [6], bytes [7..]. */
        if(!omni_ui_external_chunk(request[4]|((unsigned)request[5]<<8),request+7,request[6],omni_ui_milliseconds())) response[3]=2;
        break;
    case 77: /* Tune display SPI divider (SCK=12MHz/(div+1)) for faster refresh. */
        omni_display_lpc5528_set_div(request[4]|((unsigned)request[5]<<8));
        break;
    case 74: {
        uint32_t values[15];
        if(request[4]==0u) audio_probe_format_status(values);
        else if(request[4]==1u) usb_audio_ring_format_status(values);
        else if(request[4]==2u) playback_startup_status(values);
        else {response[3]=2;break;}
        memcpy(response+4,values,60);break;
    }
    default: response[3]=1; break;
    }
}

static usb_status_t hid_callback(class_handle_t handle,uint32_t event,void *param)
{
    (void)handle;
    usb_device_hid_report_struct_t *r=param;
    if (event==kUSB_DeviceHidEventSendResponse) return kStatus_USB_Success;
    if (event==kUSB_DeviceHidEventSetIdle || event==kUSB_DeviceHidEventGetIdle)
        return kStatus_USB_Success;
    if (event!=kUSB_DeviceHidEventGetReport && event!=kUSB_DeviceHidEventSetReport &&
        event!=kUSB_DeviceHidEventRequestReportBuffer) return kStatus_USB_InvalidRequest;
    if (!r || r->reportId!=1 || r->reportType!=3) return kStatus_USB_InvalidRequest;
    if (event==kUSB_DeviceHidEventGetReport) {
        r->reportBuffer=response; r->reportLength=64; return kStatus_USB_Success;
    }
    if (r->reportLength!=64) return kStatus_USB_InvalidRequest;
    if (event==kUSB_DeviceHidEventRequestReportBuffer) {
        r->reportBuffer=request; return kStatus_USB_Success;
    }
    if (r->reportBuffer!=request || request[0]!=1) return kStatus_USB_InvalidRequest;
    command();
    return kStatus_USB_Success;
}

static usb_status_t device_callback(usb_device_handle handle,uint32_t event,void *param)
{
    (void)handle;
    if (event==kUSB_DeviceEventBusReset) { configuration=0; return kStatus_USB_Success; }
    if (!param) return kStatus_USB_InvalidRequest;
    usb_device_get_descriptor_common_struct_t *d=param;
    switch(event) {
    case kUSB_DeviceEventSetConfiguration:
        if (*(uint8_t *)param>1) break;
        configuration=*(uint8_t *)param; return kStatus_USB_Success;
    case kUSB_DeviceEventGetConfiguration:
        *(uint8_t *)param=configuration; return kStatus_USB_Success;
    case kUSB_DeviceEventSetInterface:
    case kUSB_DeviceEventGetInterface: {
        uint16_t *value=param;
        uint8_t iface=(uint8_t)(*value>>8);
        int alt=iface==OMNI_HID_INTERFACE?0:audio_probe_alternate(iface);
        if (alt<0) break;
        if (event==kUSB_DeviceEventGetInterface) *value=(uint16_t)((uint16_t)(iface<<8)|(uint16_t)alt);
        else if ((uint8_t)*value!=(uint8_t)alt) break;
        return kStatus_USB_Success;
    }
    case kUSB_DeviceEventGetDeviceDescriptor:
        d->buffer=device_desc; d->length=sizeof(device_desc); return kStatus_USB_Success;
    case kUSB_DeviceEventGetConfigurationDescriptor:
        if (((usb_device_get_configuration_descriptor_struct_t *)param)->configuration!=0) break;
        d->buffer=config_desc; d->length=sizeof(config_desc); return kStatus_USB_Success;
    case kUSB_DeviceEventGetHidDescriptor:
    case kUSB_DeviceEventGetHidReportDescriptor:
        if (((usb_device_get_hid_descriptor_struct_t *)param)->interfaceNumber!=OMNI_HID_INTERFACE) break;
        d->buffer=event==kUSB_DeviceEventGetHidDescriptor?config_desc+OMNI_HID_DESCRIPTOR_OFFSET:report_desc;
        d->length=event==kUSB_DeviceEventGetHidDescriptor?9:sizeof(report_desc);
        return kStatus_USB_Success;
    case kUSB_DeviceEventGetStringDescriptor: {
        uint8_t index=((usb_device_get_string_descriptor_struct_t *)param)->stringIndex;
        const char *s=omni_usb_string(index,build_id);
        if (index==0) { string_buf[0]=4; string_buf[1]=3; string_buf[2]=9; string_buf[3]=4; }
        else if(s) {
            size_t n=strlen(s);if(n>(sizeof(string_buf)-2u)/2u) break;
            string_buf[0]=(uint8_t)(2+2*n); string_buf[1]=3;
            for(size_t i=0;i<n;++i) { string_buf[2+2*i]=(uint8_t)s[i]; string_buf[3+2*i]=0; }
        } else break;
        d->buffer=string_buf; d->length=string_buf[0]; return kStatus_USB_Success;
    }
    default: break;
    }
    return kStatus_USB_InvalidRequest;
}
static usb_status_t audio_callback(class_handle_t handle,uint32_t event,void *param)
{ (void)handle; (void)event; (void)param; return kStatus_USB_InvalidRequest; }
static usb_device_class_config_struct_t classes[]={
    {hid_callback,NULL,&hid_class},{audio_callback,NULL,&audio_class}};
static usb_device_class_config_list_struct_t class_list={classes,device_callback,2};
void USB0_IRQHandler(void) { USB_DeviceLpcIp3511IsrFunction(device); }

/* --- non-blocking main-loop playback lifecycle (auto start/stop on the UAC2 stream) ---
 * Runs entirely in the main loop, so it NEVER stalls the USB ISR: diagnostics and
 * recovery stay responsive while audio streams. Blocking here (a couple ms for the
 * clock) is fine -- the USB ISR (priority 3) preempts the thread. */
static bool bring_up_local_ready(void)
{
    static uint32_t last_attempt_ms;
    static bool attempted;
    static bool owned_and_settled;
    if (pb_clock_blocked) return false;
    uint32_t now = omni_ui_milliseconds();
    /* Failed readiness must not rerun the programming sequence on every main
     * loop iteration. Keep its last reason visible throughout the cooldown. */
    if (attempted && (uint32_t)(now - last_attempt_ms) < 1000u) return false;
    last_attempt_ms = now;
    ++pb_clock_attempts;
    attempted = true;
    pb_clock_state = OMNI_AUDIO_CLOCK_PREFLIGHT;
    pb_clock_error = OMNI_AUDIO_CLOCK_ERROR_NONE;
    volatile const uint32_t *const ahbclk2 = (volatile const uint32_t *)(uintptr_t)0x40000208u;
    volatile const uint32_t *const preset2 = (volatile const uint32_t *)(uintptr_t)0x40000108u;
    if (((*ahbclk2 >> 27) & 1u) == 0u || ((*preset2 >> 27) & 1u) != 0u) {
        pb_clock_state = OMNI_AUDIO_CLOCK_FAILED;
        pb_clock_error = OMNI_AUDIO_CLOCK_ERROR_PREREQUISITE;
        remember_clock_failure(NULL,false);
        return false; /* ANACTRL not clocked */
    }
    omni_audio_clock_lpc5528_t backend; backend.mmio_read = 0; backend.mmio_write = 0; backend.user = 0;
    omni_audio_clock_ops_t ops = omni_audio_clock_lpc5528_ops(&backend);
    bool xo_changed=false;
    uint32_t m = __get_PRIMASK(); __disable_irq();
    omni_audio_clock_error_t prepared=omni_audio_clock_startup_prepare_xo(&ops,&xo_changed);
    __set_PRIMASK(m);
    if (prepared!=OMNI_AUDIO_CLOCK_ERROR_NONE) {
        owned_and_settled = false;
        pb_clock_state = OMNI_AUDIO_CLOCK_FAILED;
        pb_clock_error = (uint32_t)prepared;
        remember_clock_failure(NULL,xo_changed);
        return false;
    }
    volatile const uint32_t *const xo_status = (volatile const uint32_t *)(uintptr_t)0x40013024u;
    if (xo_changed || (*xo_status & 1u) == 0u) {
        owned_and_settled = false;
        pb_clock_state = OMNI_AUDIO_CLOCK_WAIT_XO;
        uint32_t xo_started = omni_ui_milliseconds();
        for (uint32_t it = 0; it < OMNI_CPU_GUARD_ITERATIONS(200000u); ++it) {
            if (*xo_status & 1u) break;
            watchdog_feed();
            if ((uint32_t)(omni_ui_milliseconds() - xo_started) >= OMNI_AUDIO_CLOCK_XO_TIMEOUT_MS) break;
        }
        if ((*xo_status & 1u) == 0u) {
            pb_clock_state = OMNI_AUDIO_CLOCK_FAILED;
            pb_clock_error = OMNI_AUDIO_CLOCK_ERROR_XO_TIMEOUT;
            remember_clock_failure(NULL,xo_changed);
            return false;
        }
    }
    if (owned_and_settled) {
        static const omni_audio_clock_register_t checks[]={
            OMNI_AC_XO_STATUS,OMNI_AC_XO_CTRL,OMNI_AC_PLL0SEL,OMNI_AC_PLL0CTRL,
            OMNI_AC_PLL0NDEC,OMNI_AC_PLL0PDEC,OMNI_AC_PLL0SSCG0,OMNI_AC_PLL0SSCG1,
            OMNI_AC_MCLKSEL,OMNI_AC_PLL0DIV,OMNI_AC_MCLKDIV
        };
        uint32_t current[OMNI_AC_REGISTER_COUNT]={0};
        bool read_ok=true;
        for (unsigned i=0;i<sizeof(checks)/sizeof(checks[0]);++i)
            if (!ops.read(ops.context,checks[i],&current[checks[i]])) { read_ok=false; break; }
        uint32_t power=*(volatile const uint32_t *)(uintptr_t)0x400200b8u;
        if (read_ok && omni_audio_clock_startup_ready(owned_and_settled,current,power)) {
            pb_clock_state = OMNI_AUDIO_CLOCK_LOCAL_READY;
            attempted = false;
            return true;
        }
        /* An inherited or changed clock cannot skip the guarded settling
         * sequence merely because LOCK flickered high or MCLKSEL is odd. */
        owned_and_settled = false;
    }
    omni_audio_clock_t clk;
    if (!omni_audio_clock_init(&clk, &ops)) {
        pb_clock_state = OMNI_AUDIO_CLOCK_FAILED;
        pb_clock_error = OMNI_AUDIO_CLOCK_ERROR_PREREQUISITE;
        return false;
    }
    bool ready = omni_audio_clock_startup_run(&clk, 0x01c3459au,
        omni_ui_milliseconds, watchdog_feed);
    pb_clock_state = (uint32_t)clk.state;
    pb_clock_error = (uint32_t)clk.error;
    owned_and_settled = ready;
    if (!ready) remember_clock_failure(&clk,clk.successful_writes!=0u || xo_changed);
    if (ready) attempted = false;
    return ready;
}

static void playback_teardown(void)
{
    am_txring_teardown();
    usb_audio_ring_stop();
    /* Only the startup handshake owns this UART. Running/idle playback
     * must not tear down a concurrent mixer transaction. */
    if(pb_state==1u || pb_state==4u) omni_control_uart_stop(3);
    pb_state = 0u;
}

static bool playback_receive(uint32_t now)
{
    /* Startup owns UART3 exclusively. Forward passive lifecycle/control reports
     * even while collecting ACKs; native gain owns reception only after release. */
    omni_link_expire(&am_rx_parser,now);
    for(unsigned budget=0;budget<64u;++budget) {
        uint8_t byte;
        int result=omni_am_io.rx(omni_am_io.context,&byte);
        if(result<0) return false;
        if(!result) break;
        runtime_dsp_observe(byte,now);
        uint32_t ignored=am_rx_parser.ignored;
        const uint8_t *frame;size_t length;
        (void)omni_link_feed(&am_rx_parser,byte,now,&frame,&length);
        /* Stock framing intentionally filters DD from ordinary DB dispatch.
         * Its ignored counter advances only after all four bytes arrived. */
        if(am_rx_parser.ignored!=ignored)
            (void)omni_audio_mode_receive_ack(&pb_mode,am_rx_parser.bytes,4u);
    }
    return true;
}

static void playback_failure_record(uint32_t now,uint32_t reason)
{
    /* Retain the first cause before teardown or another owner resets UART3.
     * Reasons:1 RX,2 mode,3 USB ISO,4 DMA ring,5 failed physical quiescence. */
    uint32_t mask=__get_PRIMASK();__disable_irq();
    pb_failure_epoch=pb_format.epoch;pb_failure_ms=now;
    pb_failure_mode=(uint32_t)pb_mode.state|((uint32_t)pb_mode.error<<8)|(reason<<16);
    pb_failure_ordinal=pb_fails+1u;
    omni_control_uart_stats(3u,pb_failure_uart);
    __set_PRIMASK(mask);
}
static void playback_startup_status(uint32_t out[15])
{
    uint32_t values[15]={1u,2u,(uint32_t)pb_mode.state|((uint32_t)pb_mode.error<<8),
        pb_mode.acknowledged_frames|((uint32_t)pb_mode.require_ack<<8)|
            ((uint32_t)pb_mode.ack_waiting<<9)|((uint32_t)pb_mode.ack_received<<10)|
            ((uint32_t)pb_mode.quiescing<<11)|((uint32_t)pb_mode.ack_command<<16)|
            ((uint32_t)pb_mode.ack_status<<24),
        pb_mode.transmitted_bytes,pb_failure_epoch,pb_failure_ms,pb_failure_mode,pb_failure_ordinal};
    memcpy(values+9,pb_failure_uart,sizeof(pb_failure_uart));
    memcpy(out,values,sizeof(values));
}

static void playback_service(void)
{
    omni_audio_format desired;
    bool stream=audio_probe_playback_format(&desired);
    bool capture=audio_probe_alternate(OMNI_MICROPHONE_AS_INTERFACE)>0;
    usb_audio_ring_capture_only(!stream);
    int want = (configuration == 1 && !recovery_state && (stream || capture) &&
        (boot_ack_status==OMNI_ACK_WRITTEN || boot_ack_status==OMNI_ACK_ALREADY_VALID));
    uint32_t now = omni_ui_milliseconds();
    if(pb_state==4u) {
        /* The DMA producer/consumer is already stopped. Keep UART ownership
         * until the current frame drains; a fresh format cannot splice it. */
        bool receive_ok=playback_receive(now);
        if((!receive_ok || pb_mode.state==OMNI_AUDIO_MODE_FAILED) && !pb_stop_fault) {
            playback_failure_record(now,receive_ok?2u:1u);
            pb_stop_fault=true;++pb_fails;
        }
        omni_audio_mode_io_t stopped=omni_audio_mode_quiesce(&pb_mode,now);
        if(stopped==OMNI_AUDIO_MODE_IO_PENDING) return;
        if(stopped==OMNI_AUDIO_MODE_IO_ERROR && !pb_stop_fault) playback_failure_record(now,5u);
        playback_teardown();
        /* A successfully drained old generation cannot poison the host's new
         * stream. Keep real cancellation/ownership errors latched regardless
         * of epoch; a fresh handshake is allowed only after COMPLETE. */
        bool same_format=desired.epoch==pb_format.epoch &&
            desired.sample_rate==pb_format.sample_rate && desired.sample_bits==pb_format.sample_bits;
        if(stopped==OMNI_AUDIO_MODE_IO_ERROR || (pb_stop_fault && want && same_format)) {
            pb_format=desired;pb_state=3u;
            if(stopped==OMNI_AUDIO_MODE_IO_ERROR && !pb_stop_fault) ++pb_fails;
        }
        return;
    }
    /* An epoch also captures close/reopen between main-loop polls. A format
     * change never reuses old queued samples or DMA-owned descriptors. */
    if(pb_state && (!want || desired.epoch!=pb_format.epoch ||
        desired.sample_rate!=pb_format.sample_rate || desired.sample_bits!=pb_format.sample_bits)) {
        if(pb_state==1u) {
            am_txring_teardown();pb_stop_fault=false;pb_state=4u;
        } else playback_teardown();
        return;
    }
    /* A diagnostic DSP reservation must finish before playback claims UART3.
     * MCU2 owns independent UART7 and does not reserve the audio transport. */
    if (want && (omni_dsp_capture_probe_busy() || omni_dsp_probe_busy())) return;
    if (want && pb_state!=3u && audio_probe_playback_failed()) {
        playback_failure_record(now,3u);
        if(pb_state==1u) { am_txring_teardown();pb_stop_fault=true;pb_state=4u; }
        else { if(pb_state) playback_teardown();pb_format=desired;pb_state=3u; }
        ++pb_fails; return;
    }
    if (pb_state == 0u) {
        if (want) {
            if(!omni_native_gain_release(now)) return;
            pb_format=desired;
            if(!omni_audio_format_valid(&pb_format)) { pb_state=3u; ++pb_fails; return; }
            if(!usb_audio_ring_reset()) { pb_state=3u; ++pb_fails; return; }
            uint32_t attempts=pb_clock_attempts;
            if (!bring_up_local_ready()) { if(pb_clock_attempts!=attempts) ++pb_fails; return; }
            am_src_usb = 1u;
            omni_audio_mode_ops_t ops;
            ops.context = &pb_format; ops.write = am_write; ops.tx_status = am_tx_status;
            ops.pins = am_pins_route; ops.configure_begin = am_txring_begin; ops.configure_poll = am_txring_poll;
            if (omni_control_uart_start(3, &omni_am_io) && omni_link_init(&am_rx_parser,100u) &&
                omni_audio_mode_init(&pb_mode, &ops) && omni_audio_mode_require_ack(&pb_mode) &&
                omni_audio_mode_begin(&pb_mode, pb_format.sample_rate, omni_ui_milliseconds())) {
                pb_state = 1u;
            } else { usb_audio_ring_stop(); omni_control_uart_stop(3); pb_state=3u; ++pb_fails; }
        }
    } else if (pb_state == 1u) {
        if(!playback_receive(now)) {
            playback_failure_record(now,1u);
            am_txring_teardown();pb_stop_fault=true;pb_state=4u;++pb_fails;return;
        }
        omni_audio_mode_poll(&pb_mode, now);
        if (pb_mode.state == OMNI_AUDIO_MODE_LOCAL_COMPLETE) { omni_control_uart_stop(3); pb_state = 2u; }
        else if (pb_mode.state == OMNI_AUDIO_MODE_FAILED || pb_mode.state == OMNI_AUDIO_MODE_CANCELED) {
            playback_failure_record(now,2u);
            am_txring_teardown();pb_stop_fault=true;pb_state=4u; ++pb_fails;return;
        }
    } else if(pb_state==3u) { /* preserve DMA ownership fault until stream closes */
        if(!want) playback_teardown();
        return;
    } else { /* RUNNING */
        if (!want) { playback_teardown(); return; }
    }
    if(pb_state && usb_audio_ring_fault()) {
        playback_failure_record(now,4u);
        if(pb_state==1u) { am_txring_teardown();pb_stop_fault=true;pb_state=4u; }
        else { playback_teardown();pb_format=desired;pb_state=3u; }
        ++pb_fails; return;
    }
    if(pb_state==2u && capture && !microphone_started) {
        microphone_started=true;
        (void)omni_microphone_start(&omni_dma_desc[16],pb_format.sample_rate);
    }
    if(microphone_started && !capture) {
        (void)omni_microphone_stop();microphone_started=false;
    }
    /* DMA is already active during the tail of the DSP handshake. */
    usb_audio_ring_clock_servo();
}

static void sof_capture_service(void)
{
    if(!sof_requested && sof_capture.state!=OMNI_SOF_RUNNING) return;
    if(sof_requested) {
        /* Keep pending asserted during setup so an interrupt cannot enqueue
         * a second capture before the first publishes its running state. */
        uint32_t selector=sof_requested;
        if(!recovery_state && configuration==1u && (selector==3u || pb_state==2u)) {
            sof_active_selector=selector;
            (void)omni_sof_capture_start_mode(&sof_capture,omni_ui_milliseconds(),selector);
        }
        sof_requested=0u;
    }
    if(recovery_state || configuration!=1u || (sof_active_selector!=3u && pb_state!=2u))
        omni_sof_capture_stop(&sof_capture);
    else omni_sof_capture_poll(&sof_capture,omni_ui_milliseconds());
    uint32_t values[15];
    omni_sof_capture_status(&sof_capture,values);
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    memcpy(sof_status,values,sizeof(values));
    __set_PRIMASK(mask);
}

int main(void)
{
    _Static_assert(sizeof(build_id)<=44,"build ID exceeds HID response");
    board_init();
    omni_sof_capture_ops_t sof_ops=omni_sof_capture_mmio_ops();
    omni_sof_capture_init(&sof_capture,&sof_ops);
    omni_sof_capture_status(&sof_capture,sof_status);
    if (USB_DeviceClassInit(kUSB_ControllerLpcIp3511Fs0,&class_list,&device)!=kStatus_USB_Success)
        Fault_Handler();
    /* The retained updater can hand off without a host-visible removal.
     * Establish a detached interval using our initialized controller. */
    if (USB_DeviceStop(device)!=kStatus_USB_Success) Fault_Handler();
    board_usb_detach_wait();
    NVIC_SetPriority(USB0_IRQn,3);
    NVIC_ClearPendingIRQ(USB0_IRQn);
    NVIC_EnableIRQ(USB0_IRQn);
    __enable_irq();
    if (USB_DeviceRun(device)!=kStatus_USB_Success) Fault_Handler();
    omni_ui_clock_start();
    (void)omni_link_init(&runtime_dsp_parser,20u);
    omni_mcu2_controls_init(&runtime_controls);
    omni_native_gain_observer(runtime_dsp_observe);
    for (;;) {
        watchdog_feed();
        omni_usb_guard_poll(omni_ui_milliseconds());
        audio_probe_poll();
        /* First-boot metadata programming masks interrupts and detaches USB.
         * Start UART7 only after that blackout; otherwise one unsolicited
         * envelope can overflow its FIFO before the main loop can drain it. */
        if(recovery_state || boot_ack_status==OMNI_ACK_WRITTEN || boot_ack_status==OMNI_ACK_ALREADY_VALID)
            omni_mcu2_service(omni_ui_milliseconds(),!recovery_state);
        native_commands_service(omni_ui_milliseconds(),configuration==1u && !recovery_state &&
            (boot_ack_status==OMNI_ACK_WRITTEN || boot_ack_status==OMNI_ACK_ALREADY_VALID));
        /* Resolve a probe reservation or cancellation before playback can
         * acquire UART3. A queued probe cooperatively drains native gain. */
        omni_dsp_capture_probe_poll(omni_ui_milliseconds(), configuration==1 && !recovery_state &&
            audio_probe_alternate(OMNI_PLAYBACK_AS_INTERFACE)==0 && audio_probe_alternate(OMNI_MICROPHONE_AS_INTERFACE)==0 &&
            (boot_ack_status==OMNI_ACK_WRITTEN || boot_ack_status==OMNI_ACK_ALREADY_VALID));
        playback_service();   /* auto start/stop USB-audio playback (non-blocking) */
        if(pb_state!=1u && pb_state!=4u && !omni_dsp_capture_probe_busy() && !omni_dsp_probe_busy() &&
           !(pb_state==0u && audio_probe_alternate(OMNI_PLAYBACK_AS_INTERFACE)>0))
        omni_native_gain_service(omni_ui_milliseconds(),(pb_state==2u || (pb_state==0u && audio_probe_alternate(1)==0)) &&
            configuration==1u && !recovery_state &&
            (boot_ack_status==OMNI_ACK_WRITTEN || boot_ack_status==OMNI_ACK_ALREADY_VALID));
        native_commands_publish(omni_ui_milliseconds());
        omni_mixer_control_service(omni_ui_milliseconds(),configuration==1u && !recovery_state &&
            (boot_ack_status==OMNI_ACK_WRITTEN || boot_ack_status==OMNI_ACK_ALREADY_VALID));
        if(configuration==1u && !recovery_state &&
           (boot_ack_status==OMNI_ACK_WRITTEN || boot_ack_status==OMNI_ACK_ALREADY_VALID))
            omni_charger_poll(omni_ui_milliseconds());
        sof_capture_service(); /* observation only; stops after 64 captures/250ms */
        omni_dsp_probe_poll(omni_ui_milliseconds(), configuration==1 && !recovery_state &&
            audio_probe_alternate(OMNI_PLAYBACK_AS_INTERFACE)==0 && audio_probe_alternate(OMNI_MICROPHONE_AS_INTERFACE)==0 &&
            (boot_ack_status==OMNI_ACK_WRITTEN || boot_ack_status==OMNI_ACK_ALREADY_VALID));
        /* Acknowledge only after USB has started and the host configured us.
         * Flash work runs once in main context, never inside an EP0 callback. */
        if (configuration == 1 && boot_ack_status == OMNI_ACK_WAITING) {
            /* Keep the loader's successful-host-configuration gate, but do
             * not stall a live USB session during ROM flash operations. The
             * subsequent bus reset rebuilds class/endpoint state normally. */
            if (USB_DeviceStop(device)!=kStatus_USB_Success) Fault_Handler();
            boot_acknowledge_startup();
            board_usb_detach_wait();
            if (USB_DeviceRun(device)!=kStatus_USB_Success) Fault_Handler();
        }
        if ((boot_ack_status==OMNI_ACK_WRITTEN || boot_ack_status==OMNI_ACK_ALREADY_VALID) && !recovery_state)
            omni_ui_poll();
        if (recovery_state==1) {
            int32_t driver=0;
            omni_ack_result result=boot_prepare_recovery(&driver);
            recovery_driver=driver;
            recovery_result=(uint32_t)result;
            recovery_state=(result==OMNI_ACK_WRITTEN || result==OMNI_ACK_ALREADY_VALID) && driver==0 ? 2U : 3U;
        }
        /* The commit transfer may end at disconnect. The host must prove the
         * bootloader identity and installed code; a write count is not success. */
        if (recovery_commit && recovery_state==2) {
            if (USB_DeviceStop(device)!=kStatus_USB_Success) Fault_Handler();
            NVIC_DisableIRQ(USB0_IRQn);
            NVIC_ClearPendingIRQ(USB0_IRQn);
            board_usb_detach_wait();
            NVIC_SystemReset();
        }
    }
}
