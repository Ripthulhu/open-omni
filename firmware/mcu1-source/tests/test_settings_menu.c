#include "settings_menu.h"
#include "mixer_ui.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
static unsigned writes,last_id,last_value;
static bool accept=true,known=true;
static bool read_value(unsigned id,unsigned *v) {(void)id;*v=1;return known;}
static bool write_value(unsigned id,unsigned v) {if(!accept)return false;++writes;last_id=id;last_value=v;return true;}
static const char *status(void) {return "SENT";}
int main(void)
{
    omni_settings_menu_bind((omni_settings_menu_io){read_value,write_value,status});
    assert(omni_mixer_ui_event(OMNI_CONTROL_MENU));assert(omni_mixer_ui_open());
    assert(omni_mixer_ui_event(OMNI_CONTROL_SELECT)); /* LINE OUT child. */
    assert(!omni_settings_menu_open() && omni_mixer_ui_open());
    assert(omni_mixer_ui_event(OMNI_CONTROL_BACK));assert(omni_settings_menu_open());
    assert(omni_mixer_ui_dial(-1)); /* Clockwise moves down to Headset. */
    assert(omni_mixer_ui_event(OMNI_CONTROL_SELECT));
    known=false;
    assert(omni_mixer_ui_event(OMNI_CONTROL_SELECT));
    assert(omni_mixer_ui_event(OMNI_CONTROL_SELECT));assert(!writes);
    known=true;
    assert(omni_mixer_ui_event(OMNI_CONTROL_SELECT)); /* Limiter edit. */
    assert(omni_mixer_ui_dial(INT_MIN));assert(!writes);
    assert(omni_mixer_ui_event(OMNI_CONTROL_BACK));assert(!writes); /* Cancel. */
    assert(omni_mixer_ui_event(OMNI_CONTROL_SELECT));
    assert(omni_mixer_ui_dial(INT_MAX));
    accept=false;assert(omni_mixer_ui_event(OMNI_CONTROL_SELECT));assert(!writes);
    accept=true;assert(omni_mixer_ui_event(OMNI_CONTROL_SELECT));
    assert(writes==1 && last_id==1 && last_value==1);
    assert(omni_mixer_ui_event(OMNI_CONTROL_BACK)); /* Root. */
    struct {unsigned char a[16],f[1024],b[16];} g;
    memset(&g,0xa5,sizeof(g));
    for(unsigned group=0;group<6;++group) {
        assert(omni_mixer_ui_dial(1));
        assert(omni_mixer_ui_event(OMNI_CONTROL_SELECT));
        for(unsigned row=0;row<10;++row) {
            assert(omni_mixer_ui_render(g.f));
            assert(omni_mixer_ui_dial(1));
            for(unsigned j=0;j<16;++j)assert(g.a[j]==0xa5 && g.b[j]==0xa5);
        }
        assert(omni_mixer_ui_event(OMNI_CONTROL_BACK));
    }
    assert(omni_mixer_ui_event(OMNI_CONTROL_REMOTE_MENU_EXIT));assert(!omni_mixer_ui_open());
    assert(omni_mixer_ui_event(OMNI_CONTROL_REMOTE_MENU_ENTER));assert(omni_mixer_ui_open());
    assert(omni_mixer_ui_event(OMNI_CONTROL_REMOTE_MENU_ENTER));assert(omni_mixer_ui_open());
    assert(omni_mixer_ui_event(OMNI_CONTROL_MENU));assert(!omni_mixer_ui_open());
    puts("settings navigation, commit/cancel, rejection and bounds pass");return 0;
}
