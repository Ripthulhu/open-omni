#ifndef OMNI_CONTROL_ACTION_H
#define OMNI_CONTROL_ACTION_H

#include "controls.h"

/* Device-independent navigation intent, one level above omni_controls events.
 * It normalizes the transmitter's local switches and the headset's remote
 * DB0491 frames into the SAME small action set, exactly as the stock display
 * dispatcher does (CONTROLS-ROUTING-2026-09-09.md): the local switch table and
 * the remote 0x27EA6 branch enqueue identical event IDs. This is the "normalize
 * local and remote controls before a single serialized menu/volume state
 * machine" step; it owns no menu state and invents no menu-tree semantics. */

typedef enum {
    OMNI_NAV_NONE = 0,               /* event carries no navigation meaning */
    OMNI_NAV_DIAL,                   /* dial movement (stock 0x2104/0x2105) */
    OMNI_NAV_SELECT,                 /* stock 0x2106: select/confirm/enter */
    OMNI_NAV_MENU,                   /* stock 0x2107: open menu at root / toggle */
    OMNI_NAV_BACK,                   /* stock 0x2108: cancel/ascend/leave root */
    /* Stock 0x2109 (local Back hold): a context-dependent three-state action
     * whose feature identity is unresolved. Surfaced, never assigned a name. */
    OMNI_NAV_BACK_HOLD_UNKNOWN,
    /* Remote DB0491 08/0A menu coordination (stock 0x27F6A). Protocol events,
     * NOT inferred physical long-press gestures. */
    OMNI_NAV_REMOTE_MENU_EXIT,
    OMNI_NAV_REMOTE_MENU_ENTER
} omni_nav_action_t;

typedef enum {
    OMNI_NAV_SOURCE_LOCAL,           /* transmitter switch (PIO0_0 / PIO1_17) */
    OMNI_NAV_SOURCE_REMOTE           /* headset via DSP DB0491 frame */
} omni_nav_source_t;

typedef struct {
    omni_nav_action_t action;
    omni_nav_source_t source;
    /* Valid only for OMNI_NAV_DIAL: the stock 0/1 event distinction
     * (05->0x2104->0, 06->0x2105->1). NOT a clockwise/up or signed claim. */
    uint8_t direction;
} omni_nav_intent_t;

/* Normalize one decoded control event into a navigation intent. Returns false
 * (and sets *out to a zeroed OMNI_NAV_NONE) for a NULL argument or an event kind
 * with no navigation meaning, so a caller can drain the controls FIFO straight
 * into one handler regardless of whether each event came from the local switches
 * or the headset. Pure; no state, no hardware, no menu-tree assumptions. */
bool omni_control_intent(const omni_control_event_t *event, omni_nav_intent_t *out);

typedef enum {
    OMNI_DIAL_TARGET_VOLUME,         /* adjust volume (current out-of-menu behavior) */
    OMNI_DIAL_TARGET_MENU_MOVE       /* move the menu selection */
} omni_dial_target_t;

/* Evidenced routing only: outside a menu the dial is the volume control (the
 * behavior the installed firmware already ships); inside a menu it moves the
 * selection (stock 0x1F0C0 shared dial/navigation movement). Owns no menu state
 * -- the caller supplies menu_active. Applies to a remote OMNI_NAV_DIAL event and
 * equally to a local rotary step; the local-rotary sign<->stock-direction
 * correspondence stays a board-level choice and is deliberately not decided here. */
omni_dial_target_t omni_control_dial_target(bool menu_active);

/* Native headset calibration, confirmed physically 2026-09-10: DB0491/05 is
 * counter-clockwise/decrease, /06 clockwise/increase. This signed adaptation
 * does not change the recovered stock 0/1 intent or the local rotary polarity.
 * Non-remote/non-dial/null events return 0. No D2 absolute-volume policy. */
int omni_control_headset_step(const omni_control_event_t *event);

#endif
