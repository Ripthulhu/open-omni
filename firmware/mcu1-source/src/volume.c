#include "omni_core.h"

uint8_t omni_volume_attenuation(const omni_volume *v)
{
    /* Provisional STAGED store-and-forward index in [0,56]: 0 = loudest
     * (current at maximum), 56 = silent (current at minimum), linear in the dB
     * domain with round-to-nearest. This is NOT a calibrated dB->step gain law
     * -- only the 0..56 range and the 0=loud inversion are recovered from stock;
     * the exact quantization curve is not. Emits no wire command. */
    if (!v || v->maximum <= v->minimum) return 0u;
    int32_t span = (int32_t)v->maximum - (int32_t)v->minimum;
    int32_t below_top = (int32_t)v->maximum - (int32_t)v->current;
    if (below_top < 0) below_top = 0;
    if (below_top > span) below_top = span;
    return (uint8_t)((below_top * 56 + span / 2) / span); /* [0,56] by construction */
}

bool omni_volume_init(omni_volume *v, int16_t minimum, int16_t maximum, int16_t step, int16_t initial)
{
    if (!v || step <= 0 || minimum > maximum ||
        ((int32_t)maximum-minimum)%step || initial < minimum || initial > maximum ||
        ((int32_t)initial-minimum)%step) return false;
    *v = (omni_volume){.minimum=minimum,.maximum=maximum,.step=step,
                       .current=initial,.muted=true};
    v->attenuation=omni_volume_attenuation(v);
    return true;
}

bool omni_volume_set(omni_volume *v, int16_t db, bool local)
{
    if (db<v->minimum || db>v->maximum || ((int32_t)db-v->minimum)%v->step) return false;
    if (db!=v->current) {
        v->current=db;
        v->attenuation=omni_volume_attenuation(v);
        ++v->revision;
        if (local) v->pending|=1u;
    }
    return true;
}

void omni_mute_set(omni_volume *v, bool mute, bool local)
{
    if (v->muted!=mute) {
        v->muted=mute;
        ++v->revision;
        if (local) v->pending|=2u;
    }
}

void omni_volume_dial(omni_volume *v, int steps)
{
    if (!steps) return;
    int64_t db=(int64_t)v->current+(int64_t)steps*v->step;
    if(db<v->minimum) db=v->minimum;
    if(db>v->maximum) db=v->maximum;
    (void)omni_volume_set(v,(int16_t)db,true);
    /* Zero on the dial means mute; turning up explicitly resumes playback.
     * Keep a separate manual mute when turning down above the minimum. */
    if(db==v->minimum) omni_mute_set(v,true,true);
    else if(steps>0) omni_mute_set(v,false,true);
}

size_t omni_volume_notification(const omni_volume *v, uint8_t out[6])
{
    if (!v->pending) return 0;
    /* ADC-2 6.1: bmInfo=0, bAttribute=CUR, wValue=selector/channel,
       wIndex=entity/interface. These are not HID reports. */
    out[0]=0; out[1]=1; out[2]=0;
    out[3]=(v->pending&1u) ? 2u : 1u;
    out[4]=0; out[5]=5;
    return 6;
}

void omni_volume_notification_complete(omni_volume *v, uint8_t selector, uint32_t sent_revision)
{
    if (sent_revision!=v->revision) return; /* Send latest state after an in-flight change. */
    if (selector==2) v->pending&=(uint8_t)~1u;
    if (selector==1) v->pending&=(uint8_t)~2u;
}
