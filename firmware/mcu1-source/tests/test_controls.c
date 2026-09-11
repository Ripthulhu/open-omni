#include "controls.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void local_golden(uint8_t mask, uint32_t released_ms, bool expected_event,
                         omni_control_kind_t expected_kind, uint32_t expected_ms,
                         uint32_t start)
{
    omni_controls_t controls;
    assert(omni_controls_init(&controls, 0, start));
    unsigned count = 0;
    for (uint32_t t = 0; t <= released_ms + 10u; ++t) {
        assert(omni_controls_sample(&controls, t < released_ms ? mask : 0u, start + t));
        omni_control_event_t event;
        while (omni_controls_pop(&controls, &event)) {
            ++count;
            assert(expected_event && event.kind == expected_kind && t == expected_ms);
            assert(event.origin == (mask == 1u ? OMNI_CONTROL_LOCAL_PUSH : OMNI_CONTROL_LOCAL_BACK));
            assert(event.raw == mask);
        }
    }
    assert(count == (expected_event ? 1u : 0u));
}

static void stock_local_cases(void)
{
    /* Golden event times independently recovered by executing stock10CC4,
     * GPIO IRQ10BB0, timers10AE0/10AF8 and table317DC. See19-case JSON oracle.
     * Its two mode6 raw diagnostic cases are deliberately outside this API. */
    for (unsigned wrap = 0; wrap < 2u; ++wrap) {
        uint32_t start = wrap != 0u ? UINT32_MAX - 70u : 0u;
        local_golden(1, 100, true, OMNI_CONTROL_SELECT, 105, start);
        local_golden(1, 1300, true, OMNI_CONTROL_MENU, 1005, start);
        local_golden(2, 100, true, OMNI_CONTROL_BACK, 105, start);
        local_golden(2, 1300, true, OMNI_CONTROL_BACK_HOLD_UNKNOWN, 1005, start);
        local_golden(1, 2, false, OMNI_CONTROL_SELECT, 0, start);
        local_golden(1, 999, true, OMNI_CONTROL_SELECT, 1004, start);
        local_golden(1, 1000, true, OMNI_CONTROL_MENU, 1005, start);
    }
}

static void remote_golden_and_validation(void)
{
    omni_controls_t controls;
    assert(omni_controls_init(&controls, 0, 0));
    /* Actual main dispatcher26B38 event10D, branch27EA6 produced these actions.
     * 08/0A remain explicit protocol events, not guessed physical long holds. */
    const uint8_t commands[] = {4, 5, 6, 7, 8, 10};
    const omni_control_kind_t kinds[] = {
        OMNI_CONTROL_SELECT, OMNI_CONTROL_DIAL_0, OMNI_CONTROL_DIAL_1,
        OMNI_CONTROL_BACK, OMNI_CONTROL_REMOTE_MENU_EXIT, OMNI_CONTROL_REMOTE_MENU_ENTER
    };
    uint8_t frame[] = {0xdb, 4, 0x91, 4, 0};
    omni_control_event_t event;
    for (unsigned i = 0; i < sizeof(commands); ++i) {
        frame[3] = commands[i];
        assert(omni_controls_receive(&controls, frame, 4) == OMNI_CONTROL_FRAME_QUEUED);
        assert(omni_controls_pop(&controls, &event));
        assert(event.kind == kinds[i] && event.origin == OMNI_CONTROL_REMOTE_DSP && event.raw == commands[i]);
        assert(!omni_controls_pop(&controls, &event));
    }
    /* The oracle's four rejected subcommands are0/3/9/11; exhaustively cover
     * every other unsupported subcommand and every corrupt identifying byte. */
    for (unsigned sub = 0; sub < 256u; ++sub) {
        if (sub == 4u || sub == 5u || sub == 6u || sub == 7u || sub == 8u || sub == 10u) continue;
        frame[3] = (uint8_t)sub;
        assert(omni_controls_receive(&controls, frame, 4) == OMNI_CONTROL_FRAME_UNSUPPORTED);
        assert(controls.count == 0u);
    }
    frame[3] = 4;
    const uint8_t header[] = {0xdb, 4, 0x91};
    for (unsigned index = 0; index < 3u; ++index) {
        for (unsigned value = 0; value < 256u; ++value) {
            if (value == header[index]) continue;
            frame[index] = (uint8_t)value;
            assert(omni_controls_receive(&controls, frame, 4) == OMNI_CONTROL_FRAME_MALFORMED);
        }
        frame[index] = header[index];
    }
    for (size_t length = 0; length < 4u; ++length)
        assert(omni_controls_receive(&controls, frame, length) == OMNI_CONTROL_FRAME_MALFORMED);
    assert(omni_controls_receive(&controls, frame, 5) == OMNI_CONTROL_FRAME_MALFORMED);
    assert(omni_controls_receive(&controls, NULL, 4) == OMNI_CONTROL_FRAME_MALFORMED);
    assert(omni_controls_receive(&controls, frame, SIZE_MAX) == OMNI_CONTROL_FRAME_MALFORMED);
    assert(controls.count == 0u && controls.dropped_events == 0u);
}

