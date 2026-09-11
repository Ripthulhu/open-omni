#ifndef OMNI_AUDIO_RATE_H
#define OMNI_AUDIO_RATE_H

#include <stdbool.h>
#include <stdint.h>

/* Pure buffer-fill controller. It owns neither samples nor PLL registers.
 * Increasing MD must increase the consumer rate. Board code must establish
 * that relationship, apply the returned value and report hardware faults. */
#define OMNI_AUDIO_RATE_MD_NOMINAL UINT32_C(0xf5c28f5c)
#define OMNI_AUDIO_RATE_MD_LIMIT INT32_C(12000000)
#define OMNI_AUDIO_RATE_KP_DEFAULT INT32_C(40000)
#define OMNI_AUDIO_RATE_KI_DEFAULT INT32_C(4000)
#define OMNI_AUDIO_RATE_MAX_ELAPSED_MS UINT32_C(1000)
#define OMNI_AUDIO_RATE_MAX_FILL_FRAMES UINT32_C(65535)

typedef struct {
    uint32_t target_frames;
    int32_t kp;                 /* MD / frame */
    int32_t ki;                 /* MD / (frame * second) */
    int64_t integral_milli_q8;   /* MD * 256 * 1000; retains integration remainder */
    uint32_t forced_md;         /* zero selects automatic control */
    uint32_t last_md;
} omni_audio_rate_t;

/* A target outside 1..MAX_FILL_FRAMES leaves a reset target of 1 and fails. */
bool omni_audio_rate_init(omni_audio_rate_t *rate, uint32_t target_frames);
/* Preserve target, gains and explicit override; clear accumulated error. */
void omni_audio_rate_reset(omni_audio_rate_t *rate);
/* Caller averages real queue occupancy (in stereo frames) at a consistent
 * DMA boundary, then supplies that mean in Q8 approximately every 32 ms.
 * elapsed_ms must cover those measurements and use the same monotonic timer.
 * Inactive streams, invalid occupancy and gaps over 1000 ms reset to nominal.
 * A zero elapsed interval holds the output and never integrates.
 * Calls must be serialized; this object has no interrupt synchronization. */
uint32_t omni_audio_rate_update(omni_audio_rate_t *rate, uint32_t mean_fill_q8,
                               uint32_t elapsed_ms, bool stream_active);
/* Existing controller/target/gains remain expressed in 48k-equivalent frames.
 * Actual queue occupancy at 96k is divided by 2 so identical buffered time and
 * clock-frequency error produce the same control law. Invalid rates reset. */
uint32_t omni_audio_rate_update_format(omni_audio_rate_t *rate,uint32_t mean_fill_q8,
    uint32_t sample_rate,uint32_t elapsed_ms,bool stream_active);
/* Force is bounded to the same safe numerical range as automatic control.
 * Changing force clears the integral. An inactive stream always uses nominal. */
void omni_audio_rate_set_force(omni_audio_rate_t *rate, uint32_t md);
/* Zero disables a term. Reject negative or excessive gains without mutation.
 * Accepted ranges: Kp 0..1000000; Ki 0..100000. These limits ensure arithmetic
 * safety, not stability for every possible gain combination or real board. */
bool omni_audio_rate_set_gains(omni_audio_rate_t *rate, int32_t kp, int32_t ki);
int32_t omni_audio_rate_integral(const omni_audio_rate_t *rate);

#endif
