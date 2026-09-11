#include "audio_mode.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t now, last_write, last_drain, isolated_at, connected_at, resumed_at;
    uint32_t configured_rate;
    unsigned callbacks, begin_count, config_polls, off_calls, on_calls;
    unsigned write_limit, backpressure;
    int write_error_after;
    bool oversize, drain_forever, drain_error, configure_error, begin_error;
    bool configure_forever, pins_off_error, pins_on_error, isolated, configured;
    uint8_t bytes[64]; size_t length;
} fixture_t;

static int write_bytes(void *context, const uint8_t *data, size_t length)
{
    fixture_t *f = context; ++f->callbacks;
    if (f->oversize) return (int)length + 1;
    if (f->write_error_after >= 0 && f->length >= (size_t)f->write_error_after) return -1;
    if (f->backpressure != 0u) { --f->backpressure; return 0; }
    /* Independent stock UART callback evidence requires20ms after the prior
     * TX completion before a new queued frame begins. Partial writes within
     * a frame have no such delay. */
    if (f->length == 6u || f->length == 17u)
        assert((uint32_t)(f->now - f->last_drain) >= 20u);
    if (length > f->write_limit) length = f->write_limit;
    assert(f->length + length <= sizeof(f->bytes));
    if (f->length == 12u) f->resumed_at = f->now;
    memcpy(f->bytes + f->length, data, length); f->length += length; f->last_write = f->now;
    return (int)length;
}

static omni_audio_mode_io_t tx_status(void *context)
{
    fixture_t *f = context; ++f->callbacks;
    if (f->drain_error) return (omni_audio_mode_io_t)2;
    if (f->drain_forever || (uint32_t)(f->now - f->last_write) < 3u) return OMNI_AUDIO_MODE_IO_PENDING;
    f->last_drain = f->now;
    return OMNI_AUDIO_MODE_IO_COMPLETE;
}

static bool pins(void *context, bool enabled)
{
    fixture_t *f = context; ++f->callbacks;
    if (enabled) {
        ++f->on_calls;
        assert(f->configured);
        /* Even a failed enable may have touched a pin; code must isolate again. */
        f->isolated = false;
        if (f->pins_on_error) return false;
        f->connected_at = f->now;
    } else {
        ++f->off_calls;
        if (f->pins_off_error) return false;
        f->isolated = true; f->isolated_at = f->now;
    }
    return true;
}

static bool configure_begin(void *context, uint32_t rate)
{
    fixture_t *f = context; ++f->callbacks; ++f->begin_count;
    assert(f->isolated && f->length == 12u);
    assert((uint32_t)(f->now - f->last_drain) >= 75u);
    f->configured_rate = rate;
    return !f->begin_error;
}

static omni_audio_mode_io_t configure_poll(void *context)
{
    fixture_t *f = context; ++f->callbacks; ++f->config_polls;
    assert(f->isolated);
    if (f->configure_error) return OMNI_AUDIO_MODE_IO_ERROR;
    if (f->configure_forever || f->config_polls < 3u) return OMNI_AUDIO_MODE_IO_PENDING;
    f->configured = true;
    return OMNI_AUDIO_MODE_IO_COMPLETE;
}

static void setup(omni_audio_mode_t *mode, fixture_t *f, uint32_t rate, uint32_t start)
{
    memset(f, 0, sizeof(*f)); f->write_limit = 2; f->write_error_after = -1;
    omni_audio_mode_ops_t ops = {f, write_bytes, tx_status, pins, configure_begin, configure_poll};
    assert(omni_audio_mode_init(mode, &ops));
    assert(omni_audio_mode_begin(mode, rate, start));
    assert(!omni_audio_mode_begin(mode, rate, start));
    f->now = start;
}

static void tick(omni_audio_mode_t *mode, fixture_t *f)
{
    unsigned before = f->callbacks;
    omni_audio_mode_poll(mode, f->now);
    assert(f->callbacks - before <= 2u);
    ++f->now;
}

static void reach(omni_audio_mode_t *mode, fixture_t *f, omni_audio_mode_state_t state)
{
    unsigned ticks = 0;
    while (mode->state != state && ticks++ < 1100u) tick(mode, f);
    assert(mode->state == state);
}

