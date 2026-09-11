#include "dsp_capture.h"
#include "mcu2_link.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t bytes[8192];
    size_t length, available, position, error_after;
    unsigned rx_calls, tx_calls;
    int error_result;
} fixture;

static int forbidden_tx(void *context, uint8_t byte)
{
    fixture *f = context;
    (void)byte;
    ++f->tx_calls;
    assert(!"passive capture must not transmit");
    return -1;
}

static int read_byte(void *context, uint8_t *byte)
{
    fixture *f = context;
    ++f->rx_calls;
    if (f->position >= f->error_after) return f->error_result;
    if (f->position >= f->available) return 0;
    assert(f->position < f->length);
    *byte = f->bytes[f->position++];
    return 1;
}

static void setup(omni_dsp_capture *capture, fixture *f, uint32_t start, uint32_t duration)
{
    memset(f, 0, sizeof(*f));
    f->error_after = SIZE_MAX;
    f->error_result = -1;
    mcu2_link_io io = {f, forbidden_tx, read_byte};
    /* Only the receive half of the existing byte-I/O contract is supplied. */
    assert(omni_dsp_capture_init(capture, io.context, io.rx));
    assert(omni_dsp_capture_begin(capture, duration, start));
}

static void append(fixture *f, const uint8_t *bytes, size_t length)
{
    assert(f->length + length <= sizeof(f->bytes));
    memcpy(f->bytes + f->length, bytes, length);
    f->length += length;
}

static void poll(omni_dsp_capture *capture, fixture *f, uint32_t now)
{
    unsigned before = f->rx_calls;
    omni_dsp_capture_poll(capture, now);
    assert(f->rx_calls - before <= 32u && f->tx_calls == 0u);
}

static omni_dsp_capture_status status(const omni_dsp_capture *capture)
{
    omni_dsp_capture_status out;
    assert(omni_dsp_capture_get_status(capture, &out));
    return out;
}

static void records_and_fragmentation(void)
{
    fixture f;
    omni_dsp_capture capture;
    setup(&capture, &f, 100, 1000);
    const uint8_t e1[] = {0xdb, 13, 0xe1, 3, 0, 0x36, 0, 0, 0x21, 0x22, 0x23, 0x24, 0x25};
    const uint8_t nav[] = {0xdb, 4, 0x91, 7};
    const uint8_t race[] = {0x15, 0x5b, 4, 0, 0xdb, 0xdd, 5, 0x15};
    const uint8_t dd[] = {0xdd, 0xdb, 5, 0x15};
    append(&f, e1, sizeof(e1)); append(&f, nav, sizeof(nav));
    append(&f, race, sizeof(race)); append(&f, dd, sizeof(dd));
    for (size_t i = 1; i <= f.length; ++i) {
        f.available = i;
        poll(&capture, &f, 100u + (uint32_t)i);
        omni_dsp_capture_record record;
        omni_dsp_capture_status live_status;
        assert(!omni_dsp_capture_get_record(&capture, 0, &record));
        assert(!omni_dsp_capture_get_status(&capture, &live_status));
    }
    assert(capture.phase == OMNI_DSP_CAPTURE_RUNNING);
    unsigned before = f.rx_calls;
    poll(&capture, &f, 1100);
    assert(f.rx_calls == before);
    omni_dsp_capture_status s = status(&capture);
    assert(s.phase == OMNI_DSP_CAPTURE_COMPLETE && s.started_ms == 100u && s.finished_ms == 1100u);
    assert(s.duration_ms == 1000u && s.rx_bytes == f.length && s.received_frames == 3u);
    assert(s.stored_records == 3u && s.ignored_frames == 1u && s.overflow_frames == 0u);
    assert(s.malformed_headers == 0u && s.expired_partial_frames == 0u && s.pending_bytes == 0u);
    const uint8_t *expected[] = {e1, nav, race};
    const size_t lengths[] = {sizeof(e1), sizeof(nav), sizeof(race)};
    uint32_t received_at = 100;
    for (unsigned i = 0; i < 3u; ++i) {
        omni_dsp_capture_record record;
        assert(omni_dsp_capture_get_record(&capture, i, &record));
        received_at += (uint32_t)lengths[i];
        assert(record.sequence == i + 1u && record.received_ms == received_at);
        assert(record.length == lengths[i] && memcmp(record.raw, expected[i], lengths[i]) == 0);
        for (size_t j = lengths[i]; j < sizeof(record.raw); ++j) assert(record.raw[j] == 0u);
    }
    /* Frozen objects do not change or consume more RX even on later polls. */
    omni_dsp_capture saved = capture;
    poll(&capture, &f, 1200);
    omni_dsp_capture_cancel(&capture, 1200);
    omni_dsp_capture_fail(&capture, -7, 1200);
    assert(memcmp(&capture, &saved, sizeof(capture)) == 0);
}

