#ifndef OMNI_USB_AUDIO_RING_H
#define OMNI_USB_AUDIO_RING_H
#include <stdbool.h>
#include <stdint.h>
#include "audio_format.h"
/* USB->2048 stereo-frame SPSC queue->DMA-owned A/B blocks of 1ms each.
 * No modulo hardware pointer, sample rate conversion or hard drift drop/dup.
 * Missing samples become counted silence; full packets are rejected when full.
 * DMA ownership faults stop playback until the stream closes/restarts. */
#define OMNI_USB_AUDIO_DMA_WORDS 96u
#define OMNI_USB_AUDIO_DMA_MAX_WORDS 192u
#define OMNI_USB_AUDIO_WORDS (2u*OMNI_USB_AUDIO_DMA_MAX_WORDS)
extern uint32_t omni_usb_audio[OMNI_USB_AUDIO_WORDS];
/* Quiesce DMA before start; activate immediately before clock/trigger enable. */
void usb_audio_ring_start(void);
/* Both producers/consumer must be quiesced. Invalid or still-active state is
 * refused without replacing geometry. Format stays immutable until stop. */
bool usb_audio_ring_start_format(const omni_audio_format *format);
uint32_t usb_audio_ring_dma_words(void);
void usb_audio_ring_dma_setup(uint32_t channel_descriptor[4]);
void usb_audio_ring_activate(void);
void usb_audio_ring_stop(void);
/* False retains a failed-stop fault: the caller must not reuse DMA storage. */
bool usb_audio_ring_reset(void);
/* Startup/fault interlock only. Runtime volume is applied by the DSP. */
void usb_audio_ring_gain_ready(int ready);
/* Independent USB1 source-bias target. Aligned mailbox, snapshotted once per
 * released DMA block; master loudness remains in the DSP. */
void usb_audio_ring_source_gain(unsigned q14);
void usb_audio_ring_write(const uint8_t *pcm,uint32_t bytes);
void usb_audio_ring_write_format(const uint8_t *pcm,uint32_t bytes,uint32_t epoch);
void usb_audio_ring_format_status(uint32_t out[15]);
void usb_audio_ring_observe_packet(uint32_t bytes,uint32_t usb_frame,uint32_t now_ms);
void usb_audio_ring_observe_packet_format(uint32_t bytes,uint32_t usb_frame,uint32_t now_ms,uint32_t epoch);
void usb_audio_ring_delivery_status(uint32_t out[15]);
void usb_audio_ring_clock_servo(void);
uint32_t usb_audio_ring_fault(void);
uint32_t usb_audio_ring_feedback(void); /*10.14 API,16.16 wire */
uint32_t usb_audio_ring_feedback_format(const omni_audio_format *format);
bool usb_audio_ring_set_force(uint32_t value); /*0 or nominal+/-1 frames,10.14*/
void usb_audio_ring_set_md_force(uint32_t md);
void usb_audio_ring_set_gains(int32_t kp,int32_t ki,int reset_i);
int32_t usb_audio_ring_kp(void);
int32_t usb_audio_ring_ki_q8(void); /* ABI v2: MD/(frame*s), NOT old KiQ8 */
int32_t usb_audio_ring_servo_i(void);
uint32_t usb_audio_ring_write_index(void); /* queue index in32bit words */
uint32_t usb_audio_ring_frames_written(void); /* USB frames while active */
uint32_t usb_audio_ring_frames_out(void); /* accepted USB, NOT consumption */
uint32_t usb_audio_ring_corrections(void); /* underrun+overflow events */
uint32_t usb_audio_ring_dups(void); /* silence frames, no duplicated PCM */
uint32_t usb_audio_ring_drops(void); /* rejected USB frames */
uint32_t usb_audio_ring_step(void);
uint32_t usb_audio_ring_gap_min(void); /* queue fill in32bit words */
uint32_t usb_audio_ring_gap_max(void);
uint32_t usb_audio_ring_gap_now(void);
void usb_audio_ring_status(uint32_t out[15]);
void usb_audio_ring_trace(uint8_t index,uint32_t out[15]);
#endif
