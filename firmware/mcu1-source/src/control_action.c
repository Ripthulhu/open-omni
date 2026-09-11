#include "control_action.h"

#include <string.h>

bool omni_control_intent(const omni_control_event_t *event, omni_nav_intent_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));   /* OMNI_NAV_NONE / LOCAL / direction 0 */
    if (event == NULL) return false;

    out->source = (event->origin == OMNI_CONTROL_REMOTE_DSP)
                      ? OMNI_NAV_SOURCE_REMOTE : OMNI_NAV_SOURCE_LOCAL;

    switch (event->kind) {
    case OMNI_CONTROL_SELECT:             out->action = OMNI_NAV_SELECT; break;
    case OMNI_CONTROL_MENU:               out->action = OMNI_NAV_MENU; break;
    case OMNI_CONTROL_BACK:               out->action = OMNI_NAV_BACK; break;
    case OMNI_CONTROL_BACK_HOLD_UNKNOWN:  out->action = OMNI_NAV_BACK_HOLD_UNKNOWN; break;
    case OMNI_CONTROL_DIAL_0:             out->action = OMNI_NAV_DIAL; out->direction = 0u; break;
    case OMNI_CONTROL_DIAL_1:             out->action = OMNI_NAV_DIAL; out->direction = 1u; break;
    case OMNI_CONTROL_REMOTE_MENU_EXIT:   out->action = OMNI_NAV_REMOTE_MENU_EXIT; break;
    case OMNI_CONTROL_REMOTE_MENU_ENTER:  out->action = OMNI_NAV_REMOTE_MENU_ENTER; break;
    default:
        out->source = OMNI_NAV_SOURCE_LOCAL;
        return false;
    }
    return true;
}

omni_dial_target_t omni_control_dial_target(bool menu_active)
{
    return menu_active ? OMNI_DIAL_TARGET_MENU_MOVE : OMNI_DIAL_TARGET_VOLUME;
}

int omni_control_headset_step(const omni_control_event_t *event)
{
    if(event==NULL || event->origin!=OMNI_CONTROL_REMOTE_DSP) return 0;
    if(event->kind==OMNI_CONTROL_DIAL_0) return -1;
    if(event->kind==OMNI_CONTROL_DIAL_1) return 1;
    return 0;
}