static void maximum_lengths_and_overflow(void)
{
    fixture f;
    omni_dsp_capture capture;
    setup(&capture, &f, 0, 1000);
    uint8_t race[204] = {0x15, 0x5a, 200, 0};
    for (unsigned i = 4; i < sizeof(race); ++i) race[i] = (uint8_t)i;
    uint8_t db[200] = {0xdb, 200, 0x91, 4};
    for (unsigned i = 4; i < sizeof(db); ++i) db[i] = (uint8_t)(255u - i);
    append(&f, race, sizeof(race)); append(&f, db, sizeof(db));
    for (unsigned sequence = 3; sequence <= 20u; ++sequence) {
        uint8_t frame[] = {0xdb, 4, 0x91, (uint8_t)sequence};
        append(&f, frame, sizeof(frame));
    }
    f.available = f.length;
    uint32_t now = 0;
    while (f.position < f.length) poll(&capture, &f, now++);
    assert(capture.phase == OMNI_DSP_CAPTURE_RUNNING);
    poll(&capture, &f, 1000);
    omni_dsp_capture_status s = status(&capture);
    assert(s.rx_bytes == f.length && s.received_frames == 20u && s.stored_records == 16u);
    assert(s.overflow_frames == 4u && s.malformed_headers == 0u && s.expired_partial_frames == 0u);
    omni_dsp_capture_record record;
    assert(omni_dsp_capture_get_record(&capture, 0, &record));
    assert(record.length == 204u && memcmp(record.raw, race, sizeof(race)) == 0);
    assert(omni_dsp_capture_get_record(&capture, 1, &record));
    assert(record.length == 200u && memcmp(record.raw, db, sizeof(db)) == 0);
    for (unsigned i = 2; i < 16u; ++i) {
        assert(omni_dsp_capture_get_record(&capture, i, &record));
        assert(record.sequence == i + 1u && record.length == 4u && record.raw[3] == i + 1u);
    }
    assert(!omni_dsp_capture_get_record(&capture, 16, &record));
    assert(!omni_dsp_capture_get_record(&capture, UINT32_MAX, &record));
}

static void malformed_expiry_and_deadline(void)
{
    fixture f;
    omni_dsp_capture capture;
    setup(&capture, &f, 0, 1000);
    const uint8_t bad[] = {0x99, 0x98, 0xdb, 2, 5, 0x59, 0x15, 0x5b, 201, 0};
    append(&f, bad, sizeof(bad)); f.available = f.length;
    poll(&capture, &f, 0);
    assert(capture.parser.malformed == 3u);
    const uint8_t partial[] = {0xdb, 13, 0xe1, 3};
    append(&f, partial, sizeof(partial)); f.available = f.length;
    poll(&capture, &f, 10); poll(&capture, &f, 29);
    assert(capture.parser.used == 4u && capture.parser.expired == 0u);
    poll(&capture, &f, 30);
    assert(capture.parser.used == 0u && capture.parser.expired == 1u);
    const uint8_t valid[] = {0xdb, 4, 0x91, 4};
    append(&f, valid, sizeof(valid)); f.available = f.length;
    poll(&capture, &f, 31);
    append(&f, partial, sizeof(partial)); f.available = f.length;
    poll(&capture, &f, 990);
    size_t pending_position = f.position;
    append(&f, valid, sizeof(valid)); f.available = f.length;
    poll(&capture, &f, 1000);
    omni_dsp_capture_status s = status(&capture);
    assert(f.position == pending_position); /* No post-deadline byte consumed. */
    assert(s.malformed_headers == 3u && s.expired_partial_frames == 1u);
    assert(s.pending_bytes == 4u && s.expected_bytes == 13u && s.stored_records == 1u);
    assert(s.received_frames == 1u);

    setup(&capture, &f, 0, 1000);
    append(&f, partial, sizeof(partial)); f.available = f.length;
    poll(&capture, &f, 0); poll(&capture, &f, 1000);
    s = status(&capture);
    assert(s.stored_records == 0u && s.expired_partial_frames == 1u && s.pending_bytes == 0u);
}

