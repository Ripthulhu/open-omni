#include "audio_queue.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static omni_audio_queue_t q;
static uint8_t pcm[2048u*4u];
static uint32_t out[4096];
static void make(uint32_t first,uint32_t frames)
{
    for(uint32_t i=0;i<frames;++i) {
        uint32_t s=(first+i)&0xffffu;
        pcm[4u*i]=(uint8_t)s; pcm[4u*i+1u]=(uint8_t)(s>>8);
        s^=0xffffu; pcm[4u*i+2u]=(uint8_t)s; pcm[4u*i+3u]=(uint8_t)(s>>8);
    }
}
static void check(uint32_t first,uint32_t frames)
{
    for(uint32_t i=0;i<frames;++i) {
        uint32_t s=(first+i)&0xffffu;
        assert(out[2u*i]==s<<16);
        assert(out[2u*i+1u]==(s^0xffffu)<<16);
    }
}
static uint8_t pcm24[OMNI_AUDIO_QUEUE_FRAMES*6u];
static void make24(uint32_t first,uint32_t frames)
{
    for(uint32_t i=0;i<frames;++i) {
        uint32_t left=(first+i)&0xffffffu,right=left^0xffffffu;
        for(unsigned byte=0;byte<3u;++byte) {
            pcm24[6u*i+byte]=(uint8_t)(left>>(8u*byte));
            pcm24[6u*i+3u+byte]=(uint8_t)(right>>(8u*byte));
        }
    }
}
static void check24(uint32_t first,uint32_t frames)
{
    for(uint32_t i=0;i<frames;++i) {
        uint32_t sample=(first+i)&0xffffffu;
        assert(out[2u*i]==sample<<8);
        assert(out[2u*i+1u]==(sample^0xffffffu)<<8);
    }
}
static void formats24(void)
{
    omni_audio_format format;
    assert(omni_audio_format_make(&format,96000u,24u,123u));
    assert(omni_audio_queue_init_format(&q,0u,&format));
    assert(q.format.epoch==123u && q.format.frames_per_ms==96u);
    static const uint8_t golden[]={1,0,0,255,255,255,255,255,127,0,0,128,86,52,18,170,203,237};
    static const uint32_t expected[]={0x00000100u,0xffffff00u,0x7fffff00u,0x80000000u,0x12345600u,0xedcbaa00u};
    assert(omni_audio_queue_push(&q,golden,sizeof(golden))==3u);
    assert(omni_audio_queue_render(&q,out,3u)==3u);
    assert(!memcmp(expected,out,sizeof(expected)));
    /* A format copy is latched; a caller's next epoch cannot alter decoding. */
    format.sample_bits=16u;
    assert(q.format.sample_bits==24u);
    uint32_t accepted=q.accepted,w=atomic_load(&q.written),r=atomic_load(&q.read);
    assert(!omni_audio_queue_push(&q,golden,5u));
    assert(!omni_audio_queue_push(&q,golden,7u));
    assert(!omni_audio_queue_push(&q,NULL,6u));
    assert(!omni_audio_queue_push(NULL,golden,6u));
    assert(!omni_audio_queue_init_format(&q,0u,&format));
    assert(!omni_audio_queue_init_format(&q,0u,NULL));
    assert(!omni_audio_queue_init_format(NULL,0u,&q.format));
    assert(!omni_audio_queue_init_format(&q,OMNI_AUDIO_QUEUE_FRAMES+1u,&q.format));
    assert(q.accepted==accepted && atomic_load(&q.written)==w && atomic_load(&q.read)==r);
    assert(q.format.sample_bits==24u && q.format.epoch==123u);
    assert(omni_audio_format_make(&format,48000u,24u,124u));
    assert(omni_audio_queue_init_format(&q,0u,&format));
    /* Both ring wrap and monotonic32-bit counter wrap preserve six-byte frames. */
    atomic_store(&q.written,UINT32_MAX-31u);atomic_store(&q.read,UINT32_MAX-31u);
    make24(0x7fffe0u,OMNI_AUDIO_QUEUE_FRAMES);
    assert(omni_audio_queue_push(&q,pcm24,sizeof(pcm24))==OMNI_AUDIO_QUEUE_FRAMES);
    uint32_t last=atomic_load(&q.written);
    assert(!omni_audio_queue_push(&q,golden,6u));
    assert(atomic_load(&q.written)==last && q.overflows==1u && q.rejected==1u);
    assert(omni_audio_queue_render(&q,out,OMNI_AUDIO_QUEUE_FRAMES)==OMNI_AUDIO_QUEUE_FRAMES);
    check24(0x7fffe0u,OMNI_AUDIO_QUEUE_FRAMES);
    /* Consumer preemption between L/R writes must not observe a half frame. */
    assert(omni_audio_queue_init_format(&q,0u,&format));
    q.pcm[0]=0x12345600u;
    assert(omni_audio_queue_render(&q,out,1u)==0u && out[0]==0u && out[1]==0u);
    q.pcm[1]=0xffabcd00u;
    atomic_store_explicit(&q.written,1u,memory_order_release);
    assert(omni_audio_queue_render(&q,out,1u)==1u);
    assert(out[0]==0x12345600u && out[1]==0xffabcd00u);
    /* At96k,1024 silence frames cover six-packet bursts while preserving
     * enough capacity for the incoming576-frame group over many wraps. */
    assert(omni_audio_format_make(&format,96000u,24u,125u));
    assert(omni_audio_queue_init_format(&q,1024u,&format));
    uint32_t supplied=0u,seen=0u,prime=1024u;
    for(uint32_t ms=0u;ms<12000u;++ms) {
        if(ms%6u==0u) {
            make24(supplied,576u);
            assert(omni_audio_queue_push(&q,pcm24,3456u)==576u);supplied+=576u;
        }
        assert(omni_audio_queue_render(&q,out,96u)==96u);
        for(unsigned i=0u;i<96u;++i) {
            if(prime) {assert(!out[2u*i] && !out[2u*i+1u]);--prime;}
            else {
                uint32_t sample=seen++&0xffffffu;
                assert(out[2u*i]==sample<<8);
                assert(out[2u*i+1u]==(sample^0xffffffu)<<8);
            }
        }
    }
    assert(!q.underruns && !q.overflows);
    /* PCM16 also supports96k, keeping its exact left-aligned representation. */
    assert(omni_audio_format_make(&format,96000u,16u,126u));
    assert(omni_audio_queue_init_format(&q,0u,&format));
    make(32760u,96u);assert(omni_audio_queue_push(&q,pcm,384u)==96u);
    assert(omni_audio_queue_render(&q,out,96u)==96u);check(32760u,96u);
}
int main(void)
{
    /* All signed PCM16 patterns survive bit-exact channel packing. */
    omni_audio_queue_init(&q,0);
    for(uint32_t i=0;i<65536u;i+=256u) {
        make(i,256); assert(omni_audio_queue_push(&q,pcm,1024)==256);
        assert(omni_audio_queue_render(&q,out,256)==256); check(i,256);
    }
    assert(!q.underruns && !q.overflows);
    /* Full capacity, reject entire packet and retain every unread frame. */
    omni_audio_queue_init(&q,0); make(700,2048);
    assert(omni_audio_queue_push(&q,pcm,sizeof(pcm))==2048);
    assert(omni_audio_queue_push(&q,pcm,192)==0);
    assert(q.overflows==1 && q.rejected==48);
    assert(omni_audio_queue_render(&q,out,2048)==2048); check(700,2048);
    /* Starvation emits silence; it never advances the reader past the writer. */
    assert(omni_audio_queue_render(&q,out,48)==0);
    for(unsigned i=0;i<96;++i) assert(!out[i]);
    assert(q.underruns==1 && q.silence==48 && !omni_audio_queue_fill(&q));
    make(800,48); assert(omni_audio_queue_push(&q,pcm,192)==48);
    assert(omni_audio_queue_render(&q,out,48)==48); check(800,48);
    /* Partial starvation preserves its valid prefix and zeros every missing
     * frame without writing outside the caller's 96-word DMA block. */
    const uint32_t partial_cases[]={1u,47u};
    for(unsigned c=0;c<sizeof(partial_cases)/sizeof(partial_cases[0]);++c) {
        uint32_t available=partial_cases[c],guarded[98];
        omni_audio_queue_init(&q,0); make(32767u,available);
        assert(omni_audio_queue_push(&q,pcm,available*4u)==available);
        for(unsigned i=0;i<98;++i) guarded[i]=0xa5a55a5au;
        assert(omni_audio_queue_render(&q,guarded+1,48)==available);
        assert(guarded[0]==0xa5a55a5au && guarded[97]==0xa5a55a5au);
        for(uint32_t i=0;i<48u;++i) {
            if(i<available) {
                uint32_t s=(32767u+i)&0xffffu;
                assert(guarded[1u+2u*i]==s<<16);
                assert(guarded[2u+2u*i]==(s^0xffffu)<<16);
            } else assert(!guarded[1u+2u*i] && !guarded[2u+2u*i]);
        }
        assert(q.accepted==available && q.consumed==available);
        assert(q.silence==48u-available && q.underruns==1u);
        assert(!q.overflows && !q.rejected && !omni_audio_queue_fill(&q));
        assert(atomic_load(&q.read)==available && atomic_load(&q.written)==available);
    }
    /* Writer is 24 frames before physical wrap. A 48-frame packet must be
     * rejected with only 47 free; consume one frame, then exactly 48 fit.
     * Verify the rejected packet did not overwrite the oldest unread frame. */
    omni_audio_queue_init(&q,0);
    atomic_store(&q.written,23u); atomic_store(&q.read,23u);
    make(1000u,OMNI_AUDIO_QUEUE_FRAMES-47u);
    assert(omni_audio_queue_push(&q,pcm,(OMNI_AUDIO_QUEUE_FRAMES-47u)*4u)==OMNI_AUDIO_QUEUE_FRAMES-47u);
    assert(atomic_load(&q.written)==OMNI_AUDIO_QUEUE_FRAMES-24u);
    make(1000u+OMNI_AUDIO_QUEUE_FRAMES-47u,48u);
    assert(omni_audio_queue_push(&q,pcm,192u)==0u);
    assert(atomic_load(&q.written)==OMNI_AUDIO_QUEUE_FRAMES-24u);
    assert(atomic_load(&q.read)==23u && q.overflows==1u && q.rejected==48u);
    assert(omni_audio_queue_render(&q,out,1u)==1u); check(1000u,1u);
    assert(omni_audio_queue_fill(&q)==OMNI_AUDIO_QUEUE_FRAMES-48u);
    assert(omni_audio_queue_push(&q,pcm,192u)==48u);
    assert(omni_audio_queue_fill(&q)==OMNI_AUDIO_QUEUE_FRAMES);
    assert(omni_audio_queue_render(&q,out,OMNI_AUDIO_QUEUE_FRAMES)==OMNI_AUDIO_QUEUE_FRAMES);
    check(1001u,OMNI_AUDIO_QUEUE_FRAMES);
    assert(q.accepted==OMNI_AUDIO_QUEUE_FRAMES+1u && q.consumed==q.accepted);
    assert(!q.silence && !q.underruns && !omni_audio_queue_fill(&q));
    /* Monotonic counters wrap at UINT32_MAX without losing occupancy/ordering. */
    omni_audio_queue_init(&q,0);
    atomic_store(&q.written,UINT32_MAX-31u); atomic_store(&q.read,UINT32_MAX-31u);
    make(65520,96); assert(omni_audio_queue_push(&q,pcm,384)==96);
    assert(omni_audio_queue_fill(&q)==96);
    assert(omni_audio_queue_render(&q,out,96)==96); check(65520,96);
    /* DMA seeing a USB copy before publication must see silence, not torn data. */
    omni_audio_queue_init(&q,0); q.pcm[0]=0xabcd0000u;q.pcm[1]=0x12340000u;
    assert(omni_audio_queue_render(&q,out,1)==0 && out[0]==0 && out[1]==0);
    atomic_store_explicit(&q.written,1,memory_order_release);
    assert(omni_audio_queue_render(&q,out,1)==1);
    assert(out[0]==0xabcd0000u && out[1]==0x12340000u);
    /* Bursty delivery: six packets together every6ms, consumer every1ms.
     * The prime covers the delivery gap; samples remain ordered across wraps. */
    omni_audio_queue_init(&q,512);
    uint32_t supplied=0,seen=0,prime=512;
    for(uint32_t ms=0;ms<120000;++ms) {
        if(ms%6u==0) { make(supplied,288); assert(omni_audio_queue_push(&q,pcm,1152)==288); supplied+=288; }
        assert(omni_audio_queue_render(&q,out,48)==48);
        for(unsigned i=0;i<48;++i) {
            if(prime) { assert(!out[2*i] && !out[2*i+1]); --prime; }
            else { uint32_t s=seen++&0xffffu; assert(out[2*i]==s<<16); assert(out[2*i+1]==(s^0xffffu)<<16); }
        }
    }
    assert(!q.underruns && !q.overflows);
    assert(!omni_audio_queue_push(&q,NULL,4)); assert(!omni_audio_queue_push(&q,pcm,3));
    formats24();
    puts("audio queue: PCM16/24,48/96k, capacity, starvation/canaries, frame publication, counter/ring wrap and bursts passed");
    return 0;
}
