#ifndef OMNI_AUDIO_FORMAT_H
#define OMNI_AUDIO_FORMAT_H
#include <stdbool.h>
#include <stdint.h>

/* A complete stream format is published/snapshotted together. Epoch changes
 * on each open/close/rate change, including changes between main-loop polls.
 * USB and DMA owners latch their own copy until their transfers are quiesced. */
typedef struct {
    uint32_t sample_rate;
    uint32_t epoch;
    uint16_t max_packet;
    uint8_t sample_bits;
    uint8_t frame_bytes;
    uint8_t frames_per_ms;
    uint8_t alternate;
} omni_audio_format;

static inline bool omni_audio_format_make(omni_audio_format *out,
                                          uint32_t sample_rate,
                                          unsigned sample_bits,
                                          uint32_t epoch)
{
    if(!out || (sample_rate!=48000u && sample_rate!=96000u) ||
       (sample_bits!=16u && sample_bits!=24u)) return false;
    omni_audio_format value={0};
    value.sample_rate=sample_rate;value.epoch=epoch;
    value.sample_bits=(uint8_t)sample_bits;
    value.frame_bytes=(uint8_t)(sample_bits/8u*2u);
    value.frames_per_ms=(uint8_t)(sample_rate/1000u);
    value.max_packet=(uint16_t)(((unsigned)value.frames_per_ms+1u)*value.frame_bytes);
    value.alternate=(uint8_t)(sample_bits==16u?1u:2u);
    *out=value;return true;
}

static inline bool omni_audio_format_valid(const omni_audio_format *format)
{
    omni_audio_format expected;
    return format && omni_audio_format_make(&expected,format->sample_rate,
                                            format->sample_bits,format->epoch) &&
        expected.max_packet==format->max_packet &&
        expected.frame_bytes==format->frame_bytes &&
        expected.frames_per_ms==format->frames_per_ms &&
        expected.alternate==format->alternate;
}
#endif
