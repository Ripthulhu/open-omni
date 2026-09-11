#include "omni_core.h"

static void le16(uint8_t *p, uint16_t x) { p[0]=(uint8_t)x; p[1]=(uint8_t)(x>>8); }
static void le32(uint8_t *p, uint32_t x) { le16(p,(uint16_t)x); le16(p+2,(uint16_t)(x>>16)); }

int omni_uac2_control_format(omni_volume *v, omni_uac2_clocks *clocks,
                            omni_setup s, const uint8_t *payload,
                            size_t payload_length, uint8_t *response, size_t capacity)
{
    uint8_t entity=(uint8_t)(s.index>>8), selector=(uint8_t)(s.value>>8);
    uint8_t iface=(uint8_t)s.index;
    bool playback=iface==0u;
    bool microphone=clocks && iface==2u && entity==11u;
    bool clock_entity=(playback && entity==10u) || microphone;
    uint8_t temp[26]={0};
    size_t n=0;
    if (!v || (!playback && !microphone) || (s.value&255u)) return -1;
    if(clocks && clocks->playback_rate!=48000u && clocks->playback_rate!=96000u) return -1;
    if(s.type==0x21 && playback && entity==10u && clocks) {
        if(s.request!=1u || selector!=1u || s.length!=4u ||
           payload_length!=4u || !payload) return -1;
        uint32_t rate=(uint32_t)payload[0]|((uint32_t)payload[1]<<8)|
                      ((uint32_t)payload[2]<<16)|((uint32_t)payload[3]<<24);
        if(rate!=48000u && rate!=96000u) return -1;
        clocks->playback_rate=rate;return 0;
    }
    if (s.type==0x21 && playback && entity==5 && s.request==1) {
        if (!payload || payload_length!=s.length) return -1;
        if (selector==2 && s.length==2) {
            uint16_t raw=(uint16_t)((uint16_t)payload[0]|((uint16_t)payload[1]<<8));
            int32_t db=raw>=0x8000u ? (int32_t)raw-65536 : (int32_t)raw;
            return omni_volume_set(v,(int16_t)db,false) ? 0 : -1;
        }
        if (selector==1 && s.length==1 && payload[0]<=1) {
            omni_mute_set(v,payload[0]!=0,false); return 0;
        }
        return -1;
    }
    if (s.type!=0xa1 || payload_length) return -1;
    if (playback && entity==5 && selector==2 && s.request==1) {
        n=2; le16(temp,(uint16_t)v->current);
    } else if (playback && entity==5 && selector==2 && s.request==2) {
        n=8; le16(temp,1); le16(temp+2,(uint16_t)v->minimum);
        le16(temp+4,(uint16_t)v->maximum); le16(temp+6,(uint16_t)v->step);
    } else if (playback && entity==5 && selector==1 && s.request==1) {
        n=1; temp[0]=(uint8_t)v->muted;
    } else if (clock_entity && selector==1 && s.request==1) {
        n=4; le32(temp,playback && clocks?clocks->playback_rate:48000u);
    } else if (clock_entity && selector==1 && s.request==2) {
        n=14; le16(temp,1); le32(temp+2,48000); le32(temp+6,48000);
        if(playback && clocks) {
            n=26;le16(temp,2);le32(temp+14,96000);le32(temp+18,96000);
        }
    } else if (clock_entity && selector==2 && s.request==1) {
        n=1; temp[0]=(uint8_t)(!playback || !clocks || clocks->playback_valid);
    } else return -1;
    if (n>s.length) n=s.length;
    if (n>capacity || (n && !response)) return -1;
    for (size_t i=0;i<n;++i) response[i]=temp[i];
    return (int)n;
}

int omni_uac2_control(omni_volume *v, omni_setup s, const uint8_t *payload,
                     size_t payload_length, uint8_t *response, size_t capacity)
{
    /* Retained fixed 48k pure API for legacy tests/tools. The live adapter uses
     * explicit clock state and the format-aware entry above. */
    return omni_uac2_control_format(v,NULL,s,payload,payload_length,response,capacity);
}
