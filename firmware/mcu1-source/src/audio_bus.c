#include "audio_bus.h"

#include <string.h>

static bool byte_range(uintptr_t address, size_t length)
{
    return length <= UINTPTR_MAX - address;
}

bool omni_audio_pcm_to_i2s32(uint8_t *destination, size_t capacity,
                           const uint8_t *source, size_t source_length,
                           unsigned source_sample_bytes, size_t *written)
{
    if (written == NULL) return false;
    *written = 0;
    if (source_sample_bytes != 2u && source_sample_bytes != 3u) return false;
    if (source_length % source_sample_bytes != 0u) return false;
    size_t samples = source_length / source_sample_bytes;
    if (samples > SIZE_MAX / 4u) return false;
    size_t output_length = samples * 4u;
    if (capacity < output_length) return false;
    if (source_length == 0u) return true;
    if (source == NULL || destination == NULL) return false;
    uintptr_t src = (uintptr_t)source;
    uintptr_t dst = (uintptr_t)destination;
    if (!byte_range(src, source_length) || !byte_range(dst, output_length)) return false;
    if (src < dst + output_length && dst < src + source_length) return false;
    for (size_t sample = 0; sample < samples; ++sample) {
        size_t input = sample * source_sample_bytes;
        size_t output = sample * 4u;
        unsigned padding = 4u - source_sample_bytes;
        for (unsigned byte = 0; byte < padding; ++byte) destination[output + byte] = 0;
        for (unsigned byte = 0; byte < source_sample_bytes; ++byte)
            destination[output + padding + byte] = source[input + byte];
    }
    *written = output_length;
    return true;
}

typedef struct { uint32_t start; uint32_t length; } ram_region_t;

static bool region_valid(ram_region_t region, uint32_t alignment)
{
    return region.start >= UINT32_C(0x20000000) &&
           region.start < UINT32_C(0x20030000) &&
           region.length <= UINT32_C(0x20030000) - region.start &&
           region.start % alignment == 0u;
}

bool omni_audio_bus_plan(omni_audio_bus_plan_t *plan, uint32_t sample_rate,
                         uint32_t rx_descriptors, uint32_t tx_descriptors,
                         uint32_t rx_buffer, uint32_t tx_buffer)
{
    if (plan == NULL || (sample_rate != 48000u && sample_rate != 96000u)) return false;
    uint32_t frames = sample_rate / 1000u;
    uint32_t rx_bytes = frames * 2u;
    uint32_t tx_bytes = frames * 8u;
    ram_region_t regions[] = {
        {rx_descriptors, 16u * OMNI_AUDIO_BUS_DESCRIPTORS},
        {tx_descriptors, 16u * OMNI_AUDIO_BUS_DESCRIPTORS},
        {rx_buffer, rx_bytes * OMNI_AUDIO_BUS_DESCRIPTORS},
        {tx_buffer, tx_bytes * OMNI_AUDIO_BUS_DESCRIPTORS},
    };
    const uint32_t alignments[] = {16u, 16u, 2u, 4u};
    for (unsigned i = 0; i < 4u; ++i) {
        if (!region_valid(regions[i], alignments[i])) return false;
        for (unsigned j = 0; j < i; ++j) {
            if (regions[i].start < regions[j].start + regions[j].length &&
                regions[j].start < regions[i].start + regions[i].length) return false;
        }
    }
    omni_audio_bus_plan_t result;
    memset(&result, 0, sizeof(result));
    result.sample_rate = sample_rate;
    result.clock_hz = 49152000u;
    result.rx_cfg1 = UINT32_C(0x000f0430);
    result.tx_cfg1 = UINT32_C(0x001f0000);
    result.cfg2 = 63u;
    result.rx_divider_register = sample_rate == 96000u ? 7u : 15u;
    result.rx_block_bytes = rx_bytes;
    result.tx_block_bytes = tx_bytes;
    for (uint32_t i = 0; i < OMNI_AUDIO_BUS_DESCRIPTORS; ++i) {
        uint32_t next = (i + 1u) % OMNI_AUDIO_BUS_DESCRIPTORS;
        result.rx[i].transfer = ((frames - 1u) << 16) | UINT32_C(0x4123);
        result.rx[i].source_end = UINT32_C(0x40086e30);
        result.rx[i].destination_end = rx_buffer + (i + 1u) * rx_bytes - 2u;
        result.rx[i].next = rx_descriptors + next * 16u;
        result.tx[i].transfer = ((frames * 2u - 1u) << 16) | UINT32_C(0x1213);
        result.tx[i].source_end = tx_buffer + (i + 1u) * tx_bytes - 4u;
        result.tx[i].destination_end = UINT32_C(0x40088e20);
        result.tx[i].next = tx_descriptors + next * 16u;
    }
    *plan = result;
    return true;
}
