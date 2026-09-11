#ifndef OMNI_DSP_CAPTURE_H
#define OMNI_DSP_CAPTURE_H

#include "interchip.h"

#define OMNI_DSP_CAPTURE_RECORDS 16u
#define OMNI_DSP_CAPTURE_RECORD_BYTES OMNI_LINK_RX_MAX
#define OMNI_DSP_CAPTURE_MIN_MS 1000u
#define OMNI_DSP_CAPTURE_MAX_MS 30000u
#define OMNI_DSP_CAPTURE_GAP_MS 20u
#define OMNI_DSP_CAPTURE_POLL_BYTES 32u

typedef enum {
    OMNI_DSP_CAPTURE_IDLE = 0,
    OMNI_DSP_CAPTURE_RUNNING = 1,
    /* Normal end of the requested observation window, not a peer reply. */
    OMNI_DSP_CAPTURE_COMPLETE = 2,
    OMNI_DSP_CAPTURE_IO_ERROR = 3,
    OMNI_DSP_CAPTURE_CANCELLED = 4,
    OMNI_DSP_CAPTURE_BAD_TIME = 5
} omni_dsp_capture_phase;

typedef struct {
    uint32_t sequence; /* One-based complete parser-frame number. */
    uint32_t received_ms; /* Absolute supplied time when the final byte arrived. */
    uint16_t length;
    uint8_t raw[OMNI_DSP_CAPTURE_RECORD_BYTES];
} omni_dsp_capture_record;

typedef struct {
    omni_dsp_capture_phase phase;
    uint32_t started_ms, finished_ms, duration_ms;
    uint32_t rx_bytes, received_frames, overflow_frames;
    uint32_t ignored_frames, malformed_headers, expired_partial_frames;
    int32_t io_result;
    uint16_t pending_bytes, expected_bytes;
    uint8_t stored_records;
} omni_dsp_capture_status;

typedef struct {
    /* RX only, compatible with mcu2_link_io.context/rx. There is no TX member.
     * Callback returns1 for a byte,0 for unavailable, negative on error. */
    void *context;
    int (*rx)(void *, uint8_t *);
    omni_link_parser parser;
    omni_dsp_capture_record records[OMNI_DSP_CAPTURE_RECORDS];
    omni_dsp_capture_phase phase;
    uint32_t started_ms, finished_ms, duration_ms, last_poll_ms;
    uint32_t rx_bytes, received_frames, overflow_frames;
    int32_t io_result;
    uint8_t stored_records;
    bool initialized;
} omni_dsp_capture;

/* No heap, MMIO, UART start/stop, flush, peer command or TX callback.
 * One main-loop owner serializes all operations. Init requires an inactive
 * object and discards previous evidence. Wrapper requires explicit new-token
 * rearm and obtains exclusive/fresh RX ownership before begin. */
bool omni_dsp_capture_init(omni_dsp_capture *, void *context, int (*rx)(void *, uint8_t *));
/* May reuse a frozen core, discarding old records. Hardware wrapper owns the
 * explicit rearm policy. Rejects invalid duration or running. */
bool omni_dsp_capture_begin(omni_dsp_capture *, uint32_t duration_ms, uint32_t now_ms);
/* At most 32 RX callback attempts. Stops before RX at the duration deadline.
 *20ms idle expiry is capture policy. Preserves marker15 and full 204-byte RACE;
 * DD frames stay ignored by the shared parser with an exposed counter. */
void omni_dsp_capture_poll(omni_dsp_capture *, uint32_t now_ms);
void omni_dsp_capture_cancel(omni_dsp_capture *, uint32_t now_ms);
/* Wrapper may freeze on line/ring errors even if the RX callback has no error
 * to return. Nonzero reason is retained as io_result; zero is normalized to-1. */
void omni_dsp_capture_fail(omni_dsp_capture *, int32_t reason, uint32_t now_ms);

/* Retrieval copies frozen evidence only. Running/idle/invalid index/null
 * failures leave out unchanged. Header phase is available directly for busy
 * polling by the serialized owner. No pointers into mutable records escape. */
bool omni_dsp_capture_get_status(const omni_dsp_capture *, omni_dsp_capture_status *out);
bool omni_dsp_capture_get_record(const omni_dsp_capture *, unsigned index,
                                 omni_dsp_capture_record *out);

#endif
