#include "audio_rate.h"
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>

static int64_t delta(uint32_t md)
{
    return (int64_t)md - (int64_t)OMNI_AUDIO_RATE_MD_NOMINAL;
}

static void assert_bounded(uint32_t md)
{
    assert(delta(md) >= -OMNI_AUDIO_RATE_MD_LIMIT);
    assert(delta(md) <= OMNI_AUDIO_RATE_MD_LIMIT);
}

static void test_elapsed_and_reset(void)
{
    omni_audio_rate_t rate;
    assert(omni_audio_rate_init(&rate, 512u));
    assert(rate.last_md == OMNI_AUDIO_RATE_MD_NOMINAL);
    assert(omni_audio_rate_update(&rate, 512u * 256u, 32u, true) == rate.last_md);
    /* Split/irregular integration must use actual elapsed time, not calls. */
    for (unsigned i = 0; i < 10u; ++i) {
        omni_audio_rate_update(&rate, 513u * 256u, 17u, true);
        omni_audio_rate_update(&rate, 513u * 256u, 83u, true);
    }
    assert(omni_audio_rate_integral(&rate) == 4000);
    assert(delta(rate.last_md) == 44000);
    int64_t saved_integral = rate.integral_milli_q8;
    uint32_t saved_md = rate.last_md;
    assert(omni_audio_rate_update(&rate, 0u, 0u, true) == saved_md);
    assert(rate.integral_milli_q8 == saved_integral);
    assert(omni_audio_rate_update(&rate, 0u, 0u, false) == OMNI_AUDIO_RATE_MD_NOMINAL);
    assert(rate.integral_milli_q8 == 0);
    omni_audio_rate_update(&rate, 600u * 256u, 32u, true);
    assert(omni_audio_rate_update(&rate, 600u * 256u, 1001u, true) == OMNI_AUDIO_RATE_MD_NOMINAL);
    assert(rate.integral_milli_q8 == 0);
    assert(omni_audio_rate_update(&rate, UINT32_MAX, 32u, true) == OMNI_AUDIO_RATE_MD_NOMINAL);
    assert(!omni_audio_rate_init(&rate, UINT32_MAX));
    assert(rate.target_frames == 1u);
    assert(!omni_audio_rate_init(&rate, 0u));
    assert(!omni_audio_rate_init(NULL, 512u));
    omni_audio_rate_reset(NULL);
    assert(omni_audio_rate_update(NULL, 0u, 0u, true) == OMNI_AUDIO_RATE_MD_NOMINAL);
    assert(omni_audio_rate_integral(NULL) == 0);

    /* A sub-frame error must integrate even when each update contributes less
     * than one MD unit. Negative Q8 errors must not rely on signed shifts. */
    assert(omni_audio_rate_init(&rate, 512u));
    for (unsigned i = 0; i < 1000u; ++i)
        omni_audio_rate_update(&rate, 512u * 256u + 1u, 1u, true);
    assert(rate.integral_milli_q8 == INT64_C(4000000));
    omni_audio_rate_reset(&rate);
    for (unsigned i = 0; i < 1000u; ++i)
        omni_audio_rate_update(&rate, 512u * 256u - 1u, 1u, true);
    assert(rate.integral_milli_q8 == -INT64_C(4000000));
}

