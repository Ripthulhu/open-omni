"""Execute the real main-loop playback service with bounded lifecycle fixtures.

No hardware. Verifies failed DMA quiescence and pending boot metadata prevent
clock/transport startup; a later explicit stream close permits a fresh attempt.
"""
import json
from pathlib import Path
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parents[1] / 'mcu1-source/src/diagnostic_usb.c'
STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "audio_format.h"
enum { OMNI_ACK_WAITING,OMNI_ACK_WRITTEN,OMNI_ACK_ALREADY_VALID };
enum { OMNI_AUDIO_MODE_LOCAL_COMPLETE=2,OMNI_AUDIO_MODE_FAILED,OMNI_AUDIO_MODE_CANCELED };
typedef enum {OMNI_AUDIO_MODE_IO_ERROR=-1,OMNI_AUDIO_MODE_IO_PENDING=0,OMNI_AUDIO_MODE_IO_COMPLETE=1} omni_audio_mode_io_t;
typedef struct {unsigned state;} omni_audio_mode_t;
typedef struct {void *context; int (*write)(void);int (*tx_status)(void);
 bool (*pins)(void);bool (*configure_begin)(void);int (*configure_poll)(void);} omni_audio_mode_ops_t;
static omni_audio_mode_t pb_mode;
static omni_audio_format pb_format;
static bool pb_stop_fault;
static unsigned am_rx_parser;
static unsigned pb_state,pb_fails,pb_clock_attempts,am_src_usb,omni_am_io;
static unsigned configuration=1,recovery_state,boot_ack_status,alternate=1;
static unsigned resets,clocks,starts,teardowns,stops,dma_stops,acks,last_rate;
static unsigned desired_rate=48000,desired_bits=16,desired_epoch=1;
static bool quiesced,rx_ok=true;
static unsigned mic_alternate,capture_only,mic_starts;
static bool microphone_started;
static uint32_t omni_dma_desc[64];
#define OMNI_MICROPHONE_AS_INTERFACE 3
static int audio_probe_alternate(unsigned i){assert(i==3);return (int)mic_alternate;}
static void usb_audio_ring_capture_only(int on){capture_only=(unsigned)on;}
static bool omni_microphone_start(uint32_t *p,uint32_t rate){assert(p==omni_dma_desc+16);(void)rate;++mic_starts;return true;}
static bool omni_microphone_stop(void){return true;}

