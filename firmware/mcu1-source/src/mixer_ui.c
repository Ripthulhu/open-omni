#include "mixer_ui.h"
#include "settings_menu.h"
#include "mixer.h"
#include "home_ui.h"
#include "source_mix.h"
#include "native_gain.h"
#include "volume_scale.h"
#include <string.h>
static omni_mixer mixer;
static omni_source_mix bias;
static bool initialized,editing;
static unsigned page,selected,field;
static uint32_t revision;
static void init(void) { if(!initialized) {omni_mixer_init(&mixer);omni_source_mix_init(&bias);initialized=true;} }
bool omni_mixer_ui_configure(unsigned input,unsigned level,bool linked,bool muted)
{
    init();if(!omni_mixer_set(&mixer,input,level,linked,muted)) return false;
    ++revision;return true;
}
uint32_t omni_mixer_ui_revision(void) { return revision; }
bool omni_mixer_ui_open(void) { return page!=0u || omni_settings_menu_open(); }
bool omni_mixer_ui_control(const omni_control_event_t *event)
{
    if(!event) return false;
    init();
    if(!omni_mixer_ui_open() && (event->kind==OMNI_CONTROL_SELECT || event->kind==OMNI_CONTROL_HOME_MODE)) {
        omni_source_mix_toggle(&bias);++revision;return true;
    }
    if(event->kind==OMNI_CONTROL_HOME_MODE) return false;
    return omni_mixer_ui_event(event->kind);
}
bool omni_mixer_ui_event(omni_control_kind_t kind)
{
    init();
    if(omni_settings_menu_enabled()) {
        bool open=omni_mixer_ui_open();
        if(kind==OMNI_CONTROL_REMOTE_MENU_EXIT || (kind==OMNI_CONTROL_MENU && open)) {
            page=0;editing=false;omni_settings_menu_close();++revision;return true;
        }
        if(kind==OMNI_CONTROL_REMOTE_MENU_ENTER || kind==OMNI_CONTROL_MENU) {
            if(!open) omni_settings_menu_begin();
            ++revision;return true;
        }
        if(omni_settings_menu_open()) {
            if(kind!=OMNI_CONTROL_SELECT && kind!=OMNI_CONTROL_BACK) return false;
            if(omni_settings_menu_event(kind)) {page=1;editing=false;}
            ++revision;return true;
        }
        if(page==1u && kind==OMNI_CONTROL_BACK) {
            page=0;omni_settings_menu_begin();++revision;return true;
        }
    }

    if(kind==OMNI_CONTROL_REMOTE_MENU_ENTER) {if(!page)page=1u;editing=false;}
    else if(kind==OMNI_CONTROL_REMOTE_MENU_EXIT) {page=0u;editing=false;}
    else if(kind==OMNI_CONTROL_MENU) {page=page?0u:1u;editing=false;}
    else if(kind==OMNI_CONTROL_BACK) {
        if(editing) editing=false; else if(page) --page; else return false;
    } else if(kind==OMNI_CONTROL_SELECT) {
        if(!page) page=1;
        else if(page==1) {page=2;field=0;editing=true;}
        else if(omni_mixer_available(selected)) {
            omni_mix_input s=mixer.input[selected];
            if(field==0) editing=!editing;
            else if(field==1) s.linked=!s.linked;
            else s.muted=!s.muted;
            (void)omni_mixer_set(&mixer,selected,s.level,s.linked,s.muted);
        }
    } else return false;
    ++revision;return true;
}
bool omni_mixer_ui_dial(int step)
{
    init();
    if(omni_settings_menu_open()) {omni_settings_menu_dial(step);++revision;return true;}
    if(!page) {
        uint32_t before=bias.revision;
        bool consumed=omni_source_mix_dial(&bias,step);
        if(before!=bias.revision) ++revision;
        return consumed;
    }
    if(!step) return true;
    if(page==1) {
        /* Only show/select routes whose independent faders actually work. */
        unsigned candidate=selected;
        bool found=false;
        for(unsigned i=0;i<OMNI_MIX_COUNT;++i) {
            candidate=(candidate+(step<0?1u:OMNI_MIX_COUNT-1u))%OMNI_MIX_COUNT;
            if(omni_mixer_available(candidate)) {found=true;break;}
        }
        if(!found) return true;
        selected=candidate;
    }
    else if(editing) {
        omni_mix_input s=mixer.input[selected];
        int64_t level=(int64_t)s.level+step;
        if(level<0) level=0;
        if(level>100) level=100;
        (void)omni_mixer_set(&mixer,selected,(unsigned)level,s.linked,s.muted);
    } else field=(unsigned)((int)field+(step<0?1:2))%3u;
    ++revision;return true;
}
void omni_mixer_ui_targets(int16_t db,bool muted,uint8_t levels[4])
{
    init();memset(levels,0,4);
    unsigned master=omni_volume_percent(db);
    unsigned usb=omni_mixer_effective(&mixer,OMNI_MIX_USB1,master,muted);
    int16_t target=omni_volume_percent_db(usb);
    /* Preserve the exact existing USB master mapping at unity input fader. */
    if(mixer.input[0].linked && mixer.input[0].level==100) target=db;
    (void)omni_native_gain_wire(target,usb==0,&levels[0]);
    levels[2]=(uint8_t)omni_mixer_effective(&mixer,OMNI_MIX_LINE,master,muted);
}
void omni_mixer_ui_status(int16_t db,bool muted,uint32_t out[15])
{
    init();memset(out,0,60);out[0]=1;out[1]=mixer.revision;out[2]=page;
    out[3]=selected;out[4]=field;out[5]=editing;out[6]=omni_volume_percent(db);out[7]=muted;
    for(unsigned i=0;i<4;i++) {
        omni_mix_input s=mixer.input[i];
        out[8+i]=s.level|((uint32_t)s.linked<<8)|((uint32_t)s.muted<<9)|
            ((uint32_t)omni_mixer_available(i)<<10)|
            (omni_mixer_effective(&mixer,i,out[6],muted)<<16);
    }
}
bool omni_mixer_ui_render(uint8_t frame[1024])
{
    init();
    if(omni_settings_menu_open()) {omni_settings_menu_render(frame);return true;}
    if(!page) return false;
    omni_menu_view v={.selected=(uint8_t)selected,.detail=page==2u,
        .field=(uint8_t)field,.editing=editing};
    for(unsigned i=0;i<4u;++i) {
        omni_mix_input s=mixer.input[i];
        v.input[i]=(omni_menu_input){s.level,omni_mixer_available(i),s.linked,s.muted};
    }
    omni_menu_ui_render(frame,&v);return true;
}

bool omni_mixer_ui_bias_set(bool enabled,unsigned position)
{
    init();if(position>OMNI_SOURCE_MIX_MAX_POSITION) return false;
    uint32_t before=bias.revision;
    if(!omni_source_mix_configure(&bias,position)) return false;
    if(bias.bias_mode!=enabled) omni_source_mix_toggle(&bias);
    if(bias.revision!=before) ++revision;
    return true;
}
void omni_mixer_ui_bias_snapshot(omni_source_mix *state,omni_source_mix_gains *gains)
{
    init();if(state) *state=bias;
    if(gains) (void)omni_source_mix_snapshot(&bias,gains);
}
