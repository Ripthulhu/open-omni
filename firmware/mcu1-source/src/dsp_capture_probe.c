#include "dsp_capture_probe.h"
#include "dsp_capture.h"
#include "control_uart.h"
#include "native_gain_adapter.h"
#include "fsl_device_registers.h"
#include <stddef.h>
#include <string.h>

static omni_dsp_capture capture;
static volatile uint32_t token, stage;
static uint32_t duration_ms, uart_stats[6];
static bool uart_owned;

bool omni_dsp_capture_probe_busy(void) { return stage == 1U || stage == 2U; }

bool omni_dsp_capture_probe_request(uint32_t requested, uint32_t duration)
{
    if (!requested || duration < OMNI_DSP_CAPTURE_MIN_MS || duration > OMNI_DSP_CAPTURE_MAX_MS)
        return false;
    if (stage && requested == token) return duration == duration_ms;
    if (stage == 1U || stage == 2U) return false;
    /* A new token explicitly replaces frozen evidence. Invalidate its parser,
     * counters and record count now, but leave the large inaccessible payload
     * array for main's capture_init() to clear before the next observation. */
    memset(&capture, 0, offsetof(omni_dsp_capture, records));
    memset(&capture.phase, 0, sizeof(capture) - offsetof(omni_dsp_capture, phase));
    memset(uart_stats, 0, sizeof(uart_stats));
    duration_ms = duration; token = requested; __DMB(); stage = 1; return true;
}

static void finish(void)
{
    if (uart_owned) {
        omni_control_uart_stop(3);
        omni_control_uart_stats(3, uart_stats);
        uart_owned = false;
    }
    __DMB(); stage = 3;
}

void omni_dsp_capture_probe_poll(uint32_t now_ms, bool allowed)
{
    if (!stage || stage == 3U) return;
    /* Complete any native DSP SET/readback before reserving UART3. Finish
     * this yield even after cancellation so native gain cannot remain held. */
    if (stage == 1U && !omni_native_gain_release(now_ms)) return;
    if (!allowed) {
        if (stage == 2U) { omni_dsp_capture_cancel(&capture, now_ms); finish(); }
        else { capture.phase = OMNI_DSP_CAPTURE_CANCELLED; __DMB(); stage = 3; }
        return;
    }
    if (stage == 1U) {
        mcu2_link_io io;
        /* Fixed UART3 pin/clock selection only. No peer GPIO preparation,
         * reset, status inquiry, menu coordination or audio-mode command. */
        if (!omni_control_uart_start(3, &io)) {
            capture.phase = OMNI_DSP_CAPTURE_IO_ERROR; finish(); return;
        }
        uart_owned = true;
        if (!omni_dsp_capture_init(&capture, io.context, io.rx) ||
            !omni_dsp_capture_begin(&capture, duration_ms, now_ms)) {
            capture.phase = OMNI_DSP_CAPTURE_IO_ERROR; finish(); return;
        }
        stage = 2;
    }
    omni_dsp_capture_poll(&capture, now_ms);
    if (capture.phase != OMNI_DSP_CAPTURE_RUNNING) finish();
}

bool omni_dsp_capture_probe_status(unsigned page, uint8_t out[60])
{
    if (!out || page > 1U) return false;
    uint32_t mask = __get_PRIMASK(); __disable_irq();
    uint32_t words[15] = {1, page, token, stage, (uint32_t)capture.phase};
    if (!page) {
        words[5] = duration_ms; words[6] = capture.started_ms; words[7] = capture.rx_bytes;
        words[8] = capture.stored_records; words[9] = capture.overflow_frames;
        words[10] = capture.received_frames; words[11] = capture.parser.malformed;
        words[12] = capture.parser.expired; words[13] = capture.parser.ignored;
        words[14] = capture.parser.used;
    } else {
        words[5] = capture.finished_ms; words[6] = capture.parser.expected;
        words[7] = (uint32_t)capture.io_result;
        if (stage == 2U) omni_control_uart_stats(3, words + 8);
        else memcpy(words + 8, uart_stats, sizeof(uart_stats));
    }
    __set_PRIMASK(mask);
    memcpy(out, words, sizeof(words)); return true;
}

bool omni_dsp_capture_probe_record(uint32_t requested, unsigned index, unsigned page,
                                   uint8_t out[60])
{
    omni_dsp_capture_record record;
    if (!out || !requested || requested != token || stage != 3U || page >= 6U ||
        !omni_dsp_capture_get_record(&capture, index, &record)) return false;
    unsigned offset = page * 40U;
    if (offset >= record.length) return false;
    uint32_t words[5] = {1, token, record.sequence, record.received_ms,
                        (uint32_t)record.length | ((uint32_t)offset << 16)};
    memset(out, 0, 60); memcpy(out, words, sizeof(words));
    unsigned count = (unsigned)record.length - offset;
    if (count > 40U) count = 40U;
    memcpy(out + 20, record.raw + offset, count); return true;
}