static omni_audio_mode_io_t stop_result=OMNI_AUDIO_MODE_IO_PENDING;
static uint32_t omni_ui_milliseconds(void){return 100;}
static bool audio_probe_playback_format(omni_audio_format *out)
{assert(omni_audio_format_make(out,desired_rate,desired_bits,desired_epoch));return alternate!=0;}
static bool omni_dsp_capture_probe_busy(void){return false;}
static bool omni_dsp_probe_busy(void){return false;}
static bool audio_probe_playback_failed(void){return false;}
static bool omni_native_gain_release(uint32_t now){(void)now;return true;}
static bool usb_audio_ring_reset(void){++resets;return quiesced;}
static bool bring_up_local_ready(void){++clocks;++pb_clock_attempts;return true;}
static int am_write(void){return 0;}
static int am_tx_status(void){return 0;}
static bool am_pins_route(void){return true;}
static bool am_txring_begin(void){return true;}
static int am_txring_poll(void){return 0;}
static bool omni_control_uart_start(unsigned p,unsigned *io){(void)io;assert(p==3);++starts;return true;}
static void omni_control_uart_stop(unsigned p){assert(p==3);++stops;}
static bool omni_audio_mode_init(omni_audio_mode_t *s,const omni_audio_mode_ops_t *ops)
{(void)ops;s->state=0;return true;}
static bool omni_audio_mode_begin(omni_audio_mode_t *s,unsigned rate,uint32_t now)
{(void)s;(void)now;assert(rate==desired_rate);last_rate=rate;return true;}
static bool omni_audio_mode_require_ack(omni_audio_mode_t *s){(void)s;++acks;return true;}
static bool omni_link_init(unsigned *p,uint32_t ms){(void)p;assert(ms==100);return true;}
static bool playback_receive(uint32_t now){(void)now;return rx_ok;}
static unsigned failure_records,last_failure_reason;
static void playback_failure_record(uint32_t now,uint32_t reason)
{(void)now;++failure_records;last_failure_reason=reason;}
static omni_audio_mode_io_t omni_audio_mode_quiesce(omni_audio_mode_t *s,uint32_t now)
{(void)s;(void)now;return stop_result;}
static void omni_audio_mode_poll(omni_audio_mode_t *s,uint32_t now){(void)s;(void)now;}
static void playback_teardown(void){++teardowns;pb_state=0;}
static void am_txring_teardown(void){++dma_stops;}
static void usb_audio_ring_stop(void){++stops;}
static unsigned usb_audio_ring_fault(void){return 0;}
static void usb_audio_ring_clock_servo(void){}
'''
TEST = r'''
int main(void)
{
    boot_ack_status=OMNI_ACK_WRITTEN;quiesced=true;alternate=0;mic_alternate=1;
    playback_service();assert(pb_state==1 && capture_only);
    pb_mode.state=OMNI_AUDIO_MODE_LOCAL_COMPLETE;
    playback_service();assert(pb_state==2 && mic_starts==1);
    mic_alternate=0;playback_service();assert(!pb_state);
    resets=clocks=starts=teardowns=stops=dma_stops=acks=0;
    microphone_started=false;alternate=1;
    /* The USB host can select its streaming alternate before startup metadata
     * programming. No clock/UART/DMA work may precede the successful ACK. */
    boot_ack_status=OMNI_ACK_WAITING;quiesced=true;
    playback_service();assert(!resets&&!clocks&&!starts&&!pb_state);
    boot_ack_status=OMNI_ACK_WRITTEN;quiesced=false;
    playback_service();assert(resets==1&&pb_state==3&&pb_fails==1&&!clocks&&!starts);
    playback_service();assert(resets==1&&!clocks&&!starts); /* no blind retry */
    alternate=0;++desired_epoch;playback_service();assert(teardowns==1&&!pb_state);
    quiesced=true;alternate=1;++desired_epoch;playback_service();
    assert(resets==2&&clocks==1&&starts==1&&pb_state==1&&am_src_usb==1);
    assert(acks==1&&last_rate==48000&&pb_format.epoch==desired_epoch);
    recovery_state=1;playback_service();assert(teardowns==1&&pb_state==4&&dma_stops==1);
    playback_service();assert(teardowns==1&&pb_state==4); /* UART remains owned. */
    stop_result=OMNI_AUDIO_MODE_IO_COMPLETE;playback_service();assert(teardowns==2&&!pb_state);
    assert(pb_fails==1);
    recovery_state=0;playback_service();assert(pb_state==1&&last_rate==48000);
    pb_mode.state=OMNI_AUDIO_MODE_LOCAL_COMPLETE;playback_service();assert(pb_state==2);
    unsigned before=starts;
    desired_rate=96000;desired_bits=24;++desired_epoch;
    playback_service();assert(!pb_state&&starts==before); /* Tear down before reconfigure. */
    playback_service();assert(pb_state==1&&starts==before+1&&last_rate==96000);
    assert(pb_format.frame_bytes==6&&pb_format.max_packet==582&&pb_format.frames_per_ms==96);
    /* Missed close/reopen with the same rate and bit depth still changes epoch. */
    ++desired_epoch;stop_result=OMNI_AUDIO_MODE_IO_PENDING;
    playback_service();assert(pb_state==4&&starts==before+1);
    ++desired_epoch;playback_service();assert(pb_state==4&&starts==before+1);
    stop_result=OMNI_AUDIO_MODE_IO_COMPLETE;playback_service();assert(!pb_state);
    playback_service();assert(pb_state==1&&pb_format.epoch==desired_epoch&&starts==before+2);
    /* Failed handshake is latched after its UART drains, never auto-retried. */
    pb_mode.state=OMNI_AUDIO_MODE_FAILED;playback_service();assert(pb_state==4&&pb_stop_fault);
    playback_service();assert(pb_state==3&&pb_fails==2);
    before=starts;playback_service();assert(pb_state==3&&starts==before);
    ++desired_epoch;playback_service();assert(!pb_state&&starts==before);
    playback_service();assert(pb_state==1&&starts==before+1);
    /* A receive transport error takes the same bounded cleanup/fault path. */
    rx_ok=false;playback_service();assert(pb_state==4&&pb_stop_fault&&pb_fails==3);
    playback_service();assert(pb_state==3&&pb_fails==3);
    /* An old stream's receive fault must not latch onto a newer open while
     * its UART drains; same-epoch failure above deliberately remains latched. */
    rx_ok=true;++desired_epoch;playback_service();playback_service();assert(pb_state==1);
    rx_ok=false;playback_service();assert(pb_state==4&&pb_stop_fault&&pb_fails==4);
    ++desired_epoch;before=starts;playback_service();assert(!pb_state&&starts==before);
    rx_ok=true;playback_service();assert(pb_state==1&&starts==before+1);
    /* A hard drain failure still blocks the new epoch: ownership is unknown. */
    ++desired_epoch;playback_service();assert(pb_state==4);
    stop_result=OMNI_AUDIO_MODE_IO_ERROR;playback_service();assert(pb_state==3&&pb_fails==5);
    before=starts;playback_service();assert(pb_state==3&&starts==before);
    assert(failure_records==4&&last_failure_reason==5);
    puts("playback: boot ACK, DMA quiescence, 48/96 epochs, missed reopen, UART drain, epoch-scoped faults and hard ownership latch passed");
    return 0;
}
'''


def main():
    source = SOURCE.read_text()
    body = 'static void playback_service(void)' + source.split('static void playback_service(void)', 1)[1].split('static void sof_capture_service(void)', 1)[0]
    with tempfile.TemporaryDirectory(prefix='omni-playback-') as directory:
        root = Path(directory)
        c, exe = root / 'test.c', root / 'test'
        c.write_text(STUBS + body + TEST)
        subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wconversion',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                        '-I',str(SOURCE.parents[1]/'include'),
                        str(c), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
        print(json.dumps({'passed': True, 'result': result.stdout.strip(), 'hardware_access': False}))


if __name__ == '__main__':
    main()
