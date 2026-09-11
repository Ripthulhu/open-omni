#include "audio_bus.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void pcm_vectors(void)
{
    /* First samples match execution of stock 0x2fb9c in stock-audio-emulation.json;
     * following values exercise signed extrema and -1 without signed shifts. */
    const uint8_t pcm16[] = {0x34,0x12, 0xcc,0xed, 0,0x80, 0xff,0x7f, 0xff,0xff, 0,0};
    const uint8_t expected16[] = {0,0,0x34,0x12, 0,0,0xcc,0xed, 0,0,0,0x80,
                                 0,0,0xff,0x7f, 0,0,0xff,0xff, 0,0,0,0};
    const uint8_t pcm24[] = {0x56,0x34,0x12, 0xaa,0xcb,0xed, 0,0,0x80,
                             0xff,0xff,0x7f, 0xff,0xff,0xff, 0,0,0};
    const uint8_t expected24[] = {0,0x56,0x34,0x12, 0,0xaa,0xcb,0xed, 0,0,0,0x80,
                                 0,0xff,0xff,0x7f, 0,0xff,0xff,0xff, 0,0,0,0};
    uint8_t output[25]; size_t written;
    memset(output, 0xa5, sizeof(output));
    assert(omni_audio_pcm_to_i2s32(output, 24, pcm16, sizeof(pcm16), 2, &written));
    assert(written == 24 && memcmp(output, expected16, 24) == 0 && output[24] == 0xa5);
    assert(omni_audio_pcm_to_i2s32(output, 24, pcm24, sizeof(pcm24), 3, &written));
    assert(written == 24 && memcmp(output, expected24, 24) == 0 && output[24] == 0xa5);
    memset(output, 0xa5, sizeof(output));
    assert(!omni_audio_pcm_to_i2s32(output, 23, pcm16, sizeof(pcm16), 2, &written));
    assert(written == 0 && output[0] == 0xa5);
    assert(!omni_audio_pcm_to_i2s32(output, 24, pcm24, 17, 3, &written));
    assert(!omni_audio_pcm_to_i2s32(output, 24, pcm16, sizeof(pcm16), 4, &written));
    assert(!omni_audio_pcm_to_i2s32(NULL, 24, pcm16, sizeof(pcm16), 2, &written));
    assert(!omni_audio_pcm_to_i2s32(output, 24, NULL, 2, 2, &written));
    assert(!omni_audio_pcm_to_i2s32(output, 24, pcm16, 2, 2, NULL));
    assert(!omni_audio_pcm_to_i2s32(output, sizeof(output), output + 1, 2, 2, &written));
    assert(!omni_audio_pcm_to_i2s32(output + 1, 24, output, 2, 2, &written));
    assert(!omni_audio_pcm_to_i2s32(output, SIZE_MAX, pcm16, SIZE_MAX - 1u, 2, &written));
    assert(omni_audio_pcm_to_i2s32(NULL, 0, NULL, 0, 2, &written) && written == 0);
    for (unsigned i = 0; i < sizeof(output); ++i) assert(output[i] == 0xa5);
}

