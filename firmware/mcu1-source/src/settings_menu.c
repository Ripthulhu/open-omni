#include "settings_menu.h"
#include "eq_menu.h"
#include "home_ui.h"
#include <string.h>
typedef struct {const char *name;unsigned id,min,max;} item;
static const item headset[]={
 {"LIMITER",1,0,1},{"NOISE MODE",5,0,4},{"ANC LEVEL",6,1,3},
 {"TRANSPARENCY",7,1,10},{"WIRELESS EQ",12,0,3},{"AUTO OFF",11,0,6}};
static const item microphone[]={
 {"MIC LEVEL",2,1,10},{"SIDETONE",3,0,10},{"NOISE FILTER",4,0,3},
 {"MIC EQ",13,0,8},{"MUTE LED",9,0,10}};
static const item bluetooth[]={
 {"BT EQ",14,0,3},{"AUTO START",8,0,1},{"CALL MIX",10,0,2}};
static const item display[]={
 {"BRIGHTNESS",32,1,10},{"TIMEOUT",33,0,6},{"HOME VIEW",34,0,1},{"SCREEN SAVER",35,0,1}};
static const item inputs[]={{"USB2 / USB3",36,0,1}};
static const struct {const item *items;unsigned count;} groups[]={
 {headset,6},{microphone,5},{bluetooth,3},{display,4},{inputs,1}};
static const char *const categories[]={"MIXER","HEADSET","MICROPHONE","BLUETOOTH","DISPLAY","INPUTS"};
static omni_settings_menu_io io;
static bool opened,editing,feedback,eq_express;
static unsigned category,row,value,depth;
static const char *message;
static const item *current(void) {return &groups[category-1u].items[row];}
void omni_settings_menu_bind(omni_settings_menu_io callbacks) {io=callbacks;}
bool omni_settings_menu_enabled(void) {return io.read && io.write;}
bool omni_settings_menu_open(void) {return opened;}
void omni_settings_menu_begin(void) {omni_eq_menu_close();opened=true;depth=0;editing=false;feedback=false;eq_express=false;message=0;}
void omni_settings_menu_close(void) {omni_eq_menu_close();opened=false;editing=false;feedback=false;message=0;}
void omni_settings_menu_jump_eq(unsigned control)
{
    if(!omni_settings_menu_enabled()) return;
    for(unsigned c=1;c<=sizeof(groups)/sizeof(groups[0]);++c)
        for(unsigned r=0;r<groups[c-1u].count;++r)
            if(groups[c-1u].items[r].id==control) {category=c;row=r;}
    opened=true;depth=1;editing=false;feedback=false;eq_express=true;message=0;
    omni_eq_menu_express(control,io);
}
bool omni_settings_menu_event(omni_control_kind_t kind)
{
    if(omni_eq_menu_open()) {omni_eq_menu_event(kind);if(eq_express && !omni_eq_menu_open()){opened=false;eq_express=false;}return false;}
    if(kind==OMNI_CONTROL_BACK) {
        if(editing) editing=false;
        else if(depth) depth=0;
        else opened=false;
        message=0;feedback=false;
    } else if(kind==OMNI_CONTROL_SELECT) {
        if(!depth) {
            if(!category) {opened=false;return true;}
            depth=1;row=0;
        } else if(!editing) {
            const item *i=current();
            if(i->id>=12u && i->id<=14u) {omni_eq_menu_begin(i->id,io);return false;}
            if(!io.read(i->id,&value)) {
                message="NOT LOADED";feedback=false;return false;
            }
            if(value<i->min || value>i->max) value=i->min;
            editing=true;message=0;
        } else {
            if(io.write(current()->id,value)) {editing=false;feedback=true;message=0;}
            else message="UNAVAILABLE";
        }
    }
    return false;
}
void omni_settings_menu_dial(int step)
{
    if(omni_eq_menu_open()) {omni_eq_menu_dial(step);return;}
    if(!step) return;
    message=0;feedback=false;
    if(editing) {
        const item *i=current();int64_t next=(int64_t)value-step;
        value=next<(int64_t)i->min?i->min:next>(int64_t)i->max?i->max:(unsigned)next;
    } else {
        unsigned count=depth?groups[category-1u].count:6u;
        unsigned *selected=depth?&row:&category;
        *selected=(*selected+(step<0?1u:count-1u))%count;
    }
}
static void label(char out[12],const char *s)
{size_t n=strlen(s);if(n>11u)n=11u;memcpy(out,s,n);out[n]=0;}
static void decimal(char out[12],unsigned n)
{
    if(n>999u)n=999u;
    unsigned i=0;
    if(n>=100u)out[i++]=(char)('0'+n/100u);
    if(n>=10u)out[i++]=(char)('0'+n/10u%10u);
    out[i++]=(char)('0'+n%10u);out[i]=0;
}
static void format(unsigned id,unsigned n,char out[12])
{
    static const unsigned minutes[]={0,1,5,10,15,30,60};
    static const char *const mode[]={"OFF","TRANS","HIGH","MED","LOW"};
    if(id==5u) label(out,n<5u?mode[n]:"--");
    else if(id==11u || id==33u) {
        if(n==0u) label(out,"OFF");
        else if(n<7u) {decimal(out,minutes[n]);strcat(out,"MIN");}
        else label(out,"--");
    } else if(id==1u || id==8u || id==35u) label(out,n?"ON":"OFF");
    else if(id==34u) label(out,n?"STEREO":"INPUTS");
    else if(id==12u || id==14u) {
        static const char *const eq[]={"FLAT","BASS","FOCUS","SMILEY"};
        label(out,n<4u?eq[n]:"CUSTOM");
    } else if(id==13u) {
        static const char *const eq[]={"FLAT","BALANCE","BCAST-H","BCAST-L","CLAR-L","CLAR-H","DEEP","NASAL","WALKIE"};
        label(out,n<9u?eq[n]:"CUSTOM");
    }
    else if(id==36u) label(out,n?"USB3":"USB2");
    else if((id==3u || id==4u) && !n) label(out,"OFF");
    else if(id==10u) {
        static const char *const calls[]={"MIX","DUCK","MUTE"};
        label(out,n<3u?calls[n]:"--");
    } else decimal(out,n);
}
void omni_settings_menu_render(uint8_t frame[1024])
{
    if(omni_eq_menu_open()) {omni_eq_menu_render(frame);return;}
    omni_settings_view v={0};
    v.title=depth?categories[category]:"MENU";
    v.count=depth?groups[category-1u].count:6u;
    v.selected=depth?row:category;v.editing=editing;
    unsigned first=v.selected/4u*4u;
    for(unsigned slot=0;slot<4u && first+slot<v.count;++slot) {
        unsigned index=first+slot;
        if(!depth) v.labels[slot]=categories[index];
        else {
            const item *i=&groups[category-1u].items[index];
            unsigned n;v.labels[slot]=i->name;
            bool known=io.read(i->id,&n);
            if(editing && index==row) {n=value;known=true;}
            if(known) format(i->id,n,v.values[slot]);
            else label(v.values[slot],"--");
        }
    }
    v.status=message?message:feedback && io.status?io.status():0;
    omni_settings_ui_render(frame,&v);
}