static void test_saturation_force_and_gains(void)
{
    omni_audio_rate_t rate;
    assert(omni_audio_rate_init(&rate, 512u));
    for (unsigned i = 0; i < 5000u; ++i) {
        assert(delta(omni_audio_rate_update(&rate, 2000u * 256u, 32u, true)) == OMNI_AUDIO_RATE_MD_LIMIT);
        assert(rate.integral_milli_q8 == 0); /* no outward windup */
    }
    assert(omni_audio_rate_update(&rate, 512u * 256u, 32u, true) == OMNI_AUDIO_RATE_MD_NOMINAL);
    for (unsigned i = 0; i < 5000u; ++i) {
        assert(delta(omni_audio_rate_update(&rate, 0u, 32u, true)) == -OMNI_AUDIO_RATE_MD_LIMIT);
        assert(rate.integral_milli_q8 == 0);
    }
    omni_audio_rate_set_force(&rate, UINT32_MAX);
    assert(delta(omni_audio_rate_update(&rate, 0u, 32u, true)) == OMNI_AUDIO_RATE_MD_LIMIT);
    assert(rate.integral_milli_q8 == 0);
    omni_audio_rate_set_force(&rate, 1u);
    assert(delta(omni_audio_rate_update(&rate, 2000u * 256u, 32u, true)) == -OMNI_AUDIO_RATE_MD_LIMIT);
    assert(omni_audio_rate_update(&rate, 0u, 32u, false) == OMNI_AUDIO_RATE_MD_NOMINAL);
    assert(rate.forced_md != 0u);
    omni_audio_rate_set_force(&rate, 0u);
    assert(omni_audio_rate_update(&rate, 512u * 256u, 32u, true) == OMNI_AUDIO_RATE_MD_NOMINAL);
    assert(!omni_audio_rate_set_gains(&rate, -1, 0));
    assert(!omni_audio_rate_set_gains(&rate, 0, 100001));
    assert(!omni_audio_rate_set_gains(&rate, 1000001, 0));
    assert(rate.kp == OMNI_AUDIO_RATE_KP_DEFAULT && rate.ki == OMNI_AUDIO_RATE_KI_DEFAULT);
    assert(omni_audio_rate_set_gains(&rate, 1000000, 100000));
    /* Exercise the largest supported arithmetic operands under UBSan. */
    assert_bounded(omni_audio_rate_update(&rate, OMNI_AUDIO_RATE_MAX_FILL_FRAMES * 256u,
                                        OMNI_AUDIO_RATE_MAX_ELAPSED_MS, true));
    assert(omni_audio_rate_set_gains(&rate, 0, 0));
    assert(omni_audio_rate_update(&rate, 2000u * 256u, 32u, true) == OMNI_AUDIO_RATE_MD_NOMINAL);
    assert(omni_audio_rate_set_gains(&rate, 0, 4000));
    for (unsigned i = 0; i < 100u; ++i)
        assert_bounded(omni_audio_rate_update(&rate, 1512u * 256u, 1000u, true));
    assert(omni_audio_rate_integral(&rate) == OMNI_AUDIO_RATE_MD_LIMIT);
    /* A saturated integral can unwind immediately when error changes sign. */
    omni_audio_rate_update(&rate, 12u * 256u, 1000u, true);
    assert(omni_audio_rate_integral(&rate) == 10000000);
    omni_audio_rate_reset(&rate);
    assert(rate.kp == 0 && rate.ki == 4000 && rate.integral_milli_q8 == 0);
    assert(!omni_audio_rate_set_gains(NULL, 0, 0));
    omni_audio_rate_set_force(NULL, 1u);
}

/* Analytical plant, NOT hardware evidence. The consumer is ideal with no PLL
 * lock/update delay and frequency proportional to MD. Producer packets contain
 * integral frames; bounded delivery bursts preserve every sample. This checks
 * closed-loop stability and occupancy for that model, not RF/DSP behaviour,
 * missed isochronous transfers, CPU deadlines or safe live PLL programming. */
