#ifndef OMNI_CONTROLS_H
#define OMNI_CONTROLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OMNI_CONTROL_SELECT,
    OMNI_CONTROL_MENU,
    OMNI_CONTROL_BACK,
    /* Stock2109 is a context-dependent three-state action, not yet named. */
    OMNI_CONTROL_BACK_HOLD_UNKNOWN,
    /* Directions retain the stock event distinction; no clockwise/up claim. */
    OMNI_CONTROL_DIAL_0,
    OMNI_CONTROL_DIAL_1,
    /* DB91 protocol coordination, not inferred physical headset gestures. */
    OMNI_CONTROL_REMOTE_MENU_EXIT,
    OMNI_CONTROL_REMOTE_MENU_ENTER,
    /* Captured headset home short click; raw is its reported context, not
     * a toggle value. Repeated identical reports remain separate events. */
    OMNI_CONTROL_HOME_MODE,
    /* Home source-bias dial reports. Keep separate from menu/master turns
     * so a late packet cannot move a different control after a mode change. */
    OMNI_CONTROL_BIAS_0,
    OMNI_CONTROL_BIAS_1
} omni_control_kind_t;

typedef enum {
    OMNI_CONTROL_LOCAL_PUSH,
    OMNI_CONTROL_LOCAL_BACK,
    OMNI_CONTROL_REMOTE_DSP
} omni_control_origin_t;

typedef struct {
    omni_control_kind_t kind;
    omni_control_origin_t origin;
    /* Local GPIO mask1/2, remote91/D2 subcommand, or D209 context0/1. */
    uint8_t raw;
} omni_control_event_t;

typedef enum {
    OMNI_CONTROL_FRAME_MALFORMED,
    OMNI_CONTROL_FRAME_UNSUPPORTED,
    OMNI_CONTROL_FRAME_QUEUED,
    OMNI_CONTROL_FRAME_OVERFLOW
} omni_control_frame_result_t;

#define OMNI_CONTROLS_QUEUE_CAPACITY 8u
#define OMNI_CONTROLS_DEBOUNCE_MS 5u
#define OMNI_CONTROLS_HOLD_MS 1000u

typedef struct {
    uint32_t candidate_ms;
    uint32_t pressed_ms;
    bool candidate;
    bool stable;
    bool armed;
    bool held;
} omni_control_button_t;

typedef struct {
    omni_control_button_t buttons[2];
    omni_control_event_t queue[OMNI_CONTROLS_QUEUE_CAPACITY];
    uint32_t last_sample_ms;
    uint32_t dropped_events;
    uint8_t head;
    uint8_t count;
    bool initialized;
} omni_controls_t;

/* Software only. GPIO bits0/1 correspond to PIO0_0/PIO1_17, active high.
 * No MMIO, mode selection, sensor excitation or pull configuration occurs.
 * An input held at init is ignored until a debounced release. Init requires
 * quiescent exclusive ownership; it clears the event queue and counters. */
bool omni_controls_init(omni_controls_t *controls, uint8_t input_mask, uint32_t now_ms);

/* Sample regularly from one serialized owner. At most two events are added.
 * Timestamps allow unsigned wrap; gaps/backward steps >=2^31ms are rejected.
 * Durations begin at observed debounce completion, not a guessed edge time.
 * If hold and release qualify in the same poll, hold wins and suppresses short.
 * false means invalid argument/time; queue overflow is recorded separately. */
bool omni_controls_sample(omni_controls_t *controls, uint8_t input_mask, uint32_t now_ms);

/* One complete DB0491, DB04D206/07, or captured DB06D20903SS, SS0/1. No replies,
 * peer reset or transport access. Unexpected/malformed frames leave local
 * state and queue unchanged. Retain91 subcommands08/0A as menu protocol
 * events, not long-press gestures. D209 is a home short-click report; the
 * caller must distinguish any outstanding B1 query response before delivery.
 * It does not mean toggle inside a menu. Identical reports are not deduped. */
omni_control_frame_result_t omni_controls_receive(omni_controls_t *controls,
                                                 const uint8_t *frame, size_t length);

/* Same FIFO carries local and remote events. Overflow drops the newest event
 * and saturates dropped_events; it never corrupts or replaces older events.
 * Callers serialize sample/receive/pop; no interrupt synchronization is hidden. */
bool omni_controls_pop(omni_controls_t *controls, omni_control_event_t *event);

#endif
