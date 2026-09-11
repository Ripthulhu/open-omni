#include "control_action.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Direct event->intent mapping for every control kind, both origins. */
static void mapping_table(void)
{
    struct {
        omni_control_kind_t kind;
        omni_control_origin_t origin;
        bool ok;
        omni_nav_action_t action;
        omni_nav_source_t source;
        uint8_t direction;
    } cases[] = {
        {OMNI_CONTROL_SELECT, OMNI_CONTROL_LOCAL_PUSH, true, OMNI_NAV_SELECT, OMNI_NAV_SOURCE_LOCAL, 0},
        {OMNI_CONTROL_SELECT, OMNI_CONTROL_REMOTE_DSP, true, OMNI_NAV_SELECT, OMNI_NAV_SOURCE_REMOTE, 0},
        {OMNI_CONTROL_MENU, OMNI_CONTROL_LOCAL_PUSH, true, OMNI_NAV_MENU, OMNI_NAV_SOURCE_LOCAL, 0},
        {OMNI_CONTROL_BACK, OMNI_CONTROL_LOCAL_BACK, true, OMNI_NAV_BACK, OMNI_NAV_SOURCE_LOCAL, 0},
        {OMNI_CONTROL_BACK, OMNI_CONTROL_REMOTE_DSP, true, OMNI_NAV_BACK, OMNI_NAV_SOURCE_REMOTE, 0},
        {OMNI_CONTROL_BACK_HOLD_UNKNOWN, OMNI_CONTROL_LOCAL_BACK, true, OMNI_NAV_BACK_HOLD_UNKNOWN, OMNI_NAV_SOURCE_LOCAL, 0},
        {OMNI_CONTROL_DIAL_0, OMNI_CONTROL_REMOTE_DSP, true, OMNI_NAV_DIAL, OMNI_NAV_SOURCE_REMOTE, 0},
        {OMNI_CONTROL_DIAL_1, OMNI_CONTROL_REMOTE_DSP, true, OMNI_NAV_DIAL, OMNI_NAV_SOURCE_REMOTE, 1},
        {OMNI_CONTROL_REMOTE_MENU_EXIT, OMNI_CONTROL_REMOTE_DSP, true, OMNI_NAV_REMOTE_MENU_EXIT, OMNI_NAV_SOURCE_REMOTE, 0},
        {OMNI_CONTROL_REMOTE_MENU_ENTER, OMNI_CONTROL_REMOTE_DSP, true, OMNI_NAV_REMOTE_MENU_ENTER, OMNI_NAV_SOURCE_REMOTE, 0},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        omni_control_event_t event = {cases[i].kind, cases[i].origin, 0};
        omni_nav_intent_t out;
        memset(&out, 0xa5, sizeof(out));
        assert(omni_control_intent(&event, &out) == cases[i].ok);
        assert(out.action == cases[i].action);
        assert(out.source == cases[i].source);
        assert(out.direction == cases[i].direction);
    }
}

/* Select and Back are the same navigation action whether pressed on the
 * transmitter or sent by the headset; only the source differs. */
static void local_remote_unification(void)
{
    omni_control_event_t local_select = {OMNI_CONTROL_SELECT, OMNI_CONTROL_LOCAL_PUSH, 1};
    omni_control_event_t remote_select = {OMNI_CONTROL_SELECT, OMNI_CONTROL_REMOTE_DSP, 4};
    omni_control_event_t local_back = {OMNI_CONTROL_BACK, OMNI_CONTROL_LOCAL_BACK, 2};
    omni_control_event_t remote_back = {OMNI_CONTROL_BACK, OMNI_CONTROL_REMOTE_DSP, 7};
    omni_nav_intent_t a, b;
    assert(omni_control_intent(&local_select, &a) && omni_control_intent(&remote_select, &b));
    assert(a.action == b.action && a.action == OMNI_NAV_SELECT);
    assert(a.source == OMNI_NAV_SOURCE_LOCAL && b.source == OMNI_NAV_SOURCE_REMOTE);
    assert(omni_control_intent(&local_back, &a) && omni_control_intent(&remote_back, &b));
    assert(a.action == b.action && a.action == OMNI_NAV_BACK);
    assert(a.source == OMNI_NAV_SOURCE_LOCAL && b.source == OMNI_NAV_SOURCE_REMOTE);
}

