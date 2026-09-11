#include "audio_delivery.h"
#include <assert.h>
#include <stdio.h>

static void steady_and_reset(void)
{
    omni_audio_delivery_t s;
    omni_audio_delivery_reset(&s);
    for(uint32_t i=0;i<1000u;++i)
        omni_audio_delivery_observe(&s,192u,0x800u|((2040u+i)&2047u),3600000u+i);
    /* First callback starts the clock baseline, not an inferred earlier slot. */
    assert(s.words[OMNI_AD_VERSION]==2u && s.words[OMNI_AD_PACKETS]==1000u);
    assert(s.words[OMNI_AD_FRAMES]==48000u && s.words[OMNI_AD_ELAPSED_USB_FRAMES]==999u);
    assert(s.words[OMNI_AD_SKIPPED_SLOTS]==0u && s.words[OMNI_AD_REPEATED_CALLBACKS]==0u);
    assert(s.words[OMNI_AD_MAX_DELTA]==1u && s.words[OMNI_AD_PACKETS48]==1000u);
    assert(s.words[OMNI_AD_LAST_USB_FRAME]==991u && s.words[OMNI_AD_LAST_BYTES]==192u);
    omni_audio_delivery_reset(&s);
    assert(!s.have_previous && s.words[0]==2u);
    for(unsigned i=1;i<OMNI_AUDIO_DELIVERY_WORD_COUNT;++i) assert(s.words[i]==0u);
    omni_audio_delivery_observe(&s,192u,0u,5000000u);
    assert(s.words[OMNI_AD_ELAPSED_USB_FRAMES]==0u && s.words[OMNI_AD_MAX_DELTA]==0u);
}

static void timing_vs_loss(void)
{
    omni_audio_delivery_t s; const uint32_t crossed[]={100u,102u,102u,103u};
    omni_audio_delivery_reset(&s);
    for(uint32_t i=0;i<4u;++i) omni_audio_delivery_observe(&s,192u,crossed[i],1000u+i);
    assert(s.words[OMNI_AD_PACKETS]==4u && s.words[OMNI_AD_FRAMES]==192u);
    assert(s.words[OMNI_AD_ELAPSED_USB_FRAMES]==3u);
    assert(s.words[OMNI_AD_SKIPPED_SLOTS]==1u && s.words[OMNI_AD_REPEATED_CALLBACKS]==1u);
    assert(s.words[OMNI_AD_MAX_DELTA]==2u); /* boundary jitter, no missing callback */
    const uint32_t omitted[]={100u,101u,103u,104u};
    omni_audio_delivery_reset(&s);
    for(uint32_t i=0;i<4u;++i) omni_audio_delivery_observe(&s,192u,omitted[i],1000u+i);
    assert(s.words[OMNI_AD_ELAPSED_USB_FRAMES]==4u);
    assert(s.words[OMNI_AD_SKIPPED_SLOTS]==1u && s.words[OMNI_AD_REPEATED_CALLBACKS]==0u);
}

static void packet_histogram(void)
{
    omni_audio_delivery_t s; omni_audio_delivery_reset(&s);
    const uint32_t sizes[]={188u,192u,196u,16u,0u,3u};
    for(uint32_t i=0;i<6u;++i) omni_audio_delivery_observe(&s,sizes[i],i,i);
    assert(s.words[OMNI_AD_PACKETS]==6u && s.words[OMNI_AD_FRAMES]==148u);
    assert(s.words[OMNI_AD_PACKETS47]==1u && s.words[OMNI_AD_PACKETS48]==1u);
    assert(s.words[OMNI_AD_PACKETS49]==1u && s.words[OMNI_AD_OTHER_NONZERO]==2u);
    assert(s.words[OMNI_AD_ZERO]==1u && s.words[OMNI_AD_LAST_BYTES]==3u);
}

