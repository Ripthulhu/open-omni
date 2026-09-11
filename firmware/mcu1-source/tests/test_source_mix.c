#include "source_mix.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static uint32_t reference(uint32_t word,unsigned coefficient)
{
    if(coefficient==16384u) return word;
    int64_t sample=(int64_t)(word>>16);
    if(sample>=32768) sample-=65536;
    int64_t product=sample*coefficient;
    int64_t magnitude=product<0?-product:product;
    int64_t result=(magnitude+8192)/16384;
    if(product<0) result=-result;
    return ((uint32_t)result&65535u)<<16;
}
static unsigned next(unsigned current,unsigned target)
{
    if(current<target) return target-current<32u?target:current+32u;
    if(current>target) return current-target<32u?target:current-32u;
    return current;
}
static void controls(void)
{
    static const uint16_t table[]={0,206,731,1460,2314,3268,4617,5813,7318,8963,11598,13014,16384};
    omni_source_mix state;omni_source_mix_init(&state);
    assert(state.position==12 && !state.bias_mode && !state.revision);
    assert(!omni_source_mix_dial(&state,1));assert(state.position==12);
    omni_source_mix_gains gains;
    for(unsigned pos=0;pos<=24;pos++) {
        assert(omni_source_mix_configure(&state,pos));
        assert(omni_source_mix_snapshot(&state,&gains));
        unsigned a=pos<=12?12:24-pos,b=pos<=12?pos:12;
        assert(gains.index[0]==a && gains.index[1]==b);
        assert(gains.q14[0]==table[a] && gains.q14[1]==table[b]);
        omni_source_mix_gains mirror;
        assert(omni_source_mix_configure(&state,24-pos));
        assert(omni_source_mix_snapshot(&state,&mirror));
        assert(gains.q14[0]==mirror.q14[1] && gains.q14[1]==mirror.q14[0]);
    }
    unsigned before=state.position;uint32_t revision=state.revision;
    assert(!omni_source_mix_configure(&state,25));
    assert(!omni_source_mix_configure(&state,UINT_MAX));
    assert(state.position==before && state.revision==revision);
    assert(omni_source_mix_configure(&state,before));assert(state.revision==revision);
    omni_source_mix_toggle(&state);assert(state.bias_mode && state.revision==revision+1u);
    assert(omni_source_mix_dial(&state,INT_MAX));assert(state.position==24);
    revision=state.revision;
    assert(omni_source_mix_dial(&state,1));assert(state.position==24 && state.revision==revision);
    assert(omni_source_mix_dial(&state,INT_MIN));assert(state.position==0);
    assert(omni_source_mix_dial(&state,12));assert(state.position==12);
    assert(omni_source_mix_dial(&state,-1));assert(state.position==11);
    assert(omni_source_mix_dial(&state,0));assert(state.position==11);
    omni_source_mix_toggle(&state);assert(!state.bias_mode);
    assert(!omni_source_mix_dial(&state,INT_MIN));assert(state.position==11);
    state.bias_mode=true;state.position=255;
    assert(!omni_source_mix_dial(&state,0));assert(!omni_source_mix_snapshot(&state,&gains));
    omni_source_mix_init(NULL);omni_source_mix_toggle(NULL);
    assert(!omni_source_mix_configure(NULL,12));assert(!omni_source_mix_dial(NULL,1));
    assert(!omni_source_mix_snapshot(NULL,&gains));assert(!omni_source_mix_snapshot(&state,NULL));
}
static void arithmetic(void)
{
    /* Exhaustive signed16 domain at every stock step, including negative
     * extrema, L/R symmetry, exact silence and lower-padding preservation. */
    static const unsigned levels[]={0,206,731,1460,2314,3268,4617,5813,7318,8963,11598,13014,16384};
    omni_source_mix_ramp ramp;
    for(unsigned gain=0;gain<sizeof(levels)/sizeof(levels[0]);gain++) {
        assert(omni_source_mix_ramp_init(&ramp,levels[gain]));
        for(unsigned value=0;value<65536u;value++) {
            uint32_t original=(value<<16)|(value^0x55aau);
            uint32_t samples[2]={original,original};
            assert(omni_source_mix_apply(&ramp,levels[gain],samples,1));
            assert(samples[0]==reference(original,levels[gain]));
            assert(samples[0]==samples[1]);
        }
    }
    /* Nearest rounding is symmetric at exact half-sample boundaries. */
    assert(omni_source_mix_ramp_init(&ramp,8192));
    uint32_t tie[]={0x00010000u,0xffff0000u};
    assert(omni_source_mix_apply(&ramp,8192,tie,1));
    assert(tie[0]==0x00010000u && tie[1]==0xffff0000u);
}
static void ramps(void)
{
    uint32_t words[OMNI_SOURCE_MIX_MAX_FRAMES*2u];
    unsigned current=16384;
    omni_source_mix_ramp ramp;assert(omni_source_mix_ramp_init(&ramp,current));
    for(unsigned i=0;i<OMNI_SOURCE_MIX_MAX_FRAMES;i++) {
        words[2*i]=0x7fff0000u;words[2*i+1]=0x80000000u;
    }
    assert(omni_source_mix_apply(&ramp,0,words,OMNI_SOURCE_MIX_MAX_FRAMES));
    assert(ramp.current_q14==0);
    for(unsigned i=0;i<OMNI_SOURCE_MIX_MAX_FRAMES;i++) {
        unsigned previous=current;current=next(current,0);
        assert(previous-current<=32u);
        assert(words[2*i]==reference(0x7fff0000u,current));
        assert(words[2*i+1]==reference(0x80000000u,current));
        if(i>=511u) assert(words[2*i]==0 && words[2*i+1]==0);
    }
    uint32_t whole[1024],chunks[1024];
    for(unsigned i=0;i<1024;i++) whole[i]=chunks[i]=0x4000a5a5u;
    omni_source_mix_ramp split;assert(omni_source_mix_ramp_init(&split,0));
    assert(omni_source_mix_apply(&ramp,16384,whole,512));
    /* The actual1ms native DMA blocks contain48 stereo frames. Splitting at
     * these boundaries cannot change a sample or restart the gain ramp. */
    for(unsigned done=0;done<512;) {
        unsigned frames=512-done;if(frames>48) frames=48;
        assert(omni_source_mix_apply(&split,16384,chunks+2*done,frames));done+=frames;
    }
    assert(!memcmp(whole,chunks,sizeof(whole)));
    assert(ramp.current_q14==16384 && split.current_q14==16384);
    assert(whole[1022]==0x4000a5a5u && whole[1023]==0x4000a5a5u);
    /* Reversing direction mid-ramp is bounded and uses one coefficient for
     * both channels, even after a target that is not divisible by32. */
    current=16384;
    for(unsigned target_index=0;target_index<20;target_index++) {
        unsigned target=target_index&1u?206u:13014u;
        for(unsigned i=0;i<96;i++) words[i]=0x12345678u;
        assert(omni_source_mix_apply(&ramp,target,words,48));
        for(unsigned i=0;i<48;i++) {
            unsigned previous=current;current=next(current,target);
            assert((previous>current?previous-current:current-previous)<=32u);
            assert(words[2*i]==words[2*i+1]);
            assert(words[2*i]==reference(0x12345678u,current));
        }
        assert(ramp.current_q14==current);
    }
}
static void invalid_and_unity(void)
{
    struct {uint32_t before,words[2048],after;} block;
    block.before=0x87654321u;block.after=0x12345678u;
    for(unsigned i=0;i<2048;i++) block.words[i]=0x12345678u^i;
    uint32_t copy[2048];memcpy(copy,block.words,sizeof(copy));
    omni_source_mix_ramp r;assert(omni_source_mix_ramp_init(&r,16384));
    assert(omni_source_mix_apply(&r,16384,block.words,1024));
    assert(!memcmp(copy,block.words,sizeof(copy)));
    assert(!omni_source_mix_apply(&r,0,block.words,1025));
    assert(!omni_source_mix_apply(&r,0,block.words,SIZE_MAX));
    assert(!omni_source_mix_apply(&r,16385,block.words,1));
    assert(!omni_source_mix_apply(&r,UINT_MAX,block.words,1));
    assert(!omni_source_mix_apply(&r,0,NULL,1));
    assert(!omni_source_mix_apply(NULL,0,block.words,1));
    assert(!omni_source_mix_ramp_init(&r,16385));assert(r.current_q14==16384);
    assert(!omni_source_mix_ramp_init(NULL,0));
    assert(!memcmp(copy,block.words,sizeof(copy)));
    assert(omni_source_mix_apply(&r,0,NULL,0));assert(r.current_q14==16384);
    r.current_q14=65535;assert(!omni_source_mix_apply(&r,0,block.words,1));
    assert(!memcmp(copy,block.words,sizeof(copy)));
    assert(omni_source_mix_ramp_init(&r,0));
    assert(omni_source_mix_apply(&r,0,block.words,1024));
    for(unsigned i=0;i<2048;i++) assert(block.words[i]==0);
    assert(block.before==0x87654321u && block.after==0x12345678u);
}
static uint32_t reference24(uint32_t word,unsigned gain)
{
    if(gain==16384u) return word;
    int64_t sample=(int64_t)(word>>8);
    if(sample>=8388608) sample-=16777216;
    int64_t magnitude=(sample<0?-sample:sample)*gain;
    int64_t rounded=(magnitude+8192)/16384;
    if(sample<0) rounded=-rounded;
    return ((uint32_t)rounded&0xffffffu)<<8;
}
static void pcm24_arithmetic(void)
{
    omni_source_mix_ramp ramp;
    assert(omni_source_mix_ramp_init(&ramp,8192u));
    uint32_t golden[]={0x00000100u,0xffffff00u,0x00010100u,0xfffeff00u,
                       0x7fffff00u,0x80000000u,0x12345600u,0xedcbaa00u};
    static const uint32_t expected[]={0x00000100u,0xffffff00u,0x00008100u,0xffff7f00u,
                                      0x40000000u,0xc0000000u,0x091a2b00u,0xf6e5d500u};
    assert(omni_source_mix_apply_format(&ramp,8192u,golden,4u,24u,96u));
    assert(!memcmp(golden,expected,sizeof(expected)));
    /* Exercise every possible low16-bit pattern at positive/negative24-bit
     * extrema and nearzero. A PCM16 truncation path fails the low-bit cases. */
    static const unsigned gains[]={0u,206u,731u,8192u,13014u,16383u,16384u};
    for(unsigned g=0u;g<sizeof(gains)/sizeof(gains[0]);++g) {
        assert(omni_source_mix_ramp_init(&ramp,gains[g]));
        for(uint32_t low=0u;low<65536u;++low) {
            uint32_t original[4]={(low<<8)|0x5au,((0xff0000u|low)<<8)|0xa5u,
                                   ((0x7f0000u|low)<<8)|0x3cu,((0x800000u|low)<<8)|0xc3u};
            uint32_t words[4];memcpy(words,original,sizeof(words));
            assert(omni_source_mix_apply_format(&ramp,gains[g],words,2u,24u,48u));
            for(unsigned i=0u;i<4u;++i) assert(words[i]==reference24(original[i],gains[g]));
        }
    }
    uint32_t untouched[]={0x7fffff5au,0x800000a5u};
    uint32_t copy[2];memcpy(copy,untouched,sizeof(copy));
    assert(omni_source_mix_ramp_init(&ramp,16384u));
    assert(!omni_source_mix_apply_format(&ramp,0u,untouched,1u,20u,96u));
    assert(!omni_source_mix_apply_format(&ramp,0u,untouched,1u,24u,44u));
    assert(!omni_source_mix_apply_format(&ramp,0u,untouched,1u,24u,0u));
    assert(!omni_source_mix_apply_format(&ramp,0u,untouched,SIZE_MAX,24u,96u));
    assert(!omni_source_mix_apply_format(&ramp,16385u,untouched,1u,24u,96u));
    assert(!memcmp(untouched,copy,sizeof(copy)) && ramp.current_q14==16384u);
    assert(omni_source_mix_apply_format(&ramp,16384u,untouched,1u,24u,96u));
    assert(!memcmp(untouched,copy,sizeof(copy)));
    assert(omni_source_mix_apply_format(&ramp,0u,NULL,0u,24u,96u));
    assert(ramp.current_q14==16384u);
}
static void rate96_ramps(void)
{
    omni_source_mix_ramp slow,fast;
    assert(omni_source_mix_ramp_init(&slow,16384u));
    assert(omni_source_mix_ramp_init(&fast,16384u));
    uint32_t at48[96],at96[192];
    /* Equal wall-time progression, for both downward and upward changes. */
    for(unsigned direction=0u;direction<2u;++direction) {
        unsigned target=direction?16384u:0u;
        for(unsigned ms=0u;ms<11u;++ms) {
            for(unsigned i=0u;i<96u;++i) at48[i]=0x12345700u;
            for(unsigned i=0u;i<192u;++i) at96[i]=0x12345700u;
            unsigned before=fast.current_q14;
            assert(omni_source_mix_apply_format(&slow,target,at48,48u,24u,48u));
            assert(omni_source_mix_apply_format(&fast,target,at96,96u,24u,96u));
            assert(slow.current_q14==fast.current_q14);
            for(unsigned frame=0u;frame<96u;++frame) {
                if(before<target) before+=target-before<16u?target-before:16u;
                else if(before>target) before-=before-target<16u?before-target:16u;
                assert(at96[2u*frame]==reference24(0x12345700u,before));
                assert(at96[2u*frame]==at96[2u*frame+1u]);
            }
        }
        assert(slow.current_q14==target && fast.current_q14==target);
    }
    /* Chunk boundaries do not alter gain timing or fractional24-bit samples. */
    uint32_t whole[2048],chunks[2048];
    for(unsigned i=0u;i<2048u;++i) whole[i]=chunks[i]=i&1u?0xfffedc00u:0x7fffff00u;
    assert(omni_source_mix_apply_format(&fast,0u,whole,1024u,24u,96u));
    for(unsigned done=0u;done<1024u;) {
        unsigned count=1024u-done;if(count>97u) count=97u;
        assert(omni_source_mix_apply_format(&slow,0u,chunks+done*2u,count,24u,96u));done+=count;
    }
    assert(!memcmp(whole,chunks,sizeof(whole)) && fast.current_q14==0u && slow.current_q14==0u);
    assert(whole[2046]==0u && whole[2047]==0u && whole[2044]!=0u);
}
int main(void)
{
    controls();arithmetic();ramps();invalid_and_unity();
    pcm24_arithmetic();rate96_ramps();
    puts("source mix tests passed (PCM16 exhaustive,PCM24 lowbits/extrema,48/96k stereo ramps, controls, bounds)");
    return 0;
}
