#ifndef OMNI_AUDIO_DELIVERY_H
#define OMNI_AUDIO_DELIVERY_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    OMNI_AD_VERSION, OMNI_AD_PACKETS, OMNI_AD_FRAMES, OMNI_AD_ELAPSED_USB_FRAMES,
    OMNI_AD_SKIPPED_SLOTS, OMNI_AD_REPEATED_CALLBACKS, OMNI_AD_MAX_DELTA,
    OMNI_AD_PACKETS47, OMNI_AD_PACKETS48, OMNI_AD_PACKETS49,
    OMNI_AD_OTHER_NONZERO, OMNI_AD_ZERO, OMNI_AD_LAST_USB_FRAME,
    OMNI_AD_LAST_BYTES, OMNI_AD_AMBIGUOUS_LONG_GAPS, OMNI_AUDIO_DELIVERY_WORD_COUNT
};
typedef struct {
    uint32_t words[OMNI_AUDIO_DELIVERY_WORD_COUNT];
    uint32_t previous_uptime_ms;
    uint8_t frame_bytes,nominal_frames;
    bool have_previous;
} omni_audio_delivery_t;

/* One USB callback producer. Reset only while that producer cannot run; copy
 * words with its IRQ excluded. Both ABIs use exactly the 15 words above.
 * ABI2 retains stereo 16/48k semantics. ABI3 supports packed stereo 16/24 at
 * 48/96k: histogram slots 7..9 mean nominal-1, nominal, nominal+1 frames/ms.
 * Read diagnostic 74 ring format for the matching nominal rate and stride.
 * Counters wrap modulo 2^32. Reset must precede the first observation.
 * SOF deltas describe callback timing, not proof of bus packet loss: a callback
 * crossing a SOF boundary can cause one apparent skip followed by a repeat. */
static inline bool omni_audio_delivery_reset_format(omni_audio_delivery_t *stats,
    unsigned frame_bytes,unsigned nominal_frames)
{
    if(!stats || (frame_bytes!=4u && frame_bytes!=6u) ||
       (nominal_frames!=48u && nominal_frames!=96u)) return false;
    memset(stats,0,sizeof(*stats));
    stats->frame_bytes=(uint8_t)frame_bytes;stats->nominal_frames=(uint8_t)nominal_frames;
    stats->words[OMNI_AD_VERSION]=frame_bytes==4u && nominal_frames==48u?2u:3u;
    return true;
}
static inline void omni_audio_delivery_reset(omni_audio_delivery_t *stats)
{
    (void)omni_audio_delivery_reset_format(stats,4u,48u);
}

static inline void omni_audio_delivery_observe(omni_audio_delivery_t *stats,
    uint32_t bytes,uint32_t usb_frame,uint32_t uptime_ms)
{
    uint32_t *w=stats->words;
    uint32_t frames=bytes/stats->frame_bytes;
    usb_frame&=2047u;
    ++w[OMNI_AD_PACKETS]; w[OMNI_AD_FRAMES]+=frames;
    if(bytes==0u) ++w[OMNI_AD_ZERO];
    else if(frames==(uint32_t)stats->nominal_frames-1u) ++w[OMNI_AD_PACKETS47];
    else if(frames==stats->nominal_frames) ++w[OMNI_AD_PACKETS48];
    else if(frames==(uint32_t)stats->nominal_frames+1u) ++w[OMNI_AD_PACKETS49];
    else ++w[OMNI_AD_OTHER_NONZERO];
    if(stats->have_previous) {
        if((uint32_t)(uptime_ms-stats->previous_uptime_ms)>=2048u) {
            /* One or more entire USB frame-number cycles may have elapsed.
             * Do not turn the remaining modulo delta into inferred delivery. */
            ++w[OMNI_AD_AMBIGUOUS_LONG_GAPS];
        } else {
            uint32_t delta=(usb_frame-w[OMNI_AD_LAST_USB_FRAME])&2047u;
            w[OMNI_AD_ELAPSED_USB_FRAMES]+=delta;
            if(delta==0u) ++w[OMNI_AD_REPEATED_CALLBACKS];
            else if(delta>1u) w[OMNI_AD_SKIPPED_SLOTS]+=delta-1u;
            if(delta>w[OMNI_AD_MAX_DELTA]) w[OMNI_AD_MAX_DELTA]=delta;
        }
    }
    stats->have_previous=true;
    stats->previous_uptime_ms=uptime_ms;
    w[OMNI_AD_LAST_USB_FRAME]=usb_frame; w[OMNI_AD_LAST_BYTES]=bytes;
}
#endif
