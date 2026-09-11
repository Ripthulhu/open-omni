#include "mixer_ui.h"
#include "control_action.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
static void local_gesture_policy(uint32_t start)
{
    omni_controls_t buttons;
    omni_control_event_t event;
    uint32_t status[15];
    assert(omni_mixer_ui_event(OMNI_CONTROL_REMOTE_MENU_EXIT));
    assert(omni_mixer_ui_bias_set(false,12));
    assert(omni_controls_init(&buttons,0,start));
    uint32_t initial_revision=omni_mixer_ui_revision();
    unsigned events=0;
    /* A short local press switches the home dial from master to source bias. */
    for(uint32_t t=0;t<200u;++t) {
        assert(omni_controls_sample(&buttons,t<100u?1u:0u,start+t));
        while(omni_controls_pop(&buttons,&event)) {
            assert(event.kind==OMNI_CONTROL_SELECT);
            assert(omni_mixer_ui_control(&event));++events;
        }
        assert(!omni_mixer_ui_open());
    }
    omni_source_mix bias;omni_mixer_ui_bias_snapshot(&bias,NULL);
    assert(events==1u && omni_mixer_ui_revision()==initial_revision+1u && bias.bias_mode);
    /* Sustained hold enters exactly once at the existing one-second threshold.
     * Continued holding and release must not toggle closed or select a row. */
    for(uint32_t t=200u;t<3400u;++t) {
        assert(omni_controls_sample(&buttons,t<3300u?1u:0u,start+t));
        while(omni_controls_pop(&buttons,&event)) {
            assert(t==1205u && event.kind==OMNI_CONTROL_MENU);
            assert(omni_mixer_ui_control(&event));++events;
        }
        omni_mixer_ui_status(0,false,status);
        assert(status[2]==(t<1205u?0u:1u));
    }
    assert(events==2u && omni_mixer_ui_revision()==initial_revision+2u);
    /* Once open, the same short click selects the highlighted mixer row. */
    for(uint32_t t=3400u;t<3550u;++t) {
        assert(omni_controls_sample(&buttons,t<3500u?1u:0u,start+t));
        while(omni_controls_pop(&buttons,&event)) {
            assert(event.kind==OMNI_CONTROL_SELECT);
            assert(omni_mixer_ui_control(&event));++events;
        }
    }
    omni_mixer_ui_status(0,false,status);
    assert(events==3u && status[2]==2u);
    /* Booting while pressed must not be mistaken for a new menu gesture. */
    assert(omni_mixer_ui_event(OMNI_CONTROL_REMOTE_MENU_EXIT));
    assert(omni_controls_init(&buttons,1,start+4000u));
    for(uint32_t t=4000u;t<5500u;++t) {
        assert(omni_controls_sample(&buttons,t<5400u?1u:0u,start+t));
        assert(!omni_controls_pop(&buttons,&event));
        assert(!omni_mixer_ui_open());
    }
    /* Exact peer enter/exit uses the same routing and remains idempotent. */
    const uint8_t enter[]={0xdb,4,0x91,10},exit[]={0xdb,4,0x91,8};
    for(unsigned i=0;i<2u;++i) {
        assert(omni_controls_receive(&buttons,enter,sizeof(enter))==OMNI_CONTROL_FRAME_QUEUED);
        assert(omni_controls_pop(&buttons,&event) && omni_mixer_ui_control(&event));
        assert(omni_mixer_ui_open());
    }
    assert(omni_controls_receive(&buttons,exit,sizeof(exit))==OMNI_CONTROL_FRAME_QUEUED);
    assert(omni_controls_pop(&buttons,&event) && omni_mixer_ui_control(&event));
    assert(!omni_mixer_ui_open() && !omni_mixer_ui_control(NULL));
    assert(omni_mixer_ui_bias_set(false,12));
}
static int headset_step(omni_controls_t *buttons,uint8_t subcommand)
{
    const uint8_t frame[]={0xdb,4,0x91,subcommand};omni_control_event_t event;
    assert(omni_controls_receive(buttons,frame,sizeof(frame))==OMNI_CONTROL_FRAME_QUEUED);
    assert(omni_controls_pop(buttons,&event));
    return omni_control_headset_step(&event);
}
static void remote_dial_policy(void)
{
    omni_controls_t buttons;uint32_t s[15];
    assert(omni_controls_init(&buttons,0,0));
    assert(omni_mixer_ui_bias_set(false,12));
    assert(headset_step(&buttons,5)==-1 && headset_step(&buttons,6)==1);
    assert(!omni_mixer_ui_dial(headset_step(&buttons,5))); /* Same -1 routes to home volume. */
    assert(!omni_mixer_ui_dial(headset_step(&buttons,6))); /* Same +1 routes to home volume. */
    assert(omni_mixer_ui_event(OMNI_CONTROL_REMOTE_MENU_ENTER));
    assert(omni_mixer_ui_dial(headset_step(&buttons,6)));
    omni_mixer_ui_status(0,false,s);assert(s[3]==3u); /* Skip unavailable USB2/USB3. */
    assert(omni_mixer_ui_dial(headset_step(&buttons,5)));
    omni_mixer_ui_status(0,false,s);assert(s[3]==0u);
    assert(omni_mixer_ui_configure(0,50,true,false));
    assert(omni_mixer_ui_event(OMNI_CONTROL_SELECT)); /* USB1 level immediately adjustable. */
    omni_mixer_ui_status(0,false,s);assert(s[4]==0u && s[5]==1u);
    assert(omni_mixer_ui_dial(headset_step(&buttons,5)));
    omni_mixer_ui_status(0,false,s);assert((s[8]&255u)==49u);
    assert(omni_mixer_ui_dial(headset_step(&buttons,6)));
    omni_mixer_ui_status(0,false,s);assert((s[8]&255u)==50u);
    assert(omni_mixer_ui_dial(INT_MAX));
    omni_mixer_ui_status(0,false,s);assert((s[8]&255u)==100u);
    assert(omni_mixer_ui_dial(INT_MIN));
    omni_mixer_ui_status(0,false,s);assert((s[8]&255u)==0u);
    assert(omni_mixer_ui_event(OMNI_CONTROL_BACK)); /* Finish edit first. */
    omni_mixer_ui_status(0,false,s);assert(s[2]==2u && s[5]==0u);
    assert(omni_mixer_ui_event(OMNI_CONTROL_BACK)); /* Then return to sources. */
    omni_mixer_ui_status(0,false,s);assert(s[2]==1u);
    for(unsigned i=0;i<12u;++i) {
        assert(omni_mixer_ui_dial(i&1u?-1:1));
        omni_mixer_ui_status(0,false,s);assert(s[3]==0u || s[3]==3u);
    }
    assert(omni_mixer_ui_configure(0,100,true,false));
    assert(omni_mixer_ui_event(OMNI_CONTROL_REMOTE_MENU_EXIT));
}
static void bias_policy(void)
{
    omni_source_mix state;
    omni_source_mix_gains gains;
    assert(omni_mixer_ui_event(OMNI_CONTROL_REMOTE_MENU_EXIT));
    assert(omni_mixer_ui_bias_set(false,12));
    const omni_control_event_t home={OMNI_CONTROL_HOME_MODE,OMNI_CONTROL_REMOTE_DSP,2};
    assert(omni_mixer_ui_control(&home));
    omni_mixer_ui_bias_snapshot(&state,&gains);
    assert(state.bias_mode && state.position==12 && gains.index[0]==12 && gains.index[1]==12);
    assert(omni_mixer_ui_dial(INT_MAX));
    omni_mixer_ui_bias_snapshot(&state,&gains);
    assert(state.position==24 && gains.index[0]==0 && gains.index[1]==12);
    uint32_t revision=omni_mixer_ui_revision();
    assert(omni_mixer_ui_dial(1)); /* Clamped bias consumes: never master fallback. */
    assert(omni_mixer_ui_revision()==revision);
    assert(omni_mixer_ui_dial(INT_MIN));
    omni_mixer_ui_bias_snapshot(&state,&gains);
    assert(state.position==0 && gains.index[0]==12 && gains.index[1]==0);
    assert(omni_mixer_ui_dial(-1));
    assert(omni_mixer_ui_control(&home));
    omni_mixer_ui_bias_snapshot(&state,NULL);assert(!state.bias_mode);
    assert(!omni_mixer_ui_dial(1));
    assert(omni_mixer_ui_event(OMNI_CONTROL_REMOTE_MENU_ENTER));
    revision=omni_mixer_ui_revision();
    assert(!omni_mixer_ui_control(&home)); /* Headset home mode cannot switch while in menu. */
    assert(omni_mixer_ui_open() && omni_mixer_ui_revision()==revision);
    omni_mixer_ui_bias_snapshot(&state,NULL);assert(!state.bias_mode);
    assert(omni_mixer_ui_event(OMNI_CONTROL_REMOTE_MENU_EXIT));
    assert(omni_mixer_ui_bias_set(false,12));
}
int main(int argc,char **argv)
{
    local_gesture_policy(0);
    local_gesture_policy(UINT32_MAX-50u);
    remote_dial_policy();
    bias_policy();
    uint32_t status[15];uint8_t levels[4],frame[1024];
    assert(!omni_mixer_ui_dial(1));
    assert(omni_mixer_ui_event(OMNI_CONTROL_REMOTE_MENU_ENTER));
    assert(omni_mixer_ui_open());
    assert(omni_mixer_ui_event(OMNI_CONTROL_REMOTE_MENU_ENTER));
    assert(omni_mixer_ui_open()); /* peer retry never toggles the menu closed */
    assert(omni_mixer_ui_event(OMNI_CONTROL_REMOTE_MENU_EXIT));
    assert(!omni_mixer_ui_open());
    assert(omni_mixer_ui_event(OMNI_CONTROL_SELECT));
    assert(omni_mixer_ui_dial(1));
    assert(omni_mixer_ui_event(OMNI_CONTROL_SELECT)); /* Line In */
    assert(omni_mixer_ui_event(OMNI_CONTROL_SELECT)); /* Done adjusting level. */
    assert(omni_mixer_ui_dial(-1)); /* Clockwise: SYNC */
    assert(omni_mixer_ui_event(OMNI_CONTROL_SELECT));
    omni_mixer_ui_status(-10*256,false,status);
    assert(status[3]==3 && (status[11]&256u));
    omni_mixer_ui_targets(-10*256,false,levels);assert(levels[2]==50);
    assert(omni_mixer_ui_dial(-1)); /* Clockwise: MUTE */
    assert(omni_mixer_ui_event(OMNI_CONTROL_SELECT));
    omni_mixer_ui_targets(0,false,levels);assert(levels[2]==0);
    assert(omni_mixer_ui_event(OMNI_CONTROL_SELECT)); /* unmute */
    assert(omni_mixer_ui_event(OMNI_CONTROL_BACK)); /* list */
    assert(omni_mixer_ui_render(frame));
    if(argc==2) {
        FILE *f=fopen(argv[1],"wb");assert(f);fprintf(f,"P1\n128 64\n");
        for(unsigned y=0;y<64;y++) for(unsigned x=0;x<128;x++)
            fprintf(f,"%u%c",((unsigned)frame[(y/8)*128+x]>>(y%8))&1u,x==127?'\n':' ');
        fclose(f);
    }
    assert(omni_mixer_ui_event(OMNI_CONTROL_BACK));
    assert(!omni_mixer_ui_render(frame));
    assert(!omni_mixer_ui_dial(1));
    return 0;
}
