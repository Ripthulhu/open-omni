#ifndef OMNI_DSP_GAIN_TRIAL_H
#define OMNI_DSP_GAIN_TRIAL_H
#include "dsp_volume.h"

/* One attenuation/restore experiment, never an automatic volume controller.
 * Exclusive fresh UART boundary required. No mode/save/reset operations.
 * Any uncertain transfer stops without retrying a SET or claiming restoration. */
typedef enum { GAIN_IDLE, GAIN_SEND, GAIN_DRAIN, GAIN_WAIT, GAIN_GAP,
    GAIN_HOLD, GAIN_DONE, GAIN_FAILED, GAIN_CANCELED } omni_gain_phase;
typedef enum { GAIN_OK, GAIN_TIMEOUT, GAIN_IO, GAIN_FRAME, GAIN_PEER,
    GAIN_MODE, GAIN_LEVEL, GAIN_CHANGED, GAIN_READBACK } omni_gain_error;
typedef struct {
    omni_dsp_volume_io io;
    omni_link_parser parser;
    omni_gain_phase phase;
    omni_gain_error error;
    uint32_t batch_ms, request_ms, drained_ms, hold_ms;
    uint32_t tx_bytes, rx_bytes, unrelated, set_may_have_applied;
    uint8_t step, offset, ack, reply, attempts, peer_status;
    uint8_t original[4], reduced[4], observed[4];
    uint8_t tx_frame[8][8], rx_frame[8][8], ack_frame[8][4];
    uint8_t tx_length[8], rx_length[8], mode;
    bool attenuated_verified, restored_verified;
    bool apply_only;
    uint8_t target, target_mask, targets[4];
} omni_gain_trial;

bool omni_gain_trial_begin(omni_gain_trial *, omni_dsp_volume_io, uint32_t now);
/* One native gain transaction: fresh mode/tuple, preserve A1/B/C, SET if
 * different, delayed readback, then DONE without a listening hold/restore. */
bool omni_gain_apply_begin(omni_gain_trial *,omni_dsp_volume_io,uint32_t now,uint8_t target);
/* Restore a prior trial only: equal other lanes, exact 25-step A reduction.
 * Fresh mode/tuple reads must match expected before the one restore SET. */
bool omni_gain_apply_masked_begin(omni_gain_trial *,omni_dsp_volume_io,uint32_t now,uint8_t mask,const uint8_t targets[4]);
bool omni_gain_restore_begin(omni_gain_trial *, omni_dsp_volume_io, uint32_t now,
                            const uint8_t expected[4],const uint8_t original[4]);
void omni_gain_trial_poll(omni_gain_trial *, uint32_t now);
void omni_gain_trial_cancel(omni_gain_trial *);
bool omni_gain_trial_busy(const omni_gain_trial *);
#endif