static void bad_arguments(void)
{
    omni_nav_intent_t out;
    omni_control_event_t event = {OMNI_CONTROL_SELECT, OMNI_CONTROL_LOCAL_PUSH, 0};
    assert(!omni_control_intent(&event, NULL));
    memset(&out, 0xa5, sizeof(out));
    assert(!omni_control_intent(NULL, &out));
    assert(out.action == OMNI_NAV_NONE && out.direction == 0);
    /* An out-of-range kind is not navigation and must not be forced through. */
    omni_control_event_t bogus = {(omni_control_kind_t)99, OMNI_CONTROL_LOCAL_PUSH, 0};
    memset(&out, 0xa5, sizeof(out));
    assert(!omni_control_intent(&bogus, &out));
    assert(out.action == OMNI_NAV_NONE);
}

static void dial_target_gate(void)
{
    assert(omni_control_dial_target(false) == OMNI_DIAL_TARGET_VOLUME);
    assert(omni_control_dial_target(true) == OMNI_DIAL_TARGET_MENU_MOVE);
    omni_control_event_t e={OMNI_CONTROL_DIAL_0,OMNI_CONTROL_REMOTE_DSP,5};
    assert(omni_control_headset_step(&e)==-1);
    e.kind=OMNI_CONTROL_DIAL_1;e.raw=6;
    assert(omni_control_headset_step(&e)==1);
    e.origin=OMNI_CONTROL_LOCAL_PUSH;
    assert(omni_control_headset_step(&e)==0);
    e.origin=OMNI_CONTROL_REMOTE_DSP;e.kind=OMNI_CONTROL_SELECT;
    assert(omni_control_headset_step(&e)==0 && omni_control_headset_step(NULL)==0);
}

/* End to end: drain a mixed local/remote FIFO from the real decoder straight
 * into one handler, confirming a single normalized navigation stream. */
static void integration_mixed_stream(void)
{
    omni_controls_t controls;
    assert(omni_controls_init(&controls, 0, 0));
    const uint8_t select_frame[] = {0xdb, 4, 0x91, 4};
    const uint8_t dial0_frame[] = {0xdb, 4, 0x91, 5};
    const uint8_t back_frame[] = {0xdb, 4, 0x91, 7};
    assert(omni_controls_receive(&controls, select_frame, 4) == OMNI_CONTROL_FRAME_QUEUED);
    assert(omni_controls_receive(&controls, dial0_frame, 4) == OMNI_CONTROL_FRAME_QUEUED);
    /* Local push short release -> SELECT (local). */
    assert(omni_controls_sample(&controls, 1, 0));
    assert(omni_controls_sample(&controls, 1, 5));
    assert(omni_controls_sample(&controls, 0, 10));
    assert(omni_controls_sample(&controls, 0, 15));
    assert(omni_controls_receive(&controls, back_frame, 4) == OMNI_CONTROL_FRAME_QUEUED);
    /* Local Back short release -> BACK (local). */
    assert(omni_controls_sample(&controls, 2, 20));
    assert(omni_controls_sample(&controls, 2, 25));
    assert(omni_controls_sample(&controls, 0, 30));
    assert(omni_controls_sample(&controls, 0, 35));

    struct {
        omni_nav_action_t action;
        omni_nav_source_t source;
        uint8_t direction;
    } expect[] = {
        {OMNI_NAV_SELECT, OMNI_NAV_SOURCE_REMOTE, 0},
        {OMNI_NAV_DIAL, OMNI_NAV_SOURCE_REMOTE, 0},
        {OMNI_NAV_SELECT, OMNI_NAV_SOURCE_LOCAL, 0},
        {OMNI_NAV_BACK, OMNI_NAV_SOURCE_REMOTE, 0},
        {OMNI_NAV_BACK, OMNI_NAV_SOURCE_LOCAL, 0},
    };
    omni_control_event_t event;
    unsigned i = 0;
    while (omni_controls_pop(&controls, &event)) {
        omni_nav_intent_t intent;
        assert(omni_control_intent(&event, &intent));
        assert(i < sizeof(expect) / sizeof(expect[0]));
        assert(intent.action == expect[i].action);
        assert(intent.source == expect[i].source);
        assert(intent.direction == expect[i].direction);
        ++i;
    }
    assert(i == sizeof(expect) / sizeof(expect[0]));
}

int main(void)
{
    mapping_table();
    local_remote_unification();
    bad_arguments();
    dial_target_gate();
    integration_mixed_stream();
    puts("control_action: normalized local/remote navigation intent and dial routing PASS");
    return 0;
}