static void stock_plans(void)
{
    omni_audio_bus_plan_t plan;
    /* Exact first/last stock descriptors, checked against captured RAM and the
     * independent emulator executing stock DMA_CreateDescriptor at0x2ffce. */
    const omni_audio_dma_descriptor_t rx96_first = {0x005f4123,0x40086e30,0x2000bc32,0x200053a0};
    const omni_audio_dma_descriptor_t rx96_last = {0x005f4123,0x40086e30,0x2000c2f2,0x20005390};
    const omni_audio_dma_descriptor_t tx96_first = {0x00bf1213,0x2000d86c,0x40088e20,0x20005440};
    const omni_audio_dma_descriptor_t tx96_last = {0x00bf1213,0x2000f36c,0x40088e20,0x20005430};
    assert(omni_audio_bus_plan(&plan, 96000, 0x20005390, 0x20005430, 0x2000bb74, 0x2000d570));
    assert(memcmp(&plan.rx[0], &rx96_first, 16) == 0);
    assert(memcmp(&plan.rx[9], &rx96_last, 16) == 0);
    assert(memcmp(&plan.tx[0], &tx96_first, 16) == 0);
    assert(memcmp(&plan.tx[9], &tx96_last, 16) == 0);
    assert(plan.rx_cfg1 == 0xf0430 && plan.tx_cfg1 == 0x1f0000 && plan.cfg2 == 63);
    assert(plan.rx_divider_register == 7 && plan.rx_block_bytes == 192 && plan.tx_block_bytes == 768);
    const omni_audio_dma_descriptor_t rx48_first = {0x002f4123,0x40086e30,0x2000b812,0x200053a0};
    const omni_audio_dma_descriptor_t rx48_last = {0x002f4123,0x40086e30,0x2000bb72,0x20005390};
    const omni_audio_dma_descriptor_t tx48_first = {0x005f1213,0x2000c7ec,0x40088e20,0x20005440};
    const omni_audio_dma_descriptor_t tx48_last = {0x005f1213,0x2000d56c,0x40088e20,0x20005430};
    assert(omni_audio_bus_plan(&plan, 48000, 0x20005390, 0x20005430, 0x2000b7b4, 0x2000c670));
    assert(memcmp(&plan.rx[0], &rx48_first, 16) == 0);
    assert(memcmp(&plan.rx[9], &rx48_last, 16) == 0);
    assert(memcmp(&plan.tx[0], &tx48_first, 16) == 0);
    assert(memcmp(&plan.tx[9], &tx48_last, 16) == 0);
    assert(plan.rx_divider_register == 15 && plan.rx_block_bytes == 96 && plan.tx_block_bytes == 384);
    for (unsigned i = 0; i < 10; ++i) {
        assert(plan.rx[i].source_end == 0x40086e30 && plan.tx[i].destination_end == 0x40088e20);
        assert(plan.rx[i].destination_end >= 0x2000b7b4 && plan.rx[i].destination_end < 0x2000bb74);
        assert(plan.tx[i].source_end >= 0x2000c670 && plan.tx[i].source_end < 0x2000d570);
    }
}

static void plan_rejections(void)
{
    omni_audio_bus_plan_t plan, original;
    memset(&plan, 0xa5, sizeof(plan)); original = plan;
    assert(!omni_audio_bus_plan(NULL, 48000, 0x20000000, 0x20000100, 0x20000200, 0x20001000));
    assert(!omni_audio_bus_plan(&plan, 44100, 0x20000000, 0x20000100, 0x20000200, 0x20001000));
    assert(!omni_audio_bus_plan(&plan, UINT32_MAX, 0x20000000, 0x20000100, 0x20000200, 0x20001000));
    assert(!omni_audio_bus_plan(&plan, 48000, 0x20000004, 0x20000100, 0x20000200, 0x20001000));
    assert(!omni_audio_bus_plan(&plan, 48000, 0x20000000, 0x20000104, 0x20000200, 0x20001000));
    assert(!omni_audio_bus_plan(&plan, 48000, 0x20000000, 0x20000100, 0x20000201, 0x20001000));
    assert(!omni_audio_bus_plan(&plan, 48000, 0x20000000, 0x20000100, 0x20000200, 0x20001002));
    assert(!omni_audio_bus_plan(&plan, 48000, 0x20000000, 0x20000090, 0x20000200, 0x20001000));
    assert(!omni_audio_bus_plan(&plan, 48000, 0x20000000, 0x20000100, 0x20000200, 0x20000200));
    assert(!omni_audio_bus_plan(&plan, 48000, 0x20000000, 0x20000100, 0x20000010, 0x20001000));
    assert(!omni_audio_bus_plan(&plan, 48000, 0x20000000, 0x20000100, 0x20000200, 0x2002fff0));
    assert(!omni_audio_bus_plan(&plan, 48000, 0x1ffffff0, 0x20000100, 0x20000200, 0x20001000));
    assert(!omni_audio_bus_plan(&plan, 48000, 0xfffffff0, 0x20000100, 0x20000200, 0x20001000));
    assert(memcmp(&plan, &original, sizeof(plan)) == 0);
    assert(omni_audio_bus_plan(&plan, 96000, 0x20000000, 0x200000a0, 0x20000140, 0x2002e200));
    assert(plan.tx[9].source_end == 0x2002fffc);
}

int main(void)
{
    _Static_assert(sizeof(omni_audio_dma_descriptor_t) == 16, "LPC DMA descriptor layout");
    pcm_vectors(); stock_plans(); plan_rejections();
    puts("audio bus stock vectors and bounds: PASS");
    return 0;
}