static void run_plant(unsigned frames_per_ms,double ppm, unsigned burst_ms, bool irregular, bool reverse)
{
    omni_audio_rate_t rate;
    assert(omni_audio_rate_init(&rate, 512u));
    double ratio=(double)frames_per_ms/48.0;
    double fill = 512.0*ratio, produced_fraction = 0.0;
    double minimum = fill, maximum = fill;
    double late_sum = 0.0;
    unsigned late_count = 0u, pending_frames = 0u;
    uint64_t fill_sum_q8 = 0u;
    unsigned samples = 0u, elapsed = 0u, schedule_index = 0u;
    static const unsigned intervals[] = {17u, 47u, 25u, 39u, 16u, 48u};
    unsigned interval = irregular ? intervals[0] : 32u;
    uint32_t md = OMNI_AUDIO_RATE_MD_NOMINAL;
    for (unsigned ms = 1u; ms <= 120000u; ++ms) {
        double current_ppm = reverse && ms > 60000u ? -ppm : ppm;
        produced_fraction += frames_per_ms * (1.0 + current_ppm / 1000000.0);
        unsigned produced = (unsigned)produced_fraction;
        produced_fraction -= produced;
        pending_frames += produced;
        if (ms % burst_ms == 0u) {
            fill += pending_frames;
            pending_frames = 0u;
        }
        fill -= frames_per_ms * ((double)md / (double)OMNI_AUDIO_RATE_MD_NOMINAL);
        assert(fill > 0.0 && fill < 2048.0); /* no samples may be lost/repeated */
        if (fill < minimum) minimum = fill;
        if (fill > maximum) maximum = fill;
        fill_sum_q8 += (uint32_t)(fill * 256.0 + 0.5);
        ++samples;
        ++elapsed;
        if (elapsed == interval) {
            md = omni_audio_rate_update_format(&rate, (uint32_t)(fill_sum_q8 / samples),frames_per_ms*1000u,elapsed,true);
            assert_bounded(md);
            fill_sum_q8 = 0u;
            samples = elapsed = 0u;
            if (irregular) {
                schedule_index = (schedule_index + 1u) % (sizeof(intervals) / sizeof(intervals[0]));
                interval = intervals[schedule_index];
            }
        }
        if (ms > 110000u) { late_sum += fill; ++late_count; }
    }
    double late_mean = late_sum / late_count;
    double desired_delta = (reverse ? -ppm : ppm) * (double)OMNI_AUDIO_RATE_MD_NOMINAL / 1000000.0;
    assert(minimum > 64.0*ratio && maximum < 1024.0*ratio);
    assert(fabs(late_mean/ratio - 512.0) < 3.0);
    assert(fabs((double)delta(md) - desired_delta) < 250000.0);
    printf("plant Fs=%u ppm=%+.0f burst=%u ms irregular=%u reverse=%u: fill %.2f..%.2f, final mean %.3f, MD delta %lld\n",
           frames_per_ms*1000u,ppm, burst_ms, irregular ? 1u : 0u, reverse ? 1u : 0u,
           minimum, maximum, late_mean, (long long)delta(md));
}

int main(void)
{
    test_elapsed_and_reset();
    test_saturation_force_and_gains();
    const double ppm[] = {-2000.0, -1000.0, -100.0, 0.0, 100.0, 1000.0, 2000.0};
    for(unsigned nominal=48u;nominal<=96u;nominal+=48u) {
        for (unsigned i = 0; i < sizeof(ppm) / sizeof(ppm[0]); ++i) {
            run_plant(nominal,ppm[i], 1u, false, false);
            run_plant(nominal,ppm[i], 4u, true, false);
        }
        run_plant(nominal,2000.0,4u,true,true);run_plant(nominal,-2000.0,4u,true,true);
    }
    omni_audio_rate_t rate;assert(omni_audio_rate_init(&rate,512u));
    assert(omni_audio_rate_update_format(&rate,1024u*256u,96000u,32u,true)==OMNI_AUDIO_RATE_MD_NOMINAL);
    assert(omni_audio_rate_update_format(&rate,1030u*256u,96000u,32u,true)>OMNI_AUDIO_RATE_MD_NOMINAL);
    assert(omni_audio_rate_update_format(&rate,UINT32_MAX,96000u,32u,true)==OMNI_AUDIO_RATE_MD_NOMINAL);
    assert(omni_audio_rate_update_format(&rate,512u*256u,44100u,32u,true)==OMNI_AUDIO_RATE_MD_NOMINAL);
    puts("audio rate tests passed");
    return 0;
}