static void remote_home_mode(void)
{
    omni_controls_t controls;
    omni_control_event_t event;
    uint8_t frame[] = {0xdb, 6, 0xd2, 9, 3, 0, 0};
    assert(OMNI_CONTROL_REMOTE_MENU_ENTER == 7 && OMNI_CONTROL_HOME_MODE == 8);
    assert(omni_controls_init(&controls, 0, 0));
    for (unsigned state = 0; state < 2u; ++state) {
        frame[5] = (uint8_t)state;
        /* The report carries the old context, not a transition identifier:
         * several actual short clicks can report the same value. */
        for (unsigned i = 0; i < OMNI_CONTROLS_QUEUE_CAPACITY; ++i)
            assert(omni_controls_receive(&controls, frame, 6) == OMNI_CONTROL_FRAME_QUEUED);
        assert(omni_controls_receive(&controls, frame, 6) == OMNI_CONTROL_FRAME_OVERFLOW);
        for (unsigned i = 0; i < OMNI_CONTROLS_QUEUE_CAPACITY; ++i) {
            assert(omni_controls_pop(&controls, &event));
            assert(event.kind == OMNI_CONTROL_HOME_MODE && event.origin == OMNI_CONTROL_REMOTE_DSP);
            assert(event.raw == state);
        }
        assert(!omni_controls_pop(&controls, &event));
    }
    frame[5] = 0;
    assert(omni_controls_receive(&controls, frame, 6) == OMNI_CONTROL_FRAME_QUEUED);
    omni_controls_t saved = controls;
    const uint8_t header[] = {0xdb, 6, 0xd2, 9, 3};
    for (unsigned i = 0; i < sizeof(header); ++i) {
        for (unsigned value = 0; value < 256u; ++value) {
            if (value == header[i]) continue;
            frame[i] = (uint8_t)value;
            assert(omni_controls_receive(&controls, frame, 6) == OMNI_CONTROL_FRAME_MALFORMED);
            assert(memcmp(&controls, &saved, sizeof(controls)) == 0);
        }
        frame[i] = header[i];
    }
    for (unsigned value = 2; value < 256u; ++value) {
        frame[5] = (uint8_t)value;
        assert(omni_controls_receive(&controls, frame, 6) == OMNI_CONTROL_FRAME_MALFORMED);
    }
    frame[5] = 0;
    for (size_t length = 0; length < 6u; ++length)
        assert(omni_controls_receive(&controls, frame, length) == OMNI_CONTROL_FRAME_MALFORMED);
    assert(omni_controls_receive(&controls, frame, 7) == OMNI_CONTROL_FRAME_MALFORMED);
    assert(omni_controls_receive(&controls, frame, SIZE_MAX) == OMNI_CONTROL_FRAME_MALFORMED);
    assert(omni_controls_receive(&controls, NULL, 6) == OMNI_CONTROL_FRAME_MALFORMED);
    assert(omni_controls_receive(NULL, frame, 6) == OMNI_CONTROL_FRAME_MALFORMED);
    assert(memcmp(&controls, &saved, sizeof(controls)) == 0);
    memset(&controls, 0, sizeof(controls));
    assert(omni_controls_receive(&controls, frame, 6) == OMNI_CONTROL_FRAME_MALFORMED);
}

