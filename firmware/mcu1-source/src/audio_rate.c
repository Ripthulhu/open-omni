#include "audio_rate.h"
#include <stddef.h>

#define INTEGRAL_SCALE INT64_C(256000)
#define Q8_SCALE INT64_C(256)
#define INTEGRAL_LIMIT (INT64_C(12000000) * INTEGRAL_SCALE)
#define OUTPUT_LIMIT_Q8 (INT64_C(12000000) * Q8_SCALE)

static int64_t clamp64(int64_t value, int64_t limit)
{
    return value > limit ? limit : (value < -limit ? -limit : value);
}

bool omni_audio_rate_init(omni_audio_rate_t *rate, uint32_t target_frames)
{
    if (rate == NULL) return false;
    bool valid = target_frames != 0u && target_frames <= OMNI_AUDIO_RATE_MAX_FILL_FRAMES;
    rate->target_frames = valid ? target_frames : 1u;
    rate->kp = OMNI_AUDIO_RATE_KP_DEFAULT;
    rate->ki = OMNI_AUDIO_RATE_KI_DEFAULT;
    rate->forced_md = 0u;
    omni_audio_rate_reset(rate);
    return valid;
}

void omni_audio_rate_reset(omni_audio_rate_t *rate)
{
    if (rate == NULL) return;
    rate->integral_milli_q8 = 0;
    rate->last_md = OMNI_AUDIO_RATE_MD_NOMINAL;
}

uint32_t omni_audio_rate_update(omni_audio_rate_t *rate, uint32_t mean_fill_q8,
                               uint32_t elapsed_ms, bool stream_active)
{
    if (rate == NULL) return OMNI_AUDIO_RATE_MD_NOMINAL;
    if (!stream_active || elapsed_ms > OMNI_AUDIO_RATE_MAX_ELAPSED_MS ||
        mean_fill_q8 > OMNI_AUDIO_RATE_MAX_FILL_FRAMES * 256u) {
        omni_audio_rate_reset(rate);
        return rate->last_md;
    }
    if (elapsed_ms == 0u) return rate->last_md;
    if (rate->forced_md != 0u) {
        rate->integral_milli_q8 = 0;
        rate->last_md = rate->forced_md;
        return rate->last_md;
    }

    int64_t error_q8 = (int64_t)mean_fill_q8 - (int64_t)rate->target_frames * Q8_SCALE;
    int64_t proportional_q8 = error_q8 * rate->kp;
    int64_t increment = error_q8 * rate->ki * elapsed_ms;
    int64_t proposed_integral = clamp64(rate->integral_milli_q8 + increment, INTEGRAL_LIMIT);
    int64_t proposed_output_q8 = proportional_q8 + proposed_integral / 1000;
    /* Conditional integration: an error that pushes a saturated actuator
     * further outward must not accumulate. An inward error can unwind it.
     * No sample insertion/removal or buffer-index correction occurs here. */
    bool outward = (proposed_output_q8 > OUTPUT_LIMIT_Q8 && increment > 0) ||
                   (proposed_output_q8 < -OUTPUT_LIMIT_Q8 && increment < 0);
    if (!outward) rate->integral_milli_q8 = proposed_integral;
    int64_t output_q8 = clamp64(proportional_q8 + rate->integral_milli_q8 / 1000,
                               OUTPUT_LIMIT_Q8);
    /* Use a signed wide addition. Nominal MD does not fit in int32_t. */
    rate->last_md = (uint32_t)((int64_t)OMNI_AUDIO_RATE_MD_NOMINAL + output_q8 / Q8_SCALE);
    return rate->last_md;
}

void omni_audio_rate_set_force(omni_audio_rate_t *rate, uint32_t md)
{
    if (rate == NULL) return;
    if (md != 0u) {
        int64_t delta = (int64_t)md - (int64_t)OMNI_AUDIO_RATE_MD_NOMINAL;
        md = (uint32_t)((int64_t)OMNI_AUDIO_RATE_MD_NOMINAL +
                        clamp64(delta, OMNI_AUDIO_RATE_MD_LIMIT));
    }
    rate->forced_md = md;
    rate->integral_milli_q8 = 0;
}

uint32_t omni_audio_rate_update_format(omni_audio_rate_t *rate,uint32_t mean_fill_q8,
    uint32_t sample_rate,uint32_t elapsed_ms,bool stream_active)
{
    if((sample_rate!=48000u && sample_rate!=96000u) ||
       mean_fill_q8>OMNI_AUDIO_RATE_MAX_FILL_FRAMES*256u)
        return omni_audio_rate_update(rate,0u,elapsed_ms,false);
    if(sample_rate==96000u) mean_fill_q8/=2u;
    return omni_audio_rate_update(rate,mean_fill_q8,elapsed_ms,stream_active);
}

bool omni_audio_rate_set_gains(omni_audio_rate_t *rate, int32_t kp, int32_t ki)
{
    if (rate == NULL || kp < 0 || kp > 1000000 || ki < 0 || ki > 100000) return false;
    rate->kp = kp;
    rate->ki = ki;
    rate->integral_milli_q8 = 0;
    return true;
}

int32_t omni_audio_rate_integral(const omni_audio_rate_t *rate)
{
    return rate == NULL ? 0 : (int32_t)(rate->integral_milli_q8 / INTEGRAL_SCALE);
}
