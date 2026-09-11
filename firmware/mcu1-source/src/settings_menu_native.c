#include "settings_menu.h"
#include "eq_menu.h"
#include "dsp_settings.h"
#include "headset_query.h"
#include "ui.h"
#include "mcu2_probe.h"
#include <string.h>
static uint32_t token=0x80000000u,submitted;
static unsigned last_kind;
static uint32_t read_token=0xc0000000u,read_ms;
static bool read_attempted;
static uint8_t drafts[3][128];
static bool have_draft[3];
static bool submit(unsigned id,const uint8_t *p,size_t n)
{
    if(omni_dsp_settings_busy()) return false;
    uint32_t next=(token+1u)|0x80000000u;
    if(!omni_dsp_settings_request(next,id,p,n,omni_ui_milliseconds())) return false;
    token=submitted=next;last_kind=3;return true;
}
static bool draft_begin(unsigned channel,bool flat)
{
    if(channel>=3u) return false;
    if(!flat && have_draft[channel]) return true;
    unsigned control=12u+channel,custom=channel==1u?8u:4u;
    uint8_t p[128];size_t n=0;
    if(!flat) n=omni_dsp_settings_custom(control,p);
    if(!n && !flat) {
        uint8_t raw[60];uint32_t flags,length;
        if(!omni_dsp_settings_value(control,0,raw)) return false;
        memcpy(&length,raw+12,4);memcpy(&flags,raw+16,4);
        if(!(flags&1u) || !length || raw[24]==custom) return false;
        n=omni_dsp_settings_eq_preset(control,raw[24],p);
    }
    if(flat) n=omni_dsp_settings_eq_preset(control,custom,p);
    if(!n) return false;
    p[0]=(uint8_t)custom;memset(p+1,0,67);
    memcpy(p+1,"Custom",6);memcpy(p+7,"Custom",6);
    memcpy(drafts[channel],p,n);have_draft[channel]=true;return true;
}
static bool draft_read(unsigned id,unsigned *value)
{
    unsigned channel=(id-OMNI_EQ_FIELD_BASE)/OMNI_EQ_FIELD_STRIDE;
    unsigned field=(id-OMNI_EQ_FIELD_BASE)%OMNI_EQ_FIELD_STRIDE;
    if(channel>=3u || field>=40u || !have_draft[channel]) return false;
    unsigned band=field%10u,kind=field/10u;
    const uint8_t *p=drafts[channel]+68u+(channel?band:6u*band);
    if(kind==0u) {uint8_t gain=p[channel?0u:3u];*value=(unsigned)((gain<128u?(int)gain:(int)gain-256)+120);}
    else if(channel) return false;
    else if(kind==1u) *value=(unsigned)p[0]|((unsigned)p[1]<<8);
    else if(kind==2u) *value=(unsigned)p[4]|((unsigned)p[5]<<8);
    else *value=p[2];
    return true;
}
static bool draft_write(unsigned id,unsigned value)
{
    unsigned channel=(id-OMNI_EQ_FIELD_BASE)/OMNI_EQ_FIELD_STRIDE;
    unsigned field=(id-OMNI_EQ_FIELD_BASE)%OMNI_EQ_FIELD_STRIDE;
    if(channel>=3u) return false;
    if(field==OMNI_EQ_BEGIN || field==OMNI_EQ_FLAT) return draft_begin(channel,field==OMNI_EQ_FLAT);
    if(!have_draft[channel]) return false;
    if(field==OMNI_EQ_APPLY) return submit(12u+channel,drafts[channel],channel?78u:128u);
    if(field>=40u) return false;
    unsigned kind=field/10u,band=field%10u;
    uint8_t *p=drafts[channel]+68u+(channel?band:6u*band);
    if(kind==0u) {if(value>240u)return false;p[channel?0u:3u]=(uint8_t)((int)value-120);}
    else if(channel) return false;
    else if(kind==1u) {if(value<20u || value>20001u)return false;p[0]=(uint8_t)value;p[1]=(uint8_t)(value>>8);}
    else if(kind==2u) {if(value<200u || value>10000u)return false;p[4]=(uint8_t)value;p[5]=(uint8_t)(value>>8);}
    else {if(value<1u || value>6u)return false;p[2]=(uint8_t)value;}
    return true;
}
static const uint8_t minutes[]={0,1,5,10,15,30,60};
static bool cached(unsigned id,uint8_t value[36],unsigned *length)
{
    uint8_t p[60];uint32_t n,flags;
    if(!omni_dsp_settings_value(id,0,p)) return false;
    memcpy(&n,p+12,4);memcpy(&flags,p+16,4);
    if(!(flags&1u) || !n) return false;
    memcpy(value,p+24,36);*length=n;return true;
}
static bool read_value(unsigned id,unsigned *value)
{
    if(id>=OMNI_EQ_FIELD_BASE) return draft_read(id,value);
    uint32_t words[15];
    if(id>=32u && id<=35u) {
        omni_ui_settings_status(words);
        *value=words[id==32u?2u:id==33u?1u:id==34u?4u:3u];return true;
    }
    if(id==36u) {
        if(!omni_mcu2_runtime_read(0,words) || (words[1]&22u)!=20u) return false;
        unsigned mode=(words[12]>>8)&255u;
        if(mode!=0u && mode!=2u) return false;
        *value=mode==2u;return true;
    }
    uint8_t p[36];unsigned n;
    if(!cached(id,p,&n)) {
        /* Bulk startup already supplies the other menu fields. ANC-off
         * snapshots omit the remembered ANC level; fetch that field once
         * while visible, with a bounded retry interval after failure. */
        uint32_t now=omni_ui_milliseconds();
        if(id==DSP_SETTING_ANC_LEVEL && !omni_headset_query_busy() &&
           !omni_dsp_settings_busy() &&
           (!read_attempted || (uint32_t)(now-read_ms)>=10000u)) {
            read_attempted=true;read_ms=now;
            (void)omni_headset_query_request(++read_token,7u,now);
        }
        return false;
    }
    if(id==3u || id==4u) {if(n!=2u)return false;*value=p[0]?p[1]:0;}
    else if(id>=8u && id<=10u) {if(n!=3u)return false;*value=p[id-8u];}
    else if(id==11u) {
        for(unsigned i=0;i<sizeof(minutes);++i) if(p[0]==minutes[i]) {*value=i;return true;}
        return false;
    } else *value=id==13u && p[0]==9u?8u:id==13u && p[0]==8u?9u:p[0];
    return true;
}
static bool write_value(unsigned id,unsigned value)
{
    if(id>=OMNI_EQ_FIELD_BASE) return draft_write(id,value);
    uint32_t words[15];
    if(id>=32u && id<=35u) {
        omni_ui_settings_status(words);
        words[id==32u?2u:id==33u?1u:id==34u?4u:3u]=value;
        if(!omni_ui_settings_set(words[1],words[2],words[3],words[4])) return false;
        last_kind=1;return true;
    }
    if(id==36u) {
        if(value>1u || !omni_mcu2_select_input((uint8_t)value)) return false;
        last_kind=2;return true;
    }
    if(omni_dsp_settings_busy()) return false;
    uint8_t p[128]={(uint8_t)value};size_t n=1;
    if(value>255u) return false;
    if(id==3u || id==4u) {
        p[0]=value!=0u;p[1]=(uint8_t)(value?value:1u);n=2;
    } else if(id>=8u && id<=10u) {
        unsigned length;
        if(!cached(id,p,&length) || length!=3u) return false;
        p[id-8u]=(uint8_t)value;n=3;
    } else if(id==11u) {
        if(value>=sizeof(minutes))return false;
        p[0]=minutes[value];
    } else if(id>=12u && id<=14u) {
        unsigned preset=id==13u && value==8u?9u:id==13u && value==9u?8u:value;
        if(preset==(id==13u?8u:4u)) n=omni_dsp_settings_custom(id,p);
        else n=omni_dsp_settings_eq_preset(id,preset,p);
    }
    if(!n) return false;
    return submit(id,p,n);
}
static const char *status(void)
{
    uint32_t w[15];
    if(last_kind==1u) return "APPLIED";
    if(last_kind==2u) {
        if(!omni_mcu2_runtime_read(1,w)) return "UNAVAILABLE";
        return w[7]==3u?"APPLIED":w[7]>3u?"INPUT FAILED":"SWITCHING";
    }
    if(last_kind==3u) {
        if(!omni_dsp_settings_status(0,w) || w[2]!=submitted) return "SUPERSEDED";
        if(w[5]&1u) return "SENDING";
        return w[4]==DSP_SETTINGS_ACCEPTED?"SENT":"SETTING FAILED";
    }
    return 0;
}
void omni_settings_menu_bind_native(void)
{omni_settings_menu_bind((omni_settings_menu_io){read_value,write_value,status});}
