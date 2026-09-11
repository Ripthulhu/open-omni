#include "dsp_volume_probe.h"
#include "dsp_volume.h"
#include "dsp_gain_trial.h"
#include "control_uart.h"
#include "audio_probe.h"
#include "fsl_device_registers.h"
#include <string.h>

static omni_dsp_volume query={.desired_db=-30*256,.desired_muted=1u};
static mcu2_link_io byte_io;
/* 0 waiting for first allowed running playback,1 querying,2 frozen. */
static uint32_t stage, finished_ms, start_failures, cancellations;
static uint32_t uart_stats[6], last_fifo, last_stat, drain_polls;
static bool uart_owned;
static uint32_t playback_stable_since;
static bool playback_stable;
static omni_gain_trial gain_trial;
static volatile uint32_t gain_token;
static uint32_t gain_stage; /* 0 idle, 1 requested, 2 running, 3 frozen */
static uint8_t gain_restore[8];
static bool restore_only;

bool omni_dsp_volume_probe_busy(void) { return stage==1u || gain_stage==1u || gain_stage==2u; }

static int transmit(void *context,uint8_t byte)
{
    (void)context;
    return uart_owned && byte_io.tx ? byte_io.tx(byte_io.context,byte) : -1;
}
static int receive(void *context,uint8_t *byte)
{
    (void)context;
    return uart_owned && byte_io.rx ? byte_io.rx(byte_io.context,byte) : -1;
}

static int tx_complete(void *context)
{
    (void)context;
    if(!uart_owned) return -1;
    omni_control_uart_stats(3u,uart_stats);
    if(uart_stats[5]) return -1;
    last_fifo=USART3->FIFOSTAT;
    last_stat=USART3->STAT;
    ++drain_polls;
    if(last_fifo&(USART_FIFOSTAT_TXERR_MASK|USART_FIFOSTAT_RXERR_MASK)) return -1;
    /* UM11126 USART STAT bit3: TXIDLE means no frame is being serialized.
     * FIFOSTAT bit4 alone only describes the FIFO; its final byte may still
     * be on the wire. Both are needed before starting the20ms cooldown. */
    return ((last_fifo&USART_FIFOSTAT_TXEMPTY_MASK) &&
            (last_stat&USART_STAT_TXIDLE_MASK)) ? 1 : 0;
}

static void finish(uint32_t now)
{
    if(uart_owned) {
        /* playback_teardown may already have stopped this port. The UART
         * stop API is idempotent for an inactive port and preserves stats. */
        omni_control_uart_stop(3u);
        omni_control_uart_stats(3u,uart_stats);
        uart_owned=false;
    }
    finished_ms=now; __DMB(); stage=2u;
}

static void desired_snapshot(void)
{
    int16_t db; uint8_t mute;
    audio_probe_volume_snapshot(&db,&mute);
    /* Briefly serialize publication with HID status reads; never keep the
     * interrupt mask over UART operations or an RX polling loop. */
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    (void)omni_dsp_volume_desire(&query,db,mute!=0u);
    __set_PRIMASK(mask);
}

bool omni_dsp_gain_trial_request(uint32_t token)
{
    if(!token) return false;
    if(gain_token) return gain_token==token && !restore_only; /* Host retries cannot repeat writes. */
    if(stage!=2u || query.phase!=OMNI_DSP_VOLUME_COMPLETE) return false;
    gain_token=token; __DMB(); gain_stage=1u; return true;
}

bool omni_dsp_gain_restore_request(uint32_t token,const uint8_t expected[4],const uint8_t original[4])
{
    if(!token || !expected || !original || !expected[0] ||
       (unsigned)expected[0]+25u!=original[0] || memcmp(expected+1,original+1,3)) return false;
    for(unsigned i=0;i<4u;++i) if(expected[i]>100u || original[i]>100u) return false;
    if(gain_token) return gain_token==token && restore_only &&
        !memcmp(gain_restore,expected,4) && !memcmp(gain_restore+4,original,4);
    if(stage!=2u || query.phase!=OMNI_DSP_VOLUME_COMPLETE) return false;
    memcpy(gain_restore,expected,4); memcpy(gain_restore+4,original,4);
    restore_only=true; gain_token=token; __DMB(); gain_stage=1u; return true;
}

static void gain_poll(uint32_t now,bool allowed)
{
    if(!gain_stage || gain_stage==3u) return;
    if(!allowed) {
        if(gain_stage==2u) omni_gain_trial_cancel(&gain_trial);
        else gain_trial.phase=GAIN_CANCELED;
    } else if(gain_stage==1u) {
        if(!omni_control_uart_start(3u,&byte_io)) {
            gain_trial.phase=GAIN_FAILED; gain_trial.error=GAIN_IO;
        } else {
            uart_owned=true;
            omni_dsp_volume_io ops={NULL,transmit,receive,tx_complete};
            bool started=restore_only ? omni_gain_restore_begin(&gain_trial,ops,now,gain_restore,gain_restore+4) :
                omni_gain_trial_begin(&gain_trial,ops,now);
            if(!started) {
                gain_trial.phase=GAIN_FAILED; gain_trial.error=GAIN_IO;
            } else gain_stage=2u;
        }
    }
    if(gain_stage==2u && omni_gain_trial_busy(&gain_trial)) {
        omni_control_uart_stats(3u,uart_stats);
        if(uart_stats[5]) { gain_trial.phase=GAIN_FAILED; gain_trial.error=GAIN_IO; }
        else omni_gain_trial_poll(&gain_trial,now);
    }
    if(!omni_gain_trial_busy(&gain_trial)) {
        finish(now); __DMB(); gain_stage=3u;
    }
}

