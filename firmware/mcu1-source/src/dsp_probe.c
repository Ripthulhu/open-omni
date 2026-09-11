#include "dsp_probe.h"
#include "dsp_link.h"
#include "control_uart.h"
#include "native_gain_adapter.h"
#include "fsl_device_registers.h"
#include "fsl_clock.h"
#include <string.h>

static omni_dsp_link link;
static volatile uint32_t token, stage;
static uint32_t prepare_ms, before_pins[6];
static uint32_t uart_stats[6];
static bool uart_owned;
bool omni_dsp_probe_busy(void) { return stage == 1U || stage == 2U || stage == 4U; }
static void finish(void)
{
    if (uart_owned) {
        omni_control_uart_stop(3);
        omni_control_uart_stats(3, uart_stats);
        uart_owned = false;
    }
    __DMB(); stage = 3;
}
static void prepare_pins(void)
{
    /* Proven unconditional stock0x23f34 immediately precedes UART3 init.
     * Roles remain unproven; HIGH can release a held peer reset. Never pulse
     * LOW. Preserve the previous digital state in the diagnostic trace. */
    CLOCK_EnableClock(kCLOCK_Iocon);
    CLOCK_EnableClock(kCLOCK_Gpio0); CLOCK_EnableClock(kCLOCK_Gpio1);
    before_pins[0]=GPIO->PIN[0]; before_pins[1]=GPIO->PIN[1];
    before_pins[2]=GPIO->DIR[0]; before_pins[3]=GPIO->DIR[1];
    before_pins[4]=*(volatile const uint32_t *)0x4000109cU;
    before_pins[5]=*(volatile const uint32_t *)0x40001044U;
    /* Stock writes0x4100; bit14 is reserved in UM11126rev2.8. Express the
     * documented function0/digital-enable settings, leaving reserved bits0. */
    *(volatile uint32_t *)0x4000109cU=0x100U;
    *(volatile uint32_t *)0x40001044U=0x100U;
    GPIO->SET[1]=1UL<<7;
    GPIO->DIRSET[1]=1UL<<7;
    GPIO->DIRCLR[0]=1UL<<17;
}
bool omni_dsp_probe_request(uint32_t requested)
{
    if (!requested) return false;
    if (stage) return requested == token;
    token = requested; stage = 1; return true;
}
void omni_dsp_probe_poll(uint32_t now_ms, bool allowed)
{
    if (!stage || stage == 3U) return;
    /* Drain native volume/battery work before the diagnostic claims UART3.
     * Finish this cooperative yield even when audio cancels the request. */
    if (stage == 1U && !omni_native_gain_release(now_ms)) return;
    if (!allowed) {
        if (stage == 2U) omni_dsp_link_cancel(&link);
        else link.phase = OMNI_DSP_CANCELLED;
        finish(); return;
    }
    if (stage == 1U) {
        prepare_pins(); prepare_ms=now_ms; stage=4; return;
    }
    if (stage == 4U) {
        /* Own inquiry policy, not a stock delay: allow a released peer to
         * settle without blocking USB/UI or making repeated queries. */
        if ((uint32_t)(now_ms-prepare_ms)<2000U) return;
        mcu2_link_io io;
        if (!omni_control_uart_start(3, &io)) {
            link.phase = OMNI_DSP_IO_ERROR; finish(); return;
        }
        uart_owned = true;
        if (!omni_dsp_link_init(&link, io) ||
            !omni_dsp_link_start(&link, now_ms)) {
            link.phase = OMNI_DSP_IO_ERROR;
            finish(); return;
        }
        stage = 2;
    }
    omni_dsp_link_poll(&link, now_ms);
    if (link.phase != OMNI_DSP_RUNNING) finish();
}
void omni_dsp_probe_status(uint8_t out[60])
{
    uint32_t words[15] = {1};
    uint32_t mask = __get_PRIMASK(); __disable_irq();
    words[1] = token; words[2] = stage; words[3] = (uint32_t)link.phase;
    words[4] = link.tx_bytes; words[5] = link.rx_bytes;
    words[6] = link.received_frames; words[7] = link.rejected_frames;
    words[8] = (uint32_t)link.reply[0] | ((uint32_t)link.reply[1] << 8) |
               ((uint32_t)link.reply[2] << 16);
    if (uart_owned) omni_control_uart_stats(3, words + 9);
    else memcpy(words + 9, uart_stats, sizeof(uart_stats));
    words[14] |= (uint32_t)link.parser.used << 16;
    __set_PRIMASK(mask);
    for (unsigned w = 0; w < 15U; ++w)
        for (unsigned b = 0; b < 4U; ++b) out[w * 4U + b] = (uint8_t)(words[w] >> (b * 8U));
}
void omni_dsp_probe_trace(uint8_t out[60])
{
    uint32_t words[15] = {1};
    uint32_t mask = __get_PRIMASK(); __disable_irq();
    words[1] = link.first_unexpected_length;
    words[4] = link.parser.malformed; words[5] = link.parser.expired;
    words[6] = link.parser.ignored; words[7] = link.parser.frames;
    words[8] = link.parser.used;
    memcpy(words+9,before_pins,sizeof(before_pins));
    /* MCU1 is little endian; first eight raw bytes retain wire order. */
    memcpy(words + 2, link.first_unexpected, 8);
    __set_PRIMASK(mask);
    memcpy(out, words, sizeof(words));
}
void omni_dsp_probe_reply(uint8_t out[60])
{
    uint32_t mask = __get_PRIMASK(); __disable_irq();
    uint32_t header[3] = {1, link.reply_length, (uint32_t)link.phase};
    memset(out, 0, 60); memcpy(out, header, sizeof(header));
    memcpy(out + 12, link.reply, 3); memcpy(out + 15, link.reply_tail, 6);
    __set_PRIMASK(mask);
}