static void remote_source_bias(void)
{
    omni_controls_t controls;
    omni_control_event_t event;
    uint8_t frame[] = {0xdb, 4, 0xd2, 7, 0, 0};
    assert(OMNI_CONTROL_BIAS_0 == 9 && OMNI_CONTROL_BIAS_1 == 10);
    assert(omni_controls_init(&controls, 0, 0));
    for (unsigned sub = 6; sub <= 7u; ++sub) {
        frame[3] = (uint8_t)sub;
        for (unsigned i = 0; i < OMNI_CONTROLS_QUEUE_CAPACITY; ++i)
            assert(omni_controls_receive(&controls, frame, 4) == OMNI_CONTROL_FRAME_QUEUED);
        assert(omni_controls_receive(&controls, frame, 4) == OMNI_CONTROL_FRAME_OVERFLOW);
        for (unsigned i = 0; i < OMNI_CONTROLS_QUEUE_CAPACITY; ++i) {
            assert(omni_controls_pop(&controls, &event));
            assert(event.kind == (sub == 7u ? OMNI_CONTROL_BIAS_0 : OMNI_CONTROL_BIAS_1));
            assert(event.origin == OMNI_CONTROL_REMOTE_DSP && event.raw == sub);
        }
    }
    omni_controls_t saved = controls;
    for (unsigned sub = 0; sub < 256u; ++sub) {
        if (sub == 6u || sub == 7u) continue;
        frame[3] = (uint8_t)sub;
        assert(omni_controls_receive(&controls, frame, 4) == OMNI_CONTROL_FRAME_MALFORMED);
    }
    frame[3] = 7;
    const uint8_t header[] = {0xdb, 4, 0xd2};
    for (unsigned i = 0; i < sizeof(header); ++i) {
        for (unsigned value = 0; value < 256u; ++value) {
            /* 91/07 is independently a supported Back packet. */
            if (value == header[i] || (i == 2u && value == 0x91u)) continue;
            frame[i] = (uint8_t)value;
            assert(omni_controls_receive(&controls, frame, 4) == OMNI_CONTROL_FRAME_MALFORMED);
        }
        frame[i] = header[i];
    }
    for (size_t length = 0; length < 4u; ++length)
        assert(omni_controls_receive(&controls, frame, length) == OMNI_CONTROL_FRAME_MALFORMED);
    assert(omni_controls_receive(&controls, frame, 5) == OMNI_CONTROL_FRAME_MALFORMED);
    assert(omni_controls_receive(&controls, frame, 6) == OMNI_CONTROL_FRAME_MALFORMED);
    assert(omni_controls_receive(&controls, frame, SIZE_MAX) == OMNI_CONTROL_FRAME_MALFORMED);
    assert(memcmp(&controls, &saved, sizeof(controls)) == 0);
}

static void startup_and_debounce(void)
{
    omni_controls_t controls;
    omni_control_event_t event;
    assert(omni_controls_init(&controls, 3, 0));
    for (uint32_t t = 0; t < 2000; ++t) assert(omni_controls_sample(&controls, 3, t));
    assert(!omni_controls_pop(&controls, &event));
    assert(omni_controls_sample(&controls, 0, 2000));
    for (uint32_t t = 2001; t <= 2005; ++t) assert(omni_controls_sample(&controls, 0, t));
    assert(!omni_controls_pop(&controls, &event));
    assert(omni_controls_sample(&controls, 3, 2010));
    assert(omni_controls_sample(&controls, 3, 2015));
    assert(omni_controls_sample(&controls, 0, 2020));
    assert(omni_controls_sample(&controls, 0, 2024));
    assert(!omni_controls_pop(&controls, &event));
    assert(omni_controls_sample(&controls, 0, 2025));
    assert(omni_controls_pop(&controls, &event) && event.kind == OMNI_CONTROL_SELECT);
    assert(omni_controls_pop(&controls, &event) && event.kind == OMNI_CONTROL_BACK);
    assert(!omni_controls_pop(&controls, &event));

    /* Multiple input changes restart the stable-sample interval. */
    assert(omni_controls_init(&controls, 0, 0));
    assert(omni_controls_sample(&controls, 1, 0));
    assert(omni_controls_sample(&controls, 0, 4));
    assert(omni_controls_sample(&controls, 1, 8));
    for (unsigned i = 0; i < 100; ++i) assert(omni_controls_sample(&controls, 1, 12));
    assert(!controls.buttons[0].stable);
    assert(omni_controls_sample(&controls, 1, 13));
    assert(controls.buttons[0].stable);
    assert(omni_controls_sample(&controls, 0, 20));
    assert(omni_controls_sample(&controls, 0, 25));
    assert(omni_controls_pop(&controls, &event) && event.kind == OMNI_CONTROL_SELECT);
    assert(!omni_controls_pop(&controls, &event));
}