static void successful_sequence(uint32_t rate, uint32_t start)
{
    fixture_t f; omni_audio_mode_t mode; setup(&mode, &f, rate, start);
    f.backpressure = 5;
    for (unsigned i = 0; i < 50; ++i) tick(&mode, &f);
    assert(f.length == 0 && f.callbacks == 0);
    reach(&mode, &f, OMNI_AUDIO_MODE_LOCAL_COMPLETE);
    /* Full command sequence comes from independent execution of stock2589c and
     * its event136/137 dispatch, not a copy of the new transition code. */
    uint8_t expected[] = {0xbd,6,0x51,1,4,0, 0xbd,6,0x88,1,0,0,
                          0xbd,5,0x88,1,0, 0xbd,6,0x51,1,1,0};
    if (rate == 96000u) expected[22] = 1;
    assert(f.length == sizeof(expected) && memcmp(f.bytes, expected, sizeof(expected)) == 0);
    assert(mode.transmitted_bytes == 23 && f.begin_count == 1 && f.configured_rate == rate);
    assert(f.off_calls == 1 && f.on_calls == 1 && !mode.pins_isolated && !f.isolated);
    assert((uint32_t)(f.resumed_at - f.connected_at) >= 75u);
    assert((uint32_t)(mode.phase_ms - f.last_drain) >= 20u);
    assert(!mode.isolation_failed && mode.error == OMNI_AUDIO_MODE_ERROR_NONE);
    unsigned before = f.callbacks;
    omni_audio_mode_poll(&mode, f.now + 5000u); omni_audio_mode_cancel(&mode);
    assert(mode.state == OMNI_AUDIO_MODE_LOCAL_COMPLETE && f.callbacks == before);
}

static void interframe_boundaries(uint32_t start)
{
    fixture_t f; omni_audio_mode_t mode; setup(&mode, &f, 48000, start);
    const omni_audio_mode_state_t gaps[] = {
        OMNI_AUDIO_MODE_STOP_GAP, OMNI_AUDIO_MODE_RESUME_GAP, OMNI_AUDIO_MODE_RATE_GAP
    };
    const omni_audio_mode_state_t next[] = {
        OMNI_AUDIO_MODE_SEND_GATE, OMNI_AUDIO_MODE_SEND_RATE, OMNI_AUDIO_MODE_LOCAL_COMPLETE
    };
    for (unsigned i = 0; i < 3u; ++i) {
        reach(&mode, &f, gaps[i]);
        uint32_t drained_at = f.last_drain;
        size_t bytes = f.length;
        unsigned callbacks = f.callbacks;
        /* Many polls without elapsed wall time cannot bypass cooldown. */
        for (unsigned j = 0; j < 50u; ++j)
            omni_audio_mode_poll(&mode, drained_at + 19u);
        assert(mode.state == gaps[i] && f.length == bytes && f.callbacks == callbacks);
        omni_audio_mode_poll(&mode, drained_at + 20u);
        assert(mode.state == next[i] && f.length == bytes && f.callbacks == callbacks);
        f.now = drained_at + 20u;
    }
    assert(mode.state == OMNI_AUDIO_MODE_LOCAL_COMPLETE);
}

static void transport_failures(void)
{
    fixture_t f; omni_audio_mode_t mode; setup(&mode, &f, 48000, 0);
    f.write_error_after = 2;
    reach(&mode, &f, OMNI_AUDIO_MODE_FAILED);
    assert(mode.error == OMNI_AUDIO_MODE_ERROR_TX && f.length == 2 && f.begin_count == 0);
    assert(mode.pins_isolated && f.isolated);
    assert(!omni_audio_mode_begin(&mode, 48000, f.now));
    unsigned calls = f.callbacks; tick(&mode, &f); assert(f.callbacks == calls);

    setup(&mode, &f, 48000, 0); f.oversize = true;
    reach(&mode, &f, OMNI_AUDIO_MODE_FAILED);
    assert(mode.error == OMNI_AUDIO_MODE_ERROR_TX && mode.transmitted_bytes == 0);
    setup(&mode, &f, 48000, 0); f.drain_error = true;
    reach(&mode, &f, OMNI_AUDIO_MODE_FAILED);
    assert(mode.error == OMNI_AUDIO_MODE_ERROR_TX && f.length == 6 && f.begin_count == 0);
    setup(&mode, &f, 48000, 0); f.drain_forever = true;
    reach(&mode, &f, OMNI_AUDIO_MODE_FAILED);
    assert(mode.error == OMNI_AUDIO_MODE_ERROR_TIMEOUT && f.length == 6 && f.begin_count == 0);
    setup(&mode, &f, 48000, 0); f.backpressure = 2000;
    reach(&mode, &f, OMNI_AUDIO_MODE_FAILED);
    assert(mode.error == OMNI_AUDIO_MODE_ERROR_TIMEOUT && f.length == 0);
}

