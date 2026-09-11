#include "dsp_capture.h"

#include <string.h>

static bool frozen(const omni_dsp_capture *capture)
{
    return capture != NULL && capture->initialized &&
           capture->phase >= OMNI_DSP_CAPTURE_COMPLETE &&
           capture->phase <= OMNI_DSP_CAPTURE_BAD_TIME;
}

static void freeze(omni_dsp_capture *capture, omni_dsp_capture_phase phase, uint32_t now_ms)
{
    capture->phase = phase;
    capture->finished_ms = now_ms;
}

static void increment(uint32_t *value)
{
    if (*value != UINT32_MAX) ++*value;
}

bool omni_dsp_capture_init(omni_dsp_capture *capture, void *context, int (*rx)(void *, uint8_t *))
{
    if (capture == NULL || rx == NULL) return false;
    memset(capture, 0, sizeof(*capture));
    capture->context = context;
    capture->rx = rx;
    capture->initialized = omni_link_init(&capture->parser, OMNI_DSP_CAPTURE_GAP_MS);
    return capture->initialized;
}

bool omni_dsp_capture_begin(omni_dsp_capture *capture, uint32_t duration_ms, uint32_t now_ms)
{
    if (capture == NULL || !capture->initialized || capture->rx == NULL ||
        capture->phase == OMNI_DSP_CAPTURE_RUNNING ||
        duration_ms < OMNI_DSP_CAPTURE_MIN_MS || duration_ms > OMNI_DSP_CAPTURE_MAX_MS)
        return false;
    if (!omni_link_init(&capture->parser, OMNI_DSP_CAPTURE_GAP_MS)) return false;
    memset(capture->records, 0, sizeof(capture->records));
    capture->started_ms = now_ms;
    capture->last_poll_ms = now_ms;
    capture->finished_ms = 0;
    capture->duration_ms = duration_ms;
    capture->rx_bytes = 0;
    capture->received_frames = 0;
    capture->overflow_frames = 0;
    capture->io_result = 0;
    capture->stored_records = 0;
    capture->phase = OMNI_DSP_CAPTURE_RUNNING;
    return true;
}

void omni_dsp_capture_fail(omni_dsp_capture *capture, int32_t reason, uint32_t now_ms)
{
    if (capture == NULL || !capture->initialized || capture->phase != OMNI_DSP_CAPTURE_RUNNING) return;
    capture->io_result = reason != 0 ? reason : -1;
    freeze(capture, OMNI_DSP_CAPTURE_IO_ERROR, now_ms);
}

void omni_dsp_capture_cancel(omni_dsp_capture *capture, uint32_t now_ms)
{
    if (capture == NULL || !capture->initialized || capture->phase != OMNI_DSP_CAPTURE_RUNNING) return;
    freeze(capture, OMNI_DSP_CAPTURE_CANCELLED, now_ms);
}

void omni_dsp_capture_poll(omni_dsp_capture *capture, uint32_t now_ms)
{
    if (capture == NULL || !capture->initialized || capture->phase != OMNI_DSP_CAPTURE_RUNNING) return;
    if ((uint32_t)(now_ms - capture->last_poll_ms) > INT32_MAX) {
        freeze(capture, OMNI_DSP_CAPTURE_BAD_TIME, now_ms);
        return;
    }
    capture->last_poll_ms = now_ms;
    omni_link_expire(&capture->parser, now_ms);
    if ((uint32_t)(now_ms - capture->started_ms) >= capture->duration_ms) {
        freeze(capture, OMNI_DSP_CAPTURE_COMPLETE, now_ms);
        return;
    }
    for (unsigned attempt = 0; attempt < OMNI_DSP_CAPTURE_POLL_BYTES; ++attempt) {
        uint8_t byte = 0;
        int result = capture->rx(capture->context, &byte);
        if (result == 0) break;
        if (result != 1) {
            omni_dsp_capture_fail(capture, result, now_ms);
            return;
        }
        increment(&capture->rx_bytes);
        const uint8_t *frame;
        size_t length;
        if (!omni_link_feed(&capture->parser, byte, now_ms, &frame, &length)) continue;
        increment(&capture->received_frames);
        if (capture->stored_records == OMNI_DSP_CAPTURE_RECORDS) {
            increment(&capture->overflow_frames);
            continue;
        }
        /* Shared parser bounds complete frames to OMNI_LINK_RX_MAX. Keep
         * every accepted byte, including 15, rather than stock normalization. */
        omni_dsp_capture_record *record = &capture->records[capture->stored_records];
        record->sequence = capture->received_frames;
        record->received_ms = now_ms;
        record->length = (uint16_t)length;
        memcpy(record->raw, frame, length);
        ++capture->stored_records;
    }
}

bool omni_dsp_capture_get_status(const omni_dsp_capture *capture, omni_dsp_capture_status *out)
{
    if (!frozen(capture) || out == NULL) return false;
    omni_dsp_capture_status status = {0};
    status.phase = capture->phase;
    status.started_ms = capture->started_ms;
    status.finished_ms = capture->finished_ms;
    status.duration_ms = capture->duration_ms;
    status.rx_bytes = capture->rx_bytes;
    status.received_frames = capture->received_frames;
    status.overflow_frames = capture->overflow_frames;
    status.ignored_frames = capture->parser.ignored;
    status.malformed_headers = capture->parser.malformed;
    status.expired_partial_frames = capture->parser.expired;
    status.io_result = capture->io_result;
    status.pending_bytes = capture->parser.used;
    status.expected_bytes = capture->parser.expected;
    status.stored_records = capture->stored_records;
    *out = status;
    return true;
}

bool omni_dsp_capture_get_record(const omni_dsp_capture *capture, unsigned index,
                                 omni_dsp_capture_record *out)
{
    if (!frozen(capture) || out == NULL || index >= capture->stored_records ||
        index >= OMNI_DSP_CAPTURE_RECORDS) return false;
    *out = capture->records[index];
    return true;
}
