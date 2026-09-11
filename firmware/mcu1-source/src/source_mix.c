#include "source_mix.h"
#include <string.h>

/* Recovered identical 13-step coefficients in stock MCU1 and MCU2. Preserve
 * indices for the peer protocol; max 16383 is represented locally by 16384 so
 * the unity path remains bit-exact. This table is source bias, not master dB. */
static const uint16_t coefficients[13]={
    0,206,731,1460,2314,3268,4617,5813,7318,8963,11598,13014,16383
};
void omni_source_mix_init(omni_source_mix *s)
{
    if(!s) return;
    s->position=OMNI_SOURCE_MIX_CENTER;s->bias_mode=false;s->revision=0;
}
bool omni_source_mix_configure(omni_source_mix *s,unsigned position)
{
    if(!s || position>OMNI_SOURCE_MIX_MAX_POSITION) return false;
    if(s->position!=position) {s->position=(uint8_t)position;++s->revision;}
    return true;
}
void omni_source_mix_toggle(omni_source_mix *s)
{
    if(!s) return;
    s->bias_mode=!s->bias_mode;++s->revision;
}
bool omni_source_mix_dial(omni_source_mix *s,int steps)
{
    if(!s || !s->bias_mode || s->position>OMNI_SOURCE_MIX_MAX_POSITION) return false;
    unsigned next;
    if(steps>(int)(OMNI_SOURCE_MIX_MAX_POSITION-s->position)) next=OMNI_SOURCE_MIX_MAX_POSITION;
    else if(steps<-(int)s->position) next=0;
    else next=(unsigned)((int)s->position+steps);
    (void)omni_source_mix_configure(s,next);return true;
}
bool omni_source_mix_snapshot(const omni_source_mix *s,omni_source_mix_gains *out)
{
    if(!s || !out || s->position>OMNI_SOURCE_MIX_MAX_POSITION) return false;
    unsigned pos=s->position;
    out->index[0]=(uint8_t)(pos<=12u?12u:24u-pos);
    out->index[1]=(uint8_t)(pos<=12u?pos:12u);
    for(unsigned i=0;i<2u;i++) out->q14[i]=out->index[i]==12u?
        OMNI_SOURCE_MIX_UNITY:coefficients[out->index[i]];
    return true;
}
bool omni_source_mix_ramp_init(omni_source_mix_ramp *r,unsigned initial)
{
    if(!r || initial>OMNI_SOURCE_MIX_UNITY) return false;
    r->current_q14=(uint16_t)initial;return true;
}
static uint32_t scaled(uint32_t word,unsigned gain)
{
    /* Explicit sign extension and unsigned repacking avoid implementation-
     * defined signed shifts/casts. Product magnitude <=536870912 fits int32. */
    int32_t sample=(int32_t)(word>>16);
    if(sample>=32768) sample-=65536;
    int32_t product=sample*(int32_t)gain;
    int32_t rounded=product>=0?(product+8192)/16384:-((-product+8192)/16384);
    return ((uint32_t)rounded&0xffffu)<<16;
}
bool omni_source_mix_apply(omni_source_mix_ramp *r,unsigned target,
                           uint32_t *words,size_t frames)
{
    return omni_source_mix_apply_format(r,target,words,frames,16u,48u);
}
static uint32_t scaled24(uint32_t word,unsigned gain)
{
    /* The 24-bit sample may exceed a signed 32-bit product. Sign-extend with
     * subtraction; never right-shift a signed negative or overflow int32. */
    int64_t sample=(int64_t)(word>>8);
    if(sample>=8388608) sample-=16777216;
    int64_t product=sample*(int64_t)gain;
    int64_t rounded=product>=0?(product+8192)/16384:-((-product+8192)/16384);
    return ((uint32_t)rounded&0xffffffu)<<8;
}
bool omni_source_mix_apply_format(omni_source_mix_ramp *r,unsigned target,
                                  uint32_t *words,size_t frames,
                                  unsigned sample_bits,unsigned frames_per_ms)
{
    if(!r || r->current_q14>OMNI_SOURCE_MIX_UNITY || target>OMNI_SOURCE_MIX_UNITY ||
       frames>OMNI_SOURCE_MIX_MAX_FRAMES || (!words && frames) ||
       (sample_bits!=16u && sample_bits!=24u) ||
       (frames_per_ms!=48u && frames_per_ms!=96u)) return false;
    if(!frames) return true;
    unsigned current=r->current_q14;
    unsigned step=frames_per_ms==96u?OMNI_SOURCE_MIX_RAMP_STEP/2u:OMNI_SOURCE_MIX_RAMP_STEP;
    if(current==target && current==OMNI_SOURCE_MIX_UNITY) return true;
    if(current==0u && target==0u) {memset(words,0,frames*2u*sizeof(*words));return true;}
    for(size_t frame=0;frame<frames;frame++) {
        if(current<target) {
            unsigned distance=target-current;
            current+=distance<step?distance:step;
        } else if(current>target) {
            unsigned distance=current-target;
            current-=distance<step?distance:step;
        }
        if(current!=OMNI_SOURCE_MIX_UNITY) {
            words[frame*2u]=sample_bits==24u?scaled24(words[frame*2u],current):scaled(words[frame*2u],current);
            words[frame*2u+1u]=sample_bits==24u?scaled24(words[frame*2u+1u],current):scaled(words[frame*2u+1u],current);
        }
    }
    r->current_q14=(uint16_t)current;return true;
}
