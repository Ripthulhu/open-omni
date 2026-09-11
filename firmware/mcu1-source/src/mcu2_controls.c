#include "mcu2_controls.h"
#include <string.h>
void omni_mcu2_controls_init(omni_mcu2_controls *s)
{ if(s) memset(s,0,sizeof(*s)); }
static void field(omni_mcu2_controls *s,uint32_t mask,uint8_t *to,uint8_t value)
{
    if(!(s->known&mask) || *to!=value) ++s->generation;
    *to=value;s->known|=mask;
}
static void mic(omni_mcu2_controls *s,uint8_t state,uint32_t now)
{
    if(!(s->known&OMNI_PEER_HAVE_MIC_STATE) || s->mic_state!=state) ++s->mic_generation;
    field(s,OMNI_PEER_HAVE_MIC_STATE,&s->mic_state,state);s->last_mic_ms=now;
}
bool omni_mcu2_controls_native(omni_mcu2_controls *s,uint8_t step,uint8_t balance)
{
    if(!s || step>56U || balance>100U) return false;
    s->loudness_step=step;
    field(s,OMNI_PEER_HAVE_VOLUME,&s->volume_percent,(uint8_t)((unsigned)step*100U/56U));
    field(s,OMNI_PEER_HAVE_BALANCE,&s->balance,balance);return true;
}
bool omni_mcu2_controls_observe(omni_mcu2_controls *s,const uint8_t *p,size_t n,uint32_t now)
{
    if(!s || !p) return false;
    if(n<3U || n>255U || p[0]!=0xdbU || p[1]!=n) { ++s->rejected;return false; }
    if(n==5U && p[2]==0xe4U && p[3]==3U && p[4]>=1U && p[4]<=3U) {
        if(p[4]!=3U) {
            uint32_t kept=s->known&(OMNI_PEER_HAVE_VOLUME|OMNI_PEER_HAVE_BALANCE);
            if(kept!=s->known) ++s->generation;
            s->known=kept;
        }
    } else if(n==6U && p[2]==0xd2U && p[3]==3U && p[4]<=2U) {
        if(p[5]>56U) { ++s->rejected;return false; }
        /* A physical home-screen detent reports three source tags. Only
         * tag0 supplies this snapshot's loudness, never three dial actions. */
        if(p[4]==0U) {
            s->loudness_step=p[5];
            field(s,OMNI_PEER_HAVE_VOLUME,&s->volume_percent,(uint8_t)((unsigned)p[5]*100U/56U));
        }
    } else if(n==6U && p[2]==0xd3U && p[3]==3U && p[4]==1U) {
        if(p[5]>1U) { ++s->rejected;return false; }
        mic(s,p[5],now);
    } else if(n==6U && p[2]==0xd3U && p[3]==3U && p[4]==2U) {
        if(p[5]<1U || p[5]>10U) { ++s->rejected;return false; }
        field(s,OMNI_PEER_HAVE_MIC_LEVEL,&s->mic_level,p[5]);
    } else if(n==5U && p[2]==0xd4U && p[3]==3U) {
        if(p[4]>10U) { ++s->rejected;return false; }
        field(s,OMNI_PEER_HAVE_LEGACY_D4,&s->legacy_d4,p[4]);
    } else if(n==46U && p[2]==0x20U && p[3]==1U) {
        if(p[4]>56U || p[14]>1U || p[15]<1U || p[15]>10U) { ++s->rejected;return false; }
        s->loudness_step=p[4];
        field(s,OMNI_PEER_HAVE_VOLUME,&s->volume_percent,(uint8_t)((unsigned)p[4]*100U/56U));
        mic(s,p[14],now);field(s,OMNI_PEER_HAVE_MIC_LEVEL,&s->mic_level,p[15]);
    } else return false;
    ++s->frames;return true;
}
bool omni_mcu2_controls_snapshot(const omni_mcu2_controls *s,uint8_t out[5])
{
    if(!s || !out || s->known!=OMNI_PEER_HAVE_ALL) return false;
    static const uint8_t legacy_map[]={0,25,50,100};
    out[0]=s->volume_percent;out[1]=s->balance;out[2]=s->mic_state;
    out[3]=(uint8_t)(s->mic_level*10U);out[4]=s->legacy_d4<4U?legacy_map[s->legacy_d4]:0;
    return true;
}
void omni_mcu2_controls_status(const omni_mcu2_controls *s,uint32_t out[15])
{
    memset(out,0,60);out[0]=1;out[1]=s->known;out[2]=s->generation;
    out[3]=s->mic_state;out[4]=s->mic_level;out[5]=s->legacy_d4;
    out[6]=s->volume_percent;out[7]=s->balance;out[8]=s->mic_generation;
    out[9]=s->last_mic_ms;out[10]=s->frames;out[11]=s->rejected;out[12]=s->loudness_step;
}
