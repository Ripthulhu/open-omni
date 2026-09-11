#ifndef OMNI_AUDIO_QUEUE_H
#define OMNI_AUDIO_QUEUE_H
#include <stdatomic.h>
#include <stdint.h>
#include "audio_format.h"

#define OMNI_AUDIO_QUEUE_FRAMES 2048u
/* One USB producer, one higher-priority DMA consumer. Publish complete frames
 * with release/acquire ordering. init/prime require BOTH contexts quiesced. */
typedef struct {
    uint32_t pcm[OMNI_AUDIO_QUEUE_FRAMES*2u]; /* stereo left-aligned I2S words */
    omni_audio_format format;               /* immutable until both IRQs stop */
    atomic_uint_least32_t written, read;
    uint32_t accepted, rejected, overflows, consumed, silence, underruns;
} omni_audio_queue_t;
void omni_audio_queue_init(omni_audio_queue_t *q, uint32_t silence_prime);
/* Packed little-endian PCM16/24 stereo; 2048 frames uses 16KiB independent of
 * format. At 96k a 1024-frame prime leaves 1024 frames of producer headroom.
 * Invalid format/prime/null queue returns false with no mutation. */
bool omni_audio_queue_init_format(omni_audio_queue_t *q, uint32_t silence_prime,
                                   const omni_audio_format *format);
uint32_t omni_audio_queue_push(omni_audio_queue_t *q, const uint8_t *pcm, uint32_t bytes);
uint32_t omni_audio_queue_render(omni_audio_queue_t *q, uint32_t *slots, uint32_t frames);
uint32_t omni_audio_queue_fill(const omni_audio_queue_t *q);
#endif