static void backend_failures(void)
{
    fixture_t f; omni_audio_mode_t mode;
    setup(&mode, &f, 48000, 0); f.configure_error = true;
    reach(&mode, &f, OMNI_AUDIO_MODE_FAILED);
    assert(mode.error == OMNI_AUDIO_MODE_ERROR_CONFIGURATION && f.length == 12);
    assert(f.on_calls == 0 && f.isolated && mode.pins_isolated && f.begin_count == 1);
    setup(&mode, &f, 48000, 0); f.begin_error = true;
    reach(&mode, &f, OMNI_AUDIO_MODE_FAILED);
    assert(mode.error == OMNI_AUDIO_MODE_ERROR_CONFIGURATION && f.config_polls == 0 && f.on_calls == 0);
    setup(&mode, &f, 96000, UINT32_MAX - 100u); f.configure_forever = true;
    reach(&mode, &f, OMNI_AUDIO_MODE_FAILED);
    assert(mode.error == OMNI_AUDIO_MODE_ERROR_TIMEOUT && f.length == 12 && f.on_calls == 0);
    setup(&mode, &f, 48000, 0); f.pins_off_error = true;
    reach(&mode, &f, OMNI_AUDIO_MODE_FAILED);
    assert(mode.error == OMNI_AUDIO_MODE_ERROR_PINS && mode.isolation_failed && !mode.pins_isolated);
    assert(f.off_calls == 1 && f.begin_count == 0);
    setup(&mode, &f, 48000, 0); f.pins_on_error = true;
    reach(&mode, &f, OMNI_AUDIO_MODE_FAILED);
    assert(mode.error == OMNI_AUDIO_MODE_ERROR_PINS && mode.pins_isolated && f.isolated);
    assert(f.off_calls == 2 && f.on_calls == 1 && f.length == 12);
}

static void cancellation_and_arguments(void)
{
    fixture_t f; omni_audio_mode_t mode;
    setup(&mode, &f, 48000, 0); reach(&mode, &f, OMNI_AUDIO_MODE_CONFIGURING);
    omni_audio_mode_cancel(&mode);
    assert(mode.state == OMNI_AUDIO_MODE_CANCELED && f.isolated && f.length == 12 && f.on_calls == 0);
    assert(!omni_audio_mode_begin(&mode, 48000, f.now));
    setup(&mode, &f, 48000, 0); f.pins_off_error = true; omni_audio_mode_cancel(&mode);
    assert(mode.state == OMNI_AUDIO_MODE_CANCELED && mode.isolation_failed);
    setup(&mode, &f, 48000, 0); reach(&mode, &f, OMNI_AUDIO_MODE_RATE_DRAIN);
    omni_audio_mode_cancel(&mode);
    assert(mode.state == OMNI_AUDIO_MODE_CANCELED && mode.pins_isolated && f.length == 23);
    assert(!omni_audio_mode_init(NULL, &mode.ops));
    assert(!omni_audio_mode_init(&mode, NULL));
    omni_audio_mode_ops_t invalid = mode.ops; invalid.write = NULL;
    assert(!omni_audio_mode_init(&mode, &invalid));
    assert(omni_audio_mode_init(&mode, &mode.ops));
    assert(!omni_audio_mode_begin(&mode, 44100, 0));
    assert(!omni_audio_mode_begin(NULL, 48000, 0));
    memset(&mode, 0, sizeof(mode)); assert(!omni_audio_mode_begin(&mode, 48000, 0));
    omni_audio_mode_poll(NULL, 0); omni_audio_mode_cancel(NULL);
}

