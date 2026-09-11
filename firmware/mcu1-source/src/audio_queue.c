#include "audio_queue.h"
#include <string.h>
#define MASK (OMNI_AUDIO_QUEUE_FRAMES - 1u)
_Static_assert((OMNI_AUDIO_QUEUE_FRAMES & MASK) == 0u, "queue must be power of two");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && sizeof(atomic_uint_least32_t)==4,
    "nested IRQ queue requires lock-free32bit atomics");

void omni_audio_queue_init(omni_audio_queue_t *q, uint32_t silence_prime)
{
    omni_audio_format format;
    (void)omni_audio_format_make(&format,48000u,16u,0u);
    (void)omni_audio_queue_init_format(q,silence_prime<=OMNI_AUDIO_QUEUE_FRAMES?silence_prime:0u,&format);
}
bool omni_audio_queue_init_format(omni_audio_queue_t *q, uint32_t silence_prime,
                                   const omni_audio_format *format)
{
    if(!q || !omni_audio_format_valid(format) || silence_prime>OMNI_AUDIO_QUEUE_FRAMES) return false;
    q->format=*format;
    memset(q->pcm, 0, sizeof(q->pcm));
    atomic_init(&q->written, silence_prime);
    atomic_init(&q->read, 0u);
    q->accepted=q->rejected=q->overflows=q->consumed=q->silence=q->underruns=0u;
    return true;
}
uint32_t omni_audio_queue_fill(const omni_audio_queue_t *q)
{
    /* Caller uses its owned index plus the peer's published index. A third
     * observer must exclude both contexts when taking a telemetry snapshot. */
    uint32_t w=atomic_load_explicit(&q->written,memory_order_acquire);
    uint32_t r=atomic_load_explicit(&q->read,memory_order_acquire);
    return w-r;
}
uint32_t omni_audio_queue_push(omni_audio_queue_t *q, const uint8_t *pcm, uint32_t bytes)
{
    if (!q || !pcm || !bytes || !omni_audio_format_valid(&q->format) || bytes%q->format.frame_bytes) return 0u;
    uint32_t frames=bytes/q->format.frame_bytes;
    uint32_t w=atomic_load_explicit(&q->written,memory_order_relaxed);
    uint32_t r=atomic_load_explicit(&q->read,memory_order_acquire);
    uint32_t fill=w-r;
    if (fill>OMNI_AUDIO_QUEUE_FRAMES || frames>OMNI_AUDIO_QUEUE_FRAMES-fill) {
        q->rejected+=frames; ++q->overflows; return 0u;
    }
    for(uint32_t i=0;i<frames;++i) {
        const uint8_t *s=pcm+(uint32_t)q->format.frame_bytes*i;
        uint32_t index=((w+i)&MASK)*2u;
        if(q->format.sample_bits==24u) {
            q->pcm[index]=((uint32_t)s[0]<<8)|((uint32_t)s[1]<<16)|((uint32_t)s[2]<<24);
            q->pcm[index+1u]=((uint32_t)s[3]<<8)|((uint32_t)s[4]<<16)|((uint32_t)s[5]<<24);
        } else {
            q->pcm[index]=((uint32_t)s[0]<<16)|((uint32_t)s[1]<<24);
            q->pcm[index+1u]=((uint32_t)s[2]<<16)|((uint32_t)s[3]<<24);
        }
    }
    /* DMA may preempt above; unpublished data is never consumed. */
    atomic_store_explicit(&q->written,w+frames,memory_order_release);
    q->accepted+=frames;
    return frames;
}
uint32_t omni_audio_queue_render(omni_audio_queue_t *q, uint32_t *slots, uint32_t frames)
{
    uint32_t r=atomic_load_explicit(&q->read,memory_order_relaxed);
    uint32_t w=atomic_load_explicit(&q->written,memory_order_acquire);
    uint32_t available=w-r;
    uint32_t take=available<frames?available:frames;
    if (available>OMNI_AUDIO_QUEUE_FRAMES) take=0u;
    for(uint32_t i=0;i<take;++i) {
        uint32_t index=((r+i)&MASK)*2u;
        slots[2u*i]=q->pcm[index];
        slots[2u*i+1u]=q->pcm[index+1u];
    }
    for(uint32_t i=take;i<frames;++i) { slots[2u*i]=0u; slots[2u*i+1u]=0u; }
    atomic_store_explicit(&q->read,r+take,memory_order_release);
    q->consumed+=take;
    if(take<frames) { q->silence+=frames-take; ++q->underruns; }
    return take;
}
