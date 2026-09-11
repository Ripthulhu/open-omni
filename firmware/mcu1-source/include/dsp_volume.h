#ifndef OMNI_DSP_VOLUME_H
#define OMNI_DSP_VOLUME_H
#include "interchip.h"

/* Read-only DSP discovery. No PCM scaling, raw-command API or SET encoder.
 * All operations have one main-loop owner; caller serializes desired state
 * with USB/dial changes using a coherent snapshot. Never call from an ISR. */
typedef struct {
    void *context;
    /* Byte I/O: 1 transferred,0 backpressure/unavailable,negative failure. */
    int (*tx)(void *, uint8_t);
    int (*rx)(void *, uint8_t *);
    /* 1 means the query physically left UART;0 pending;negative failure.
     * This is local transport completion, not DSP acceptance. */
    int (*tx_complete)(void *);
} omni_dsp_volume_io;

typedef enum {
    OMNI_DSP_VOLUME_IDLE, OMNI_DSP_VOLUME_SEND, OMNI_DSP_VOLUME_DRAIN,
    OMNI_DSP_VOLUME_WAIT, OMNI_DSP_VOLUME_COOLDOWN,
    OMNI_DSP_VOLUME_COMPLETE, OMNI_DSP_VOLUME_FAILED,
    OMNI_DSP_VOLUME_CANCELED
} omni_dsp_volume_phase;
typedef enum {
    OMNI_DSP_VOLUME_OK, OMNI_DSP_VOLUME_TIMEOUT, OMNI_DSP_VOLUME_IO_ERROR,
    OMNI_DSP_VOLUME_PEER_ERROR, OMNI_DSP_VOLUME_BAD_REPLY
} omni_dsp_volume_error;

#define OMNI_DSP_VOLUME_TIMEOUT_MS 750u
#define OMNI_DSP_VOLUME_QUERY_MS 250u
#define OMNI_DSP_VOLUME_GAP_MS 20u
#define OMNI_DSP_VOLUME_RX_BUDGET 32u
typedef struct {
    omni_dsp_volume_io io;
    omni_link_parser parser;
    omni_dsp_volume_phase phase;
    omni_dsp_volume_error error;
    uint32_t started_ms, query_ms, drained_ms;
    uint32_t mode_received_ms, levels_received_ms;
    uint32_t tx_bytes, rx_bytes, unexpected_frames, ack_frames;
    uint32_t desired_revision;
    int16_t desired_db;
    uint8_t desired_muted;
    uint8_t mode, levels[4]; /* Preserve four wire lanes; lane0!=lane1 allowed. */
    uint8_t mode_valid, levels_valid, query_index, tx_offset, got_ack, got_reply;
    uint8_t peer_status, initialized;
} omni_dsp_volume;

/* No I/O in init/begin. Requires fresh exclusive RX boundary and resynchronized
 * UART after partial TX/failure. Init is the only reset; begin allowed once.
 * No transaction ID exists: freshness cannot be established from replies alone.
 * COMPLETE means both observations and generic query replies were received,
 * not atomic settings, audible gain, mode ownership or permission to SET. */
bool omni_dsp_volume_init(omni_dsp_volume *, omni_dsp_volume_io);
bool omni_dsp_volume_begin(omni_dsp_volume *, uint32_t now_ms);
void omni_dsp_volume_poll(omni_dsp_volume *, uint32_t now_ms);
void omni_dsp_volume_cancel(omni_dsp_volume *);
/* Native UAC2 range in signed dB8.8. Coalesces while retaining volume during
 * mute; defaults -30dB and muted. This function never emits UART bytes. */
bool omni_dsp_volume_desire(omni_dsp_volume *, int16_t db, bool muted);
#endif
