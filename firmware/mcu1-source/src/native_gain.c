#include "native_gain.h"
#include <string.h>

bool omni_native_gain_init(omni_native_gain *v,omni_native_transport transport)
{
    if(!v || !transport.open || !transport.close) return false;
    memset(v,0,sizeof(*v)); v->transport=transport;
    v->desired_db=-30*256; v->desired_muted=1u; v->initialized=true; return true;
}
static void release(omni_native_gain *v)
{
    if(v->owned) v->transport.close(v->transport.context);
    v->owned=false;
}
bool omni_native_gain_inputs(omni_native_gain *v,uint8_t mask,const uint8_t levels[4])
{
    if(!v || !levels || mask!=5u) return false;
    for(unsigned i=0;i<4;i++) if((mask&(1u<<i)) && levels[i]>100u) return false;
    if(v->input_mask!=mask || memcmp(v->input_levels,levels,4)) {
        v->input_mask=mask;memcpy(v->input_levels,levels,4);++v->desired_revision;
    }
    return true;
}
static bool verified(const omni_native_gain *v)
{
    if(v->desired_wire!=v->verified_wire || v->input_mask!=v->verified_mask) return false;
    for(unsigned i=0;i<4;i++) if((v->input_mask&(1u<<i)) && v->input_levels[i]!=v->verified_levels[i]) return false;
    return true;
}
void omni_native_gain_poll(omni_native_gain *v,uint32_t now,bool online,int16_t db,bool muted)
{
    if(!v || !v->initialized) return;
    uint8_t target;
    if(!omni_native_gain_wire(db,muted,&target)) {
        v->fault=true; v->ready=false; ++v->failures;
        omni_gain_trial_cancel(&v->transfer); release(v); return;
    }
    if(db!=v->desired_db || muted!=(v->desired_muted!=0u)) {
        v->desired_db=db; v->desired_muted=(uint8_t)muted; ++v->desired_revision;
    }
    if(v->input_mask) target=v->input_levels[0];
    v->desired_wire=target;
    if(!online) {
        if(omni_gain_trial_busy(&v->transfer)) { omni_gain_trial_cancel(&v->transfer); ++v->cancellations; }
        release(v); v->online=false; v->ready=false; v->fault=false; v->have_verified=false;
        return;
    }
    if(!v->online) { v->online=true; ++v->epoch; }
    if(v->fault) return;
    if(v->owned) {
        omni_gain_trial_poll(&v->transfer,now);
        if(omni_gain_trial_busy(&v->transfer)) return;
        release(v);
        if(v->transfer.phase!=GAIN_DONE || !v->transfer.attenuated_verified) {
            v->fault=true; v->ready=false; ++v->failures; return;
        }
        v->verified_wire=v->submitted_wire; v->verified_revision=v->submitted_revision;
        v->verified_mask=v->submitted_mask;memcpy(v->verified_levels,v->submitted_levels,4);
        v->have_verified=true; v->last_verified_ms=now; ++v->verified_transactions;
        if(verified(v)) v->ready=true;
    }
    if(v->have_verified && verified(v)) {
        v->verified_revision=v->desired_revision; /* muted remembered gain can change without a SET */
        if((uint32_t)(now-v->last_verified_ms)<1000u) return;
    }
    if(v->yielding) return;
    omni_dsp_volume_io io;
    if(!v->transport.open(v->transport.context,&io)) {
        v->fault=true; v->ready=false; ++v->failures; return;
    }
    v->owned=true;
    memset(&v->transfer,0,sizeof(v->transfer));
    v->submitted_wire=v->desired_wire; v->submitted_revision=v->desired_revision;
    v->submitted_mask=v->input_mask;memcpy(v->submitted_levels,v->input_levels,4);
    bool begun=v->input_mask ? omni_gain_apply_masked_begin(&v->transfer,io,now,v->submitted_mask,v->submitted_levels)
                            : omni_gain_apply_begin(&v->transfer,io,now,v->submitted_wire);
    if(!begun) {
        release(v); v->fault=true; v->ready=false; ++v->failures; return;
    }
    ++v->transactions;
}
void omni_native_gain_status(const omni_native_gain *v,unsigned page,uint32_t out[15])
{
    uint32_t w[15]={1u,page};
    if(!page) {
        w[2]=(uint32_t)v->online|((uint32_t)v->ready<<1)|((uint32_t)v->fault<<2)|((uint32_t)v->owned<<3);
        w[3]=(uint32_t)(int32_t)v->desired_db; w[4]=v->desired_muted;
        w[5]=v->desired_wire; w[6]=v->submitted_wire; w[7]=v->verified_wire;
        w[8]=v->desired_revision; w[9]=v->submitted_revision; w[10]=v->verified_revision;
        w[11]=v->transactions; w[12]=v->verified_transactions; w[13]=v->failures; w[14]=v->epoch;
    } else {
        w[2]=(uint32_t)v->transfer.phase; w[3]=(uint32_t)v->transfer.error; w[4]=v->transfer.step;
        memcpy(w+5,v->transfer.original,4); memcpy(w+6,v->transfer.reduced,4);
        memcpy(w+7,v->transfer.observed,4);
        w[8]=v->transfer.tx_bytes; w[9]=v->transfer.rx_bytes; w[10]=v->transfer.unrelated;
        w[11]=v->transfer.set_may_have_applied; w[12]=v->cancellations;
        w[13]=v->last_verified_ms; w[14]=1u; /* mute policy: finite DSP minimum, not hard mute */
    }
    memcpy(out,w,sizeof(w));
}