static void fifo_and_overflow(void)
{
    omni_controls_t controls;
    omni_control_event_t event;
    const uint8_t select[] = {0xdb, 4, 0x91, 4};
    const uint8_t back[] = {0xdb, 4, 0x91, 7};
    assert(omni_controls_init(&controls, 0, 0));
    for (unsigned i = 0; i < OMNI_CONTROLS_QUEUE_CAPACITY; ++i)
        assert(omni_controls_receive(&controls, select, sizeof(select)) == OMNI_CONTROL_FRAME_QUEUED);
    assert(omni_controls_receive(&controls, back, sizeof(back)) == OMNI_CONTROL_FRAME_OVERFLOW);
    assert(controls.dropped_events == 1u && controls.count == OMNI_CONTROLS_QUEUE_CAPACITY);
    assert(omni_controls_sample(&controls, 1, 0));
    assert(omni_controls_sample(&controls, 1, 5));
    assert(omni_controls_sample(&controls, 0, 10));
    assert(omni_controls_sample(&controls, 0, 15));
    assert(controls.dropped_events == 2u);
    for (unsigned i = 0; i < OMNI_CONTROLS_QUEUE_CAPACITY; ++i) {
        assert(omni_controls_pop(&controls, &event));
        assert(event.kind == OMNI_CONTROL_SELECT && event.origin == OMNI_CONTROL_REMOTE_DSP);
    }
    assert(omni_controls_sample(&controls, 0, 2000));
    assert(!omni_controls_pop(&controls, &event)); /* No replay after overflow. */
    for (unsigned cycle = 0; cycle < 24u; ++cycle) {
        assert(omni_controls_receive(&controls, back, sizeof(back)) == OMNI_CONTROL_FRAME_QUEUED);
        assert(omni_controls_sample(&controls, 1, 2010u + cycle * 20u));
        assert(omni_controls_sample(&controls, 1, 2015u + cycle * 20u));
        assert(omni_controls_sample(&controls, 0, 2020u + cycle * 20u));
        assert(omni_controls_sample(&controls, 0, 2025u + cycle * 20u));
        assert(omni_controls_pop(&controls, &event) && event.kind == OMNI_CONTROL_BACK);
        assert(omni_controls_pop(&controls, &event) && event.origin == OMNI_CONTROL_LOCAL_PUSH);
        assert(!omni_controls_pop(&controls, &event));
    }
    controls.dropped_events = UINT32_MAX;
    for (unsigned i = 0; i < OMNI_CONTROLS_QUEUE_CAPACITY; ++i)
        assert(omni_controls_receive(&controls, select, sizeof(select)) == OMNI_CONTROL_FRAME_QUEUED);
    assert(omni_controls_receive(&controls, back, sizeof(back)) == OMNI_CONTROL_FRAME_OVERFLOW);
    assert(controls.dropped_events == UINT32_MAX);
}

static void bad_arguments_and_time(void)
{
    omni_controls_t controls;
    omni_control_event_t event;
    memset(&controls, 0, sizeof(controls));
    assert(!omni_controls_sample(&controls, 0, 0));
    assert(!omni_controls_pop(&controls, &event));
    assert(!omni_controls_init(NULL, 0, 0));
    assert(!omni_controls_init(&controls, 4, 0));
    assert(omni_controls_init(&controls, 0, 100));
    omni_controls_t saved = controls;
    assert(!omni_controls_sample(&controls, 4, 100));
    assert(!omni_controls_sample(&controls, 0, 99));
    assert(!omni_controls_sample(&controls, 0, 100u + 0x80000000u));
    assert(memcmp(&controls, &saved, sizeof(controls)) == 0);
    assert(!omni_controls_sample(NULL, 0, 100));
    assert(!omni_controls_pop(&controls, NULL));
    assert(!omni_controls_pop(NULL, &event));
}

int main(void)
{
    stock_local_cases(); remote_golden_and_validation(); remote_home_mode(); remote_source_bias();
    startup_and_debounce();
    fifo_and_overflow(); bad_arguments_and_time();
    puts("controls: stock normal-mode gestures and remote frames, bounds, timing and FIFO PASS");
    return 0;
}