static void wrap_and_ambiguous_gap(void)
{
    omni_audio_delivery_t s; omni_audio_delivery_reset(&s);
    omni_audio_delivery_observe(&s,192u,2047u,UINT32_MAX);
    omni_audio_delivery_observe(&s,192u,0u,0u);
    assert(s.words[OMNI_AD_ELAPSED_USB_FRAMES]==1u);
    assert(s.words[OMNI_AD_AMBIGUOUS_LONG_GAPS]==0u);
    /* A2048ms pause returning to the same frame must not count as a duplicate. */
    omni_audio_delivery_observe(&s,192u,0u,2048u);
    assert(s.words[OMNI_AD_AMBIGUOUS_LONG_GAPS]==1u);
    assert(s.words[OMNI_AD_ELAPSED_USB_FRAMES]==1u && s.words[OMNI_AD_REPEATED_CALLBACKS]==0u);
    assert(s.words[OMNI_AD_SKIPPED_SLOTS]==0u && s.words[OMNI_AD_MAX_DELTA]==1u);
    omni_audio_delivery_observe(&s,188u,1u,2049u); /* new baseline was established */
    assert(s.words[OMNI_AD_ELAPSED_USB_FRAMES]==2u && s.words[OMNI_AD_PACKETS47]==1u);
    omni_audio_delivery_observe(&s,196u,2000u,8192u);
    assert(s.words[OMNI_AD_AMBIGUOUS_LONG_GAPS]==2u);
    assert(s.words[OMNI_AD_ELAPSED_USB_FRAMES]==2u && s.words[OMNI_AD_SKIPPED_SLOTS]==0u);
    assert(s.words[OMNI_AD_PACKETS]==5u && s.words[OMNI_AD_FRAMES]==240u);
    /* The documented2048ms boundary remains strict;2047ms can span2047 slots. */
    omni_audio_delivery_reset(&s);
    omni_audio_delivery_observe(&s,192u,0u,0u);
    omni_audio_delivery_observe(&s,192u,2047u,2047u);
    assert(s.words[OMNI_AD_ELAPSED_USB_FRAMES]==2047u && s.words[OMNI_AD_SKIPPED_SLOTS]==2046u);
    assert(s.words[OMNI_AD_AMBIGUOUS_LONG_GAPS]==0u);
}

static void format_geometry(void)
{
    omni_audio_delivery_t s;
    for(unsigned stride=4u;stride<=6u;stride+=2u) for(unsigned nominal=48u;nominal<=96u;nominal+=48u) {
        assert(omni_audio_delivery_reset_format(&s,stride,nominal));
        assert(s.words[0]==(stride==4u && nominal==48u?2u:3u));
        for(unsigned i=0;i<1002u;++i)
            omni_audio_delivery_observe(&s,(nominal-1u+i%3u)*stride,i,i);
        assert(s.words[OMNI_AD_FRAMES]==1002u*nominal);
        assert(s.words[OMNI_AD_PACKETS47]==334u && s.words[OMNI_AD_PACKETS48]==334u && s.words[OMNI_AD_PACKETS49]==334u);
        assert(!s.words[OMNI_AD_OTHER_NONZERO] && !s.words[OMNI_AD_SKIPPED_SLOTS]);
    }
    omni_audio_delivery_t saved=s;
    assert(!omni_audio_delivery_reset_format(&s,3u,48u));assert(!memcmp(&s,&saved,sizeof(s)));
    assert(!omni_audio_delivery_reset_format(&s,4u,44u));assert(!memcmp(&s,&saved,sizeof(s)));
    assert(!omni_audio_delivery_reset_format(NULL,4u,48u));
}

int main(void)
{
    _Static_assert(OMNI_AUDIO_DELIVERY_WORD_COUNT==15,"Diagnostic page has15 words");
    steady_and_reset(); timing_vs_loss(); packet_histogram(); wrap_and_ambiguous_gap();format_geometry();
    puts("Audio delivery: callback timing, packet sizes, wrap and ambiguous-gap tests passed");
    return 0;
}