static void acknowledged_sequences(void)
{
    /* Replies are supplied by complete stock wire-command boundaries, not by
     * private state enums. Includes a rate change across uint32 clock wrap. */
    for (unsigned rate = 0; rate < 2u; ++rate) {
        fixture_t f; omni_audio_mode_t mode;
        setup(&mode, &f, rate ? 96000u : 48000u, rate ? UINT32_MAX-100u : 0u);
        assert(omni_audio_mode_require_ack(&mode));
        const size_t boundaries[]={6u,12u,17u,23u};
        const uint8_t opcodes[]={0x51u,0x88u,0x88u,0x51u};
        unsigned reply=0;
        for(unsigned tick_count=0;tick_count<1000u && mode.state!=OMNI_AUDIO_MODE_LOCAL_COMPLETE;++tick_count) {
            tick(&mode,&f);
            if(reply<4u && f.length==boundaries[reply] &&
                (uint32_t)(f.now-f.last_write)>=8u) {
                uint8_t ack[]={0xdd,3,opcodes[reply],0};
                const uint8_t wrong[]={0xdd,3,0x43,0};
                assert(!omni_audio_mode_receive_ack(&mode,wrong,sizeof(wrong)));
                assert(!omni_audio_mode_receive_ack(&mode,ack,3u));
                assert(omni_audio_mode_receive_ack(&mode,ack,sizeof(ack)));
                assert(!omni_audio_mode_receive_ack(&mode,ack,sizeof(ack)));
                ++reply;
            }
        }
        assert(mode.state==OMNI_AUDIO_MODE_LOCAL_COMPLETE && reply==4u);
        assert(mode.acknowledged_frames==4u && mode.ack_status==0u && f.begin_count==1u);
        assert(f.configured_rate==(rate?96000u:48000u));
    }
    fixture_t f; omni_audio_mode_t mode;
    const uint8_t ack[]={0xdd,3,0x51,0},nack[]={0xdd,3,0x51,1};
    setup(&mode,&f,96000u,0u);assert(omni_audio_mode_require_ack(&mode));
    assert(!omni_audio_mode_receive_ack(&mode,ack,sizeof(ack))); /* Before any TX. */
    reach(&mode,&f,OMNI_AUDIO_MODE_STOP_GAP);
    assert(!omni_audio_mode_require_ack(&mode)); /* Cannot alter a live policy. */
    reach(&mode,&f,OMNI_AUDIO_MODE_FAILED);
    assert(mode.error==OMNI_AUDIO_MODE_ERROR_TIMEOUT && f.length==6u && !f.begin_count);
    setup(&mode,&f,96000u,0u);assert(omni_audio_mode_require_ack(&mode));
    reach(&mode,&f,OMNI_AUDIO_MODE_STOP_GAP);
    assert(omni_audio_mode_receive_ack(&mode,nack,sizeof(nack)));
    assert(mode.state==OMNI_AUDIO_MODE_FAILED && mode.error==OMNI_AUDIO_MODE_ERROR_REJECTED);
    assert(mode.pins_isolated && !mode.acknowledged_frames && !f.begin_count);
    assert(!omni_audio_mode_require_ack(NULL));
    assert(!omni_audio_mode_receive_ack(NULL,ack,sizeof(ack)));
}

static void quiesce_sequences(void)
{
    const omni_audio_mode_state_t sending[]={OMNI_AUDIO_MODE_SEND_STOP,
        OMNI_AUDIO_MODE_SEND_GATE,OMNI_AUDIO_MODE_SEND_RESUME,OMNI_AUDIO_MODE_SEND_RATE};
    const size_t ends[]={6u,12u,17u,23u};
    for(unsigned i=0;i<4u;++i) {
        fixture_t f;omni_audio_mode_t mode;
        setup(&mode,&f,96000u,UINT32_MAX-100u);
        reach(&mode,&f,sending[i]);tick(&mode,&f);
        assert(mode.frame_offset==2u);
        omni_audio_mode_io_t result=OMNI_AUDIO_MODE_IO_PENDING;
        for(unsigned count=0;count<100u && result==OMNI_AUDIO_MODE_IO_PENDING;++count) {
            result=omni_audio_mode_quiesce(&mode,f.now++);
            size_t before=f.length;
            omni_audio_mode_poll(&mode,f.now);assert(f.length==before);
        }
        assert(result==OMNI_AUDIO_MODE_IO_COMPLETE && mode.state==OMNI_AUDIO_MODE_CANCELED);
        assert(f.length==ends[i] && f.isolated && mode.pins_isolated);
        assert((uint32_t)(f.now-mode.quiesce_drained_ms)>=20u);
    }
    fixture_t f;omni_audio_mode_t mode;
    setup(&mode,&f,48000u,0u);
    /* Closing before the first byte never starts STOP merely to cancel it. */
    omni_audio_mode_io_t result=OMNI_AUDIO_MODE_IO_PENDING;
    while(result==OMNI_AUDIO_MODE_IO_PENDING) result=omni_audio_mode_quiesce(&mode,f.now++);
    assert(result==OMNI_AUDIO_MODE_IO_COMPLETE && !f.length && !f.begin_count);
    setup(&mode,&f,48000u,0u);reach(&mode,&f,OMNI_AUDIO_MODE_SEND_STOP);tick(&mode,&f);
    f.drain_forever=true;
    result=OMNI_AUDIO_MODE_IO_PENDING;
    while(result==OMNI_AUDIO_MODE_IO_PENDING) result=omni_audio_mode_quiesce(&mode,f.now++);
    assert(result==OMNI_AUDIO_MODE_IO_ERROR && mode.error==OMNI_AUDIO_MODE_ERROR_TIMEOUT);
    assert(f.length==6u && !f.begin_count && f.isolated);
}

int main(void)
{
    successful_sequence(48000, 0); successful_sequence(96000, UINT32_MAX - 100u);
    interframe_boundaries(0); interframe_boundaries(UINT32_MAX - 70u);
    transport_failures(); backend_failures(); cancellation_and_arguments();
    acknowledged_sequences();
    quiesce_sequences();
    puts("audio mode stock sequence, timing and failures: PASS");
    return 0;
}