bool omni_dsp_gain_trial_status(unsigned page,uint32_t out[15])
{
    if(!out || page>8u) return false;
    uint32_t v[15]={1u,page,gain_token,gain_stage};
    if(!page) {
        v[4]=(uint32_t)gain_trial.phase; v[5]=(uint32_t)gain_trial.error;
        v[6]=gain_trial.step; v[7]=gain_trial.set_may_have_applied;
        v[8]=(uint32_t)gain_trial.attenuated_verified|((uint32_t)gain_trial.restored_verified<<1);
        memcpy(v+9,gain_trial.original,4); memcpy(v+10,gain_trial.reduced,4);
        memcpy(v+11,gain_trial.observed,4);
        v[12]=gain_trial.tx_bytes; v[13]=gain_trial.rx_bytes; v[14]=gain_trial.unrelated;
    } else {
        unsigned s=page-1u;
        v[4]=gain_trial.tx_length[s]; v[5]=gain_trial.rx_length[s];
        memcpy(v+6,gain_trial.tx_frame[s],8); memcpy(v+8,gain_trial.rx_frame[s],8);
        memcpy(v+10,gain_trial.ack_frame[s],4);
        v[11]=gain_trial.offset; v[12]=gain_trial.parser.malformed;
        v[13]=gain_trial.parser.expired; v[14]=gain_trial.hold_ms;
    }
    memcpy(out,v,sizeof(v)); return true;
}

void omni_dsp_volume_probe_poll(uint32_t now,bool playback_running,bool allowed)
{
    if(!query.initialized) {
        /* Software-only initialization also records native desired state while
         * idle or after an eventual UART-claim failure. */
        omni_dsp_volume_io ops={NULL,transmit,receive,tx_complete};
        (void)omni_dsp_volume_init(&query,ops);
    }
    desired_snapshot();
    if(stage==2u) { gain_poll(now,playback_running && allowed); return; }
    if(stage==1u && (!playback_running || !allowed)) {
        omni_dsp_volume_cancel(&query); ++cancellations; finish(now); return;
    }
    if(!stage) {
        if(!playback_running || !allowed) { playback_stable=false; return; }
        /* Host endpoint setup can briefly open/close playback repeatedly.
         * Reserve the single query attempt for a continuously running stream.
         * Unsigned subtraction also handles the millisecond counter wrapping. */
        if(!playback_stable) {
            playback_stable_since=now; playback_stable=true; return;
        }
        if((uint32_t)(now-playback_stable_since)<500u) return;
        /* Claim exactly once. Existing start resets only the local UART/its
         * RX ring and selects recovered TX/RX pins; it does not pulse a peer
         * GPIO. Refuse an already owned transport rather than interrupt it. */
        if(!omni_control_uart_start(3u,&byte_io)) {
            ++start_failures; query.phase=OMNI_DSP_VOLUME_FAILED;
            query.error=OMNI_DSP_VOLUME_IO_ERROR; finish(now); return;
        }
        uart_owned=true;
        if(!omni_dsp_volume_begin(&query,now)) {
            query.phase=OMNI_DSP_VOLUME_FAILED;
            query.error=OMNI_DSP_VOLUME_IO_ERROR; finish(now); return;
        }
        stage=1u;
    }
    omni_control_uart_stats(3u,uart_stats);
    if(uart_stats[5]) {
        query.phase=OMNI_DSP_VOLUME_FAILED; query.error=OMNI_DSP_VOLUME_IO_ERROR;
        finish(now); return;
    }
    omni_dsp_volume_poll(&query,now);
    if(query.phase==OMNI_DSP_VOLUME_COMPLETE || query.phase==OMNI_DSP_VOLUME_FAILED ||
       query.phase==OMNI_DSP_VOLUME_CANCELED) finish(now);
}

bool omni_dsp_volume_probe_status(unsigned page,uint32_t out[15])
{
    if(!out || page>2u) return false;
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    uint32_t v[15]={1u,page,stage};
    if(!page) {
        v[3]=(uint32_t)query.phase; v[4]=(uint32_t)query.error;
        v[5]=(uint32_t)query.mode_valid|((uint32_t)query.levels_valid<<1);
        v[6]=query.mode;
        v[7]=(uint32_t)query.levels[0]|((uint32_t)query.levels[1]<<8)|
             ((uint32_t)query.levels[2]<<16)|((uint32_t)query.levels[3]<<24);
        v[8]=(uint32_t)(int32_t)query.desired_db; v[9]=query.desired_muted;
        v[10]=query.desired_revision; v[11]=query.started_ms;
        v[12]=query.mode_received_ms; v[13]=query.levels_received_ms; v[14]=finished_ms;
    } else if(page==1u) {
        v[3]=query.tx_bytes; v[4]=query.rx_bytes; v[5]=query.ack_frames;
        v[6]=query.unexpected_frames; v[7]=query.peer_status;
        v[8]=query.parser.frames; v[9]=query.parser.ignored;
        v[10]=query.parser.malformed; v[11]=query.parser.expired;
        v[12]=query.parser.used; v[13]=query.parser.expected;
        v[14]=(uint32_t)query.query_index|((uint32_t)query.tx_offset<<8)|
              ((uint32_t)query.got_ack<<16)|((uint32_t)query.got_reply<<24);
    } else {
        if(uart_owned) omni_control_uart_stats(3u,uart_stats);
        memcpy(v+3,uart_stats,sizeof(uart_stats));
        v[9]=(uint32_t)uart_owned; v[10]=start_failures; v[11]=cancellations;
        v[12]=drain_polls; v[13]=last_fifo; v[14]=last_stat;
    }
    memcpy(out,v,sizeof(v)); __set_PRIMASK(mask); return true;
}
