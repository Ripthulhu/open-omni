#ifndef OMNI_AUDIO_BUS_H
#define OMNI_AUDIO_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OMNI_AUDIO_BUS_DESCRIPTORS 10u

typedef struct {
    uint32_t transfer;
    uint32_t source_end;
    uint32_t destination_end;
    uint32_t next;
} omni_audio_dma_descriptor_t;

typedef struct {
    uint32_t sample_rate;
    uint32_t clock_hz;
    uint32_t rx_cfg1;
    uint32_t tx_cfg1;
    uint32_t cfg2;
    uint32_t rx_divider_register;
    uint32_t rx_block_bytes;
    uint32_t tx_block_bytes;
    omni_audio_dma_descriptor_t rx[OMNI_AUDIO_BUS_DESCRIPTORS];
    omni_audio_dma_descriptor_t tx[OMNI_AUDIO_BUS_DESCRIPTORS];
} omni_audio_bus_plan_t;

/* Expand little-endian signed PCM16/PCM24 to left-aligned I2S32 samples.
 * Source and destination must not overlap. No writes on invalid input except
 * setting *written to zero. Empty input accepts null buffers. */
bool omni_audio_pcm_to_i2s32(uint8_t *destination, size_t capacity,
                           const uint8_t *source, size_t source_length,
                           unsigned source_sample_bytes, size_t *written);

/* Build, without touching hardware, the recovered LPC5528 I2S0 mono16 RX /
 * I2S2 stereo32 TX plan: ten 1 ms DMA blocks, channels 4 and 11 respectively.
 * Descriptor addresses require 16-byte alignment; buffers require 2/4-byte
 * alignment. All four regions must be disjoint and fit main SRAM
 * [0x20000000, 0x20030000). Caller owns allocation and DSP mode negotiation.
 * This does not authorize activating clocks, pins, DMA or the DSP.
 * Invalid arguments leave *plan unchanged. */
bool omni_audio_bus_plan(omni_audio_bus_plan_t *plan, uint32_t sample_rate,
                         uint32_t rx_descriptors, uint32_t tx_descriptors,
                         uint32_t rx_buffer, uint32_t tx_buffer);

#endif
