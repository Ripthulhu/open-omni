#ifndef OMNI_HEADSET_GAIN_H
#define OMNI_HEADSET_GAIN_H
#include "interchip.h"
#include <stdbool.h>
#include <stdint.h>

#define OMNI_HEADSET_GAIN_QUEUE_MS 2000u
#define OMNI_HEADSET_GAIN_TIMEOUT_MS 750u
#define OMNI_HEADSET_GAIN_GAP_MS 20u
#define OMNI_HEADSET_GAIN_ECHO_MS 1000u
typedef enum {
    HEADSET_GAIN_IDLE,HEADSET_GAIN_QUEUED,HEADSET_GAIN_SEND,HEADSET_GAIN_DRAIN,
    HEADSET_GAIN_WAIT,HEADSET_GAIN_GAP,HEADSET_GAIN_DONE,HEADSET_GAIN_TIMEOUT,
    HEADSET_GAIN_CANCELLED,HEADSET_GAIN_IO_ERROR,HEADSET_GAIN_NACK,HEADSET_GAIN_INVALID_REPLY,
    HEADSET_GAIN_CONFLICT,HEADSET_GAIN_DEFERRED
} omni_headset_gain_phase;
typedef struct {
    void *context;
    int (*tx)(void *,uint8_t);
    int (*drained)(void *);
} omni_headset_gain_io;
typedef struct {
    omni_link_parser parser;
    omni_headset_gain_phase phase;
    uint8_t frame[5],length,offset,operation,desired,submitted,verified,peer_status;
    uint8_t seen,remote,history[4],history_count,history_cursor;
    uint8_t readback,conflict_attempts;
    uint32_t revision,submitted_revision,verified_revision,frame_revision,remote_revision;
    uint32_t queued_ms,started_ms,drained_ms,finished_ms,observed_ms,remote_ms,history_ms[4];
    uint32_t tx_bytes,rx_bytes,acks,timeouts,invalid,nacks,unrelated,coalesced,completed;
    uint32_t remote_events,echoes,epoch,gain_recoveries;
    uint32_t conflicts,reconciliations,queue_deferrals;
    bool initialized,online,desired_valid,primed,have_verified,have_seen,have_link,connected;
    bool fault,blocked,cancel,yielding,ack,negative,bad_reply,matched,eligible,remote_pending;
    bool readback_conflict;
} omni_headset_gain;

bool omni_headset_gain_init(omni_headset_gain *);
/* Main-loop only. LL0..56 is the recovered D201 loudness index, not percent.
 * Revisions accompany the shared Windows/dial state; an active SET is immutable.
 * A changed queued intent refreshes its admission deadline. A wholly unsent
 * admission expiry is DEFERRED: prior readiness survives and it resumes only
 * when actual admission is available. Sent transaction timeouts still latch.
 * This client owns neither the dB mapping nor the USB volume state. */
bool omni_headset_gain_desire(omni_headset_gain *,uint8_t ll,uint32_t master_revision,uint32_t now);
void omni_headset_gain_byte(omni_headset_gain *,uint8_t byte,uint32_t now,uint32_t master_revision);
/* bulk20 first enables DSP's dedicated D202 report. can_apply separately gates
 * the following D201 SET on the adapter's mode/lineout initialization. bulk20
 * can reconcile output mode, so apply mode43 AFTER this preparation. Requests:
 * BD042001 -> DB2E2001..., BD05D201LL -> DD03D200,
 * 20ms physical spacing -> BD05D20200 -> DB06D20300LL (NO GET ACK).
 * ready means a verified local DSP source5 shadow, not acoustic proof or mute. */
void omni_headset_gain_poll(omni_headset_gain *,uint32_t now,bool can_start,bool can_apply,omni_headset_gain_io);
bool omni_headset_gain_busy(const omni_headset_gain *);
bool omni_headset_gain_active(const omni_headset_gain *);
bool omni_headset_gain_primed(const omni_headset_gain *);
bool omni_headset_gain_ready(const omni_headset_gain *);
bool omni_headset_gain_settled(const omni_headset_gain *);
bool omni_headset_gain_frame_pending(const omni_headset_gain *);
void omni_headset_gain_yield(omni_headset_gain *,uint32_t now);
void omni_headset_gain_release(omni_headset_gain *,uint32_t now);
void omni_headset_gain_acquire(omni_headset_gain *,uint32_t now);
void omni_headset_gain_io_error(omni_headset_gain *,uint32_t now);
/* Candidate physical tag0 changes only after startup verification, including
 * during an owned transaction. An eligible GET candidate also requires its
 * first-byte revision to equal the immutable SET revision: an old readback
 * must not supersede a newer Windows command. Own-value duplicates and four recent SET values suppress
 * echoes for a bounded 1s window. This wire has no transaction IDs: arbitrarily
 * late echoes cannot be distinguished perfectly from a physical revisit.
 * Caller compares expected_revision atomically before changing USB state. */
bool omni_headset_gain_take_remote(omni_headset_gain *,uint8_t *ll,uint32_t *expected_revision,uint32_t *observed_ms);
bool omni_headset_gain_status(const omni_headset_gain *,unsigned page,uint32_t out[15]);
/* Status page1 word14 counts adapter gain recovery after an actual peer
 * disconnect/connect, fresh bulk20 and verified mode2; distinct from mode1 repair. */
/* A valid differing tag0 GET is an observed shadow conflict, never malformed.
 * One reconciliation is allowed per unchanged desired revision; a second
 * mismatch terminates as CONFLICT, recoverable by a NEW master revision or a
 * real peer epoch. Startup/reconnect observations never become master state.
 * page0 flags bit11 marks the last readback conflict; word14 low8 is peer
 * status, high24 a saturating count of conflicting GET transactions.
 * page0 flags bit12 marks current DEFERRED state; high16 counts queue deferrals
 * (saturating). These are not transmitted-request timeout/errors on page1. */
#endif
