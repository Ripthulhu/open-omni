#include "eq_menu.h"
#include "home_ui.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
static unsigned fields[320],writes,applies,last;
static bool available=true;
static char labels[4][20],values[4][12];
static unsigned selected;static bool editing;
static bool read_value(unsigned id,unsigned *v) {*v=fields[id];return available;}
static bool write_value(unsigned id,unsigned v)
{
    assert(id<320);++writes;last=id;
    if(id>=128 && (id-128)%64==OMNI_EQ_APPLY) ++applies;
    else fields[id]=v;
    return available;
}
static const char *status(void) {return "SENT";}
void omni_settings_ui_render(uint8_t frame[1024],const omni_settings_view *v)
{
    memset(frame,0,1024);selected=v->selected;editing=v->editing;
    for(unsigned i=0;i<4;++i) {
        strcpy(labels[i],v->labels[i]?v->labels[i]:"");strcpy(values[i],v->values[i]);
    }
}
void omni_eq_ui_render(uint8_t frame[1024],const omni_eq_view *v)
{
    memset(frame,0,1024);selected=v->apply?10u:v->selected;editing=v->editing;
    strcpy(values[0],v->values[0]);
    if(v->apply)strcpy(labels[2],"APPLY CURVE");
    if(v->editing && v->field==0u)assert(v->gain[v->selected]==(unsigned)(fields[128u+(v->parametric?0u:!strcmp(v->title,"MIC EQ")?64u:128u)+v->selected]+5u));
}
static void click(void) {omni_eq_menu_event(OMNI_CONTROL_SELECT);}
static void back(void) {omni_eq_menu_event(OMNI_CONTROL_BACK);}
static void down(void) {omni_eq_menu_dial(-1);}
int main(void)
{
    uint8_t frame[1024];
    for(unsigned id=12;id<=14;++id) {
        unsigned base=128u+(id-12u)*64u;
        for(unsigned b=0;b<10;++b) fields[base+b]=120;
        fields[base+10]=1000;fields[base+20]=1000;fields[base+30]=1;
        omni_eq_menu_begin(id,(omni_settings_menu_io){read_value,write_value,status});
        down();click();assert(last==base+OMNI_EQ_BEGIN);
        click();if(id==12) click(); /* First band gain editor. */
        down();omni_eq_menu_render(frame);assert(editing && !strcmp(values[0],"+0.5dB"));
        unsigned before=writes;back();assert(writes==before && fields[base]==120);
        click();omni_eq_menu_dial(INT_MIN);click();assert(fields[base]==240 && !applies);
        click();omni_eq_menu_dial(INT_MAX);click();assert(fields[base]==0);
        if(id==12) {
            down();click();down();click();assert(fields[base+10]==1100);
            down();click();down();click();assert(fields[base+20]==1050);
            down();click();down();click();assert(fields[base+30]==2);
            back();
        }
        for(unsigned b=0;b<10;++b) down();
        omni_eq_menu_render(frame);assert(selected==10 && !strcmp(labels[2],"APPLY CURVE"));
        click();assert(applies==1);applies=0;
        back();back();assert(!omni_eq_menu_open());
    }
    puts("EQ menu navigation, cancellation, detents, labels and explicit apply pass");return 0;
}