static void cancellation_errors_and_wrap(void)
{
    fixture f;
    omni_dsp_capture capture;
    const uint8_t stream[] = {0xdb, 4, 0x91, 7, 0x15, 0x5b, 4};
    setup(&capture, &f, UINT32_MAX - 100u, 1000);
    append(&f, stream, sizeof(stream)); f.available = f.length;
    poll(&capture, &f, UINT32_MAX - 90u);
    omni_dsp_capture_cancel(&capture, 0);
    omni_dsp_capture_status s = status(&capture);
    assert(s.phase == OMNI_DSP_CAPTURE_CANCELLED && s.pending_bytes == 3u && s.stored_records == 1u);
    assert(s.finished_ms == 0u);

    for (int error = -7; error <= 2; error += 9) {
        setup(&capture, &f, 0, 1000);
        append(&f, stream, sizeof(stream)); f.available = f.length;
        f.error_after = 6; f.error_result = error;
        poll(&capture, &f, 1);
        s = status(&capture);
        assert(s.phase == OMNI_DSP_CAPTURE_IO_ERROR && s.io_result == error);
        assert(s.rx_bytes == 6u && s.stored_records == 1u && s.pending_bytes == 2u);
    }
    setup(&capture, &f, 0, 1000);
    omni_dsp_capture_fail(&capture, -42, 1);
    assert(status(&capture).io_result == -42 && f.rx_calls == 0u);
    setup(&capture, &f, 0, 1000);
    omni_dsp_capture_fail(&capture, 0, 1);
    assert(status(&capture).io_result == -1);

    setup(&capture, &f, UINT32_MAX - 100u, 1000);
    poll(&capture, &f, 898);
    assert(capture.phase == OMNI_DSP_CAPTURE_RUNNING);
    unsigned before = f.rx_calls;
    poll(&capture, &f, 899);
    assert(status(&capture).phase == OMNI_DSP_CAPTURE_COMPLETE && f.rx_calls == before);
    setup(&capture, &f, 100, 1000);
    poll(&capture, &f, 99);
    assert(status(&capture).phase == OMNI_DSP_CAPTURE_BAD_TIME && f.rx_calls == 0u);
}

static void arguments_and_reuse(void)
{
    fixture f;
    omni_dsp_capture capture;
    memset(&capture, 0, sizeof(capture));
    assert(!omni_dsp_capture_init(NULL, &f, read_byte));
    assert(!omni_dsp_capture_init(&capture, &f, NULL));
    assert(!omni_dsp_capture_begin(&capture, 1000, 0));
    assert(omni_dsp_capture_init(&capture, &f, read_byte));
    assert(!omni_dsp_capture_begin(&capture, 999, 0));
    assert(!omni_dsp_capture_begin(&capture, 30001, 0));
    omni_dsp_capture_record record, record_saved;
    omni_dsp_capture_status s, saved;
    memset(&record, 0x55, sizeof(record)); record_saved = record;
    memset(&s, 0x55, sizeof(s)); saved = s;
    assert(!omni_dsp_capture_get_record(&capture, 0, &record));
    assert(!omni_dsp_capture_get_status(&capture, &s));
    assert(memcmp(&record, &record_saved, sizeof(record)) == 0 && memcmp(&s, &saved, sizeof(s)) == 0);
    setup(&capture, &f, 0, 30000);
    assert(!omni_dsp_capture_begin(&capture, 1000, 1));
    poll(&capture, &f, 30000);
    assert(status(&capture).phase == OMNI_DSP_CAPTURE_COMPLETE);
    assert(!omni_dsp_capture_get_record(&capture, 0, &record));
    assert(!omni_dsp_capture_get_record(&capture, 0, NULL));
    assert(!omni_dsp_capture_get_status(&capture, NULL));
    assert(!omni_dsp_capture_get_status(NULL, &s));
    assert(!omni_dsp_capture_get_record(NULL, 0, &record));
    assert(omni_dsp_capture_begin(&capture, 1000, 30001));
    assert(capture.phase == OMNI_DSP_CAPTURE_RUNNING && capture.stored_records == 0u);
    assert(!omni_dsp_capture_get_status(&capture, &s));
    assert(!omni_dsp_capture_get_record(&capture, 0, &record));
    omni_dsp_capture_poll(NULL, 0); omni_dsp_capture_cancel(NULL, 0); omni_dsp_capture_fail(NULL, 0, 0);
}

int main(void)
{
    records_and_fragmentation(); maximum_lengths_and_overflow();
    malformed_expiry_and_deadline(); cancellation_errors_and_wrap(); arguments_and_reuse();
    puts("passive DSP capture: full frames, bounded RX, frozen evidence and failure tests PASS");
    return 0;
}
