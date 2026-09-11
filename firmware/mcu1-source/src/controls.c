#include "controls.h"

#include <string.h>

static bool enqueue(omni_controls_t *controls, omni_control_event_t event)
{
    if (controls->count == OMNI_CONTROLS_QUEUE_CAPACITY) {
        if (controls->dropped_events != UINT32_MAX) ++controls->dropped_events;
        return false;
    }
    unsigned index = ((unsigned)controls->head + controls->count) % OMNI_CONTROLS_QUEUE_CAPACITY;
    controls->queue[index] = event;
    ++controls->count;
    return true;
}

bool omni_controls_init(omni_controls_t *controls, uint8_t input_mask, uint32_t now_ms)
{
    if (controls == NULL || input_mask > 3u) return false;
    memset(controls, 0, sizeof(*controls));
    controls->last_sample_ms = now_ms;
    for (unsigned i = 0; i < 2u; ++i) {
        omni_control_button_t *button = &controls->buttons[i];
        bool pressed = (input_mask & (1u << i)) != 0u;
        button->candidate = pressed;
        button->stable = pressed;
        button->armed = !pressed;
        button->candidate_ms = now_ms;
        button->pressed_ms = now_ms;
    }
    controls->initialized = true;
    return true;
}

bool omni_controls_sample(omni_controls_t *controls, uint8_t input_mask, uint32_t now_ms)
{
    if (controls == NULL || !controls->initialized || input_mask > 3u ||
        (uint32_t)(now_ms - controls->last_sample_ms) > INT32_MAX) return false;
    controls->last_sample_ms = now_ms;
    for (unsigned i = 0; i < 2u; ++i) {
        omni_control_button_t *button = &controls->buttons[i];
        bool pressed = (input_mask & (1u << i)) != 0u;
        omni_control_event_t event = {
            i == 0u ? OMNI_CONTROL_SELECT : OMNI_CONTROL_BACK,
            i == 0u ? OMNI_CONTROL_LOCAL_PUSH : OMNI_CONTROL_LOCAL_BACK,
            (uint8_t)(1u << i)
        };
        if (pressed != button->candidate) {
            button->candidate = pressed;
            button->candidate_ms = now_ms;
        }
        if (button->armed && button->stable && !button->held &&
            (uint32_t)(now_ms - button->pressed_ms) >= OMNI_CONTROLS_HOLD_MS) {
            event.kind = i == 0u ? OMNI_CONTROL_MENU : OMNI_CONTROL_BACK_HOLD_UNKNOWN;
            (void)enqueue(controls, event);
            button->held = true;
        }
        if (button->candidate != button->stable &&
            (uint32_t)(now_ms - button->candidate_ms) >= OMNI_CONTROLS_DEBOUNCE_MS) {
            button->stable = button->candidate;
            if (button->stable) {
                button->pressed_ms = now_ms;
                button->held = false;
            } else {
                if (button->armed && !button->held) (void)enqueue(controls, event);
                button->armed = true;
                button->held = false;
            }
        }
    }
    return true;
}

omni_control_frame_result_t omni_controls_receive(omni_controls_t *controls,
                                                 const uint8_t *frame, size_t length)
{
    if (controls == NULL || !controls->initialized || frame == NULL)
        return OMNI_CONTROL_FRAME_MALFORMED;
    if (length == 6u) {
        if (frame[0] != 0xdbu || frame[1] != 6u || frame[2] != 0xd2u ||
            frame[3] != 9u || frame[4] != 3u || frame[5] > 1u)
            return OMNI_CONTROL_FRAME_MALFORMED;
        omni_control_event_t home = {OMNI_CONTROL_HOME_MODE, OMNI_CONTROL_REMOTE_DSP, frame[5]};
        return enqueue(controls, home) ? OMNI_CONTROL_FRAME_QUEUED : OMNI_CONTROL_FRAME_OVERFLOW;
    }
    if (length != 4u || frame[0] != 0xdbu || frame[1] != 4u)
        return OMNI_CONTROL_FRAME_MALFORMED;
    omni_control_event_t event = {OMNI_CONTROL_SELECT, OMNI_CONTROL_REMOTE_DSP, frame[3]};
    if (frame[2] == 0xd2u && (frame[3] == 6u || frame[3] == 7u)) {
        /* Original MCU1 dispatch: D207 ->2104, D206 ->2105, matching the
         * direction order of91/05 and91/06 respectively. */
        event.kind = frame[3] == 7u ? OMNI_CONTROL_BIAS_0 : OMNI_CONTROL_BIAS_1;
        return enqueue(controls, event) ? OMNI_CONTROL_FRAME_QUEUED : OMNI_CONTROL_FRAME_OVERFLOW;
    }
    if (frame[2] != 0x91u) return OMNI_CONTROL_FRAME_MALFORMED;
    switch (frame[3]) {
    case 4: event.kind = OMNI_CONTROL_SELECT; break;
    case 5: event.kind = OMNI_CONTROL_DIAL_0; break;
    case 6: event.kind = OMNI_CONTROL_DIAL_1; break;
    case 7: event.kind = OMNI_CONTROL_BACK; break;
    case 8: event.kind = OMNI_CONTROL_REMOTE_MENU_EXIT; break;
    case 10: event.kind = OMNI_CONTROL_REMOTE_MENU_ENTER; break;
    default: return OMNI_CONTROL_FRAME_UNSUPPORTED;
    }
    return enqueue(controls, event) ? OMNI_CONTROL_FRAME_QUEUED : OMNI_CONTROL_FRAME_OVERFLOW;
}

bool omni_controls_pop(omni_controls_t *controls, omni_control_event_t *event)
{
    if (controls == NULL || !controls->initialized || event == NULL || controls->count == 0u)
        return false;
    *event = controls->queue[controls->head];
    controls->head = (uint8_t)((controls->head + 1u) % OMNI_CONTROLS_QUEUE_CAPACITY);
    --controls->count;
    return true;
}
