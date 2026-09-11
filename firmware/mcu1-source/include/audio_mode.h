#ifndef OMNI_AUDIO_MODE_H
#define OMNI_AUDIO_MODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OMNI_AUDIO_MODE_IO_ERROR = -1,
    OMNI_AUDIO_MODE_IO_PENDING = 0,
    OMNI_AUDIO_MODE_IO_COMPLETE = 1
} omni_audio_mode_io_t;

typedef struct {
    void *context;
    /* Exclusive DSP UART transport: return accepted byte count, zero for
     * backpressure or negative for failure; never accept more than length.
     * Consume/copy accepted bytes before returning; do not retain data. */
    int (*write)(void *context, const uint8_t *data, size_t length);
    /* Completion means these bytes physically finished transmitting, not a DSP
     * acknowledgment. Must not report unrelated/earlier UART completion. */
    omni_audio_mode_io_t (*tx_status)(void *context);
    /* Bounded pin isolation/routing operation. false means failure. */
    bool (*pins)(void *context, bool enabled);
    /* Source-owned, bounded stop/configure/prime-zero/start sequence while pins
     * are isolated. Hardware clock/pin contracts must already be validated.
     * begin is called once; poll advances without blocking until complete. */
    bool (*configure_begin)(void *context, uint32_t sample_rate);
    omni_audio_mode_io_t (*configure_poll)(void *context);
} omni_audio_mode_ops_t;

typedef enum {
    OMNI_AUDIO_MODE_IDLE,
    OMNI_AUDIO_MODE_INITIAL_DELAY,
    OMNI_AUDIO_MODE_SEND_STOP,
    OMNI_AUDIO_MODE_STOP_DRAIN,
    OMNI_AUDIO_MODE_STOP_GAP,
    OMNI_AUDIO_MODE_SEND_GATE,
    OMNI_AUDIO_MODE_GATE_DRAIN,
    OMNI_AUDIO_MODE_QUIET,
    OMNI_AUDIO_MODE_ISOLATE,
    OMNI_AUDIO_MODE_CONFIGURE_BEGIN,
    OMNI_AUDIO_MODE_CONFIGURING,
    OMNI_AUDIO_MODE_CONNECT,
    OMNI_AUDIO_MODE_RESUME_DELAY,
    OMNI_AUDIO_MODE_SEND_RESUME,
    OMNI_AUDIO_MODE_RESUME_DRAIN,
    OMNI_AUDIO_MODE_RESUME_GAP,
    OMNI_AUDIO_MODE_SEND_RATE,
    OMNI_AUDIO_MODE_RATE_DRAIN,
    OMNI_AUDIO_MODE_RATE_GAP,
    /* Local transition completed. Optional ACKs prove command acceptance only,
     * never stream creation, negotiated RF format or acoustic output. */
    OMNI_AUDIO_MODE_LOCAL_COMPLETE,
    OMNI_AUDIO_MODE_FAILED,
    OMNI_AUDIO_MODE_CANCELED
} omni_audio_mode_state_t;

typedef enum {
    OMNI_AUDIO_MODE_ERROR_NONE,
    OMNI_AUDIO_MODE_ERROR_TX,
    OMNI_AUDIO_MODE_ERROR_CONFIGURATION,
    OMNI_AUDIO_MODE_ERROR_PINS,
    OMNI_AUDIO_MODE_ERROR_TIMEOUT,
    OMNI_AUDIO_MODE_ERROR_REJECTED
} omni_audio_mode_error_t;

typedef struct {
    omni_audio_mode_ops_t ops;
    omni_audio_mode_state_t state;
    omni_audio_mode_error_t error;
    uint32_t sample_rate;
    uint32_t started_ms;
    uint32_t phase_ms;
    uint32_t transmitted_bytes;
    uint8_t frame_offset;
    bool pins_isolated;
    bool isolation_failed;
    bool initialized;
    bool require_ack, ack_waiting, ack_received;
    uint8_t ack_command, ack_status;
    uint32_t acknowledged_frames;
    bool quiescing, quiesce_drained;
    uint32_t quiesce_started_ms, quiesce_drained_ms;
} omni_audio_mode_t;

#define OMNI_AUDIO_MODE_TIMEOUT_MS 1000u
/* Stock UART TX-complete callback23C60 defers queue-release B005 by 20ms. */
#define OMNI_AUDIO_MODE_INTERFRAME_MS 20u

/* Init/reinit is software-only and requires no active transition and a fresh,
 * exclusive DSP transport. Never reinit a partially transmitted frame merely
 * to retry. Recovery from such a failure requires external resynchronization. */
bool omni_audio_mode_init(omni_audio_mode_t *mode, const omni_audio_mode_ops_t *ops);
/* Opt in before begin/first TX. Old research callers remain open-loop. The
 * existing overall deadline bounds missing ACKs; no resend is attempted. */
bool omni_audio_mode_require_ack(omni_audio_mode_t *mode);
/* A complete frame from the same exclusive fresh UART session. DD/03/op/status
 * has four physical bytes despite its length byte. Matching acceptance is not
 * rate readback: stock rejects GET51. Returns true only for the pending opcode. */
bool omni_audio_mode_receive_ack(omni_audio_mode_t *mode,
                                 const uint8_t *frame, size_t length);
/* Finish only an already-partially-written command, then wait physical TXIDLE
 * and the stock 20ms queue cooldown before isolating pins. Never starts the next
 * command. Caller retains UART ownership until COMPLETE/ERROR; ERROR requires
 * a fault latch/resynchronization before another transition. */
omni_audio_mode_io_t omni_audio_mode_quiesce(omni_audio_mode_t *mode, uint32_t now_ms);
/* Caller gates on host configuration and validated hardware backend readiness.
 * No hardware action occurs in begin. Failed/canceled instances require reinit. */
bool omni_audio_mode_begin(omni_audio_mode_t *mode, uint32_t sample_rate, uint32_t now_ms);
/* Each poll performs at most one normal callback plus one failure-isolation
 * callback. Timeouts use unsigned elapsed time; no retries. */
void omni_audio_mode_poll(omni_audio_mode_t *mode, uint32_t now_ms);
void omni_audio_mode_cancel(omni_audio_mode_t *mode);

#endif
