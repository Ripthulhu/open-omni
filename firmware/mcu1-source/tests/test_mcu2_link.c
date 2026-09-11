#include "mcu2_link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t tx[1036], rx[2072];
    unsigned sent, received, available, tx_calls, rx_calls;
    int tx_error, rx_error;
    bool tx_full, early_rx;
} transport;
static int tx(void *context, uint8_t byte)
{
    transport *t = context; ++t->tx_calls;
    if (t->tx_error) return t->tx_error;
    if (t->tx_full) return 0;
    assert(t->sent < sizeof(t->tx));
    t->tx[t->sent++] = byte;
    return 1;
}
static int rx(void *context, uint8_t *byte)
{
    transport *t = context; ++t->rx_calls;
    if (t->rx_error) return t->rx_error;
    if ((!t->early_rx && t->sent != 1036U) || t->received == t->available) return 0;
    *byte = t->rx[t->received++]; return 1;
}
static void setup(omni_mcu2_link *link, transport *t, uint32_t now)
{
    memset(t, 0, sizeof(*t));
    const uint8_t reply[] = {0xcb,7,0xe1,3,1,0x32,0};
    memcpy(t->rx, reply, sizeof(reply)); t->available = 1036;
    mcu2_link_io io = {t, tx, rx};
    assert(omni_mcu2_link_init(link, io));
    assert(link->trace_first_tx_ms == UINT32_MAX && link->trace_last_tx_ms == UINT32_MAX);
    assert(link->trace_first_rx_ms == UINT32_MAX && link->trace_last_rx_ms == UINT32_MAX);
    assert(omni_mcu2_link_start(link, now));
    assert(!omni_mcu2_link_start(link, now));
}
static void poll(omni_mcu2_link *link, transport *t, uint32_t now)
{
    t->tx_calls = t->rx_calls = 0;
    omni_mcu2_link_poll(link, now);
    assert(t->tx_calls <= 16 && t->rx_calls <= 32);
    assert(link->trace_rx_used <= OMNI_MCU2_TRACE_RX_BYTES);
    assert(link->trace_rx_used + link->trace_rx_truncated == link->rx_bytes);
    assert(!memcmp(link->trace_rx, t->rx, link->trace_rx_used));
    assert((link->trace_first_tx_ms == UINT32_MAX) == (link->tx_bytes == 0));
    assert((link->trace_last_tx_ms == UINT32_MAX) == (link->tx_bytes == 0));
    assert((link->trace_first_rx_ms == UINT32_MAX) == (link->rx_bytes == 0));
    assert((link->trace_last_rx_ms == UINT32_MAX) == (link->rx_bytes == 0));
}
static void run(omni_mcu2_link *link, transport *t, uint32_t now)
{
    for (unsigned i = 0; i < 200; ++i) poll(link, t, now);
}
static void detect_setup(omni_mcu2_link *link, transport *t, unsigned bits)
{
    setup(link, t, 0);
    omni_mcu2_link_cancel(link);
    memset(t->rx, 0, sizeof(t->rx));
    t->rx[0] = 0xcb; t->rx[1] = 4; t->rx[2] = 0x87; t->rx[3] = (uint8_t)bits;
    assert(omni_mcu2_link_start_query(link, 0, OMNI_MCU2_QUERY_DETECT));
}
static void test_detect(void)
{
    omni_mcu2_link link; transport t;
    for (unsigned bits = 0; bits < 4; ++bits) {
        detect_setup(&link, &t, bits); run(&link, &t, 10);
        assert(link.phase == OMNI_MCU2_SUCCESS && link.reply[0] == bits);
        assert(link.reply[1] == 0 && link.reply[2] == 0);
        assert(t.sent == 1036 && t.tx[0] == 0xbc && t.tx[1] == 3 && t.tx[2] == 0x87);
        for (unsigned i = 3; i < 1036; ++i) assert(t.tx[i] == 0);
    }
    /* Header, detect range and ALL padding bytes must be validated, including
     * bytes4..6 retained in the larger legacy version-response header. */
    const unsigned corrupt[] = {0, 1, 2, 3, 4, 5, 6, 7, 1035};
    for (unsigned i = 0; i < sizeof(corrupt)/sizeof(corrupt[0]); ++i) {
        detect_setup(&link, &t, 0); t.rx[corrupt[i]] ^= 4;
        run(&link, &t, 10);
        assert(link.phase == OMNI_MCU2_RUNNING && link.rejected_frames == 1);
        poll(&link, &t, 3000); assert(link.phase == OMNI_MCU2_TIMEOUT);
    }
    detect_setup(&link, &t, 2); t.available = 1035;
    run(&link, &t, 10); poll(&link, &t, 3000);
    assert(link.phase == OMNI_MCU2_TIMEOUT && link.reply[0] == 0);
    detect_setup(&link, &t, 2); t.early_rx = true;
    run(&link, &t, 10); assert(link.phase == OMNI_MCU2_RUNNING && link.rejected_frames == 1);
    omni_mcu2_link_cancel(&link);
    uint32_t attempts = link.attempts;
    assert(!omni_mcu2_link_start_query(&link, 20, (omni_mcu2_query)2));
    assert(!omni_mcu2_link_start_query(&link, 20, (omni_mcu2_query)-1));
    assert(link.attempts == attempts && link.query == OMNI_MCU2_QUERY_DETECT);
}
int main(void)
{
    omni_mcu2_link link; transport t;
    mcu2_link_io invalid = {0};
    assert(!omni_mcu2_link_init(NULL, invalid));
    assert(!omni_mcu2_link_init(&link, invalid));
    memset(&link, 0, sizeof(link)); assert(!omni_mcu2_link_start(&link, 0));
    setup(&link, &t, 0); run(&link, &t, 0);
    assert(link.phase == OMNI_MCU2_SUCCESS && t.sent == 1036);
    const uint8_t query[] = {0xbc,4,0xe1,2};
    assert(!memcmp(t.tx, query, 4));
    for (unsigned i = 4; i < 1036; ++i) assert(t.tx[i] == 0);
    assert(link.reply[0] == 1 && link.reply[1] == 0x32 && link.reply[2] == 0);
    assert(link.received_frames == 1 && link.rejected_frames == 0);
    poll(&link, &t, 300); assert(!t.tx_calls && !t.rx_calls);

    /* Reject every altered identifying header byte and nonzero padding. */
    for (unsigned position = 0; position < 5; ++position) {
        setup(&link, &t, 0); t.rx[position == 4 ? 1035 : position] ^= 1;
        run(&link, &t, 0); assert(link.phase == OMNI_MCU2_RUNNING);
        assert(link.rejected_frames == 1 && link.reply[0] == 0);
        poll(&link, &t, OMNI_MCU2_QUERY_TIMEOUT_MS); assert(link.phase == OMNI_MCU2_TIMEOUT);
    }
    /* Unrelated fixed packet followed by exact reply preserves boundaries. */
    setup(&link, &t, 0); memcpy(t.rx + 1036, t.rx, 1036);
    t.rx[2] = 0xaa; t.available = 2072; run(&link, &t, 0);
    assert(link.phase == OMNI_MCU2_SUCCESS && link.rejected_frames == 1);
    /* Missing final padding byte is never success; deadline wraps safely. */
    setup(&link, &t, 0xfffffff0U); t.available = 1035;
    run(&link, &t, 0); assert(link.phase == OMNI_MCU2_RUNNING);
    poll(&link, &t, 0xfffffff0U + OMNI_MCU2_QUERY_TIMEOUT_MS - 1U);
    assert(link.phase == OMNI_MCU2_RUNNING);
    poll(&link, &t, 0xfffffff0U + OMNI_MCU2_QUERY_TIMEOUT_MS);
    assert(link.phase == OMNI_MCU2_TIMEOUT);
    setup(&link, &t, 0); t.available = 0; run(&link, &t, 0);
    poll(&link, &t, OMNI_MCU2_QUERY_TIMEOUT_MS); assert(link.phase == OMNI_MCU2_TIMEOUT);
    /* Byte backpressure does not skip TX data or repeat the query. */
    setup(&link, &t, 0); t.tx_full = true;
    poll(&link, &t, 1); assert(!link.tx_bytes);
    t.tx_full = false; run(&link, &t, 2); assert(link.phase == OMNI_MCU2_SUCCESS);
    setup(&link, &t, 0); t.tx_full = true;
    poll(&link, &t, OMNI_MCU2_QUERY_TIMEOUT_MS); assert(link.phase == OMNI_MCU2_TIMEOUT && !t.sent);
    /* A complete reply already waiting before TX completes cannot succeed. */
    setup(&link, &t, 0); t.early_rx = true; run(&link, &t, 0);
    assert(link.phase == OMNI_MCU2_RUNNING && link.rejected_frames == 1);
    /* I/O failure immediately stops traffic and leaves reply invalid. */
    setup(&link, &t, 0); t.tx_error = -1; poll(&link, &t, 0);
    assert(link.phase == OMNI_MCU2_IO_ERROR && !link.tx_bytes);
    setup(&link, &t, 0); t.rx_error = -1; poll(&link, &t, 0);
    assert(link.phase == OMNI_MCU2_IO_ERROR);
    setup(&link, &t, 0); poll(&link, &t, 0); omni_mcu2_link_cancel(&link);
    assert(link.phase == OMNI_MCU2_CANCELLED);
    poll(&link, &t, 0); assert(!t.tx_calls && !t.rx_calls);
    /* Reuse requires caller to resynchronize transport, modeled by reset. */
    memset(&t, 0, sizeof(t)); assert(omni_mcu2_link_start(&link, 10));
    assert(link.attempts == 2 && !link.tx_bytes && !link.rx_bytes);
    assert(link.trace_first_tx_ms == UINT32_MAX && link.trace_last_tx_ms == UINT32_MAX);
    assert(link.trace_first_rx_ms == UINT32_MAX && link.trace_last_rx_ms == UINT32_MAX);
    assert(link.trace_rx_used == 0 && link.trace_rx_truncated == 0);
    for (unsigned i = 0; i < OMNI_MCU2_TRACE_RX_BYTES; ++i) assert(link.trace_rx[i] == 0);

    /* A reply arriving well after the former 250 ms deadline still succeeds.
     * TX acceptance, first RX and last RX are distinct elapsed observations. */
    setup(&link, &t, 1000); t.available = 0;
    for (unsigned i = 0; i < 65; ++i) poll(&link, &t, 1010U + i);
    assert(link.tx_bytes == 1036 && link.trace_first_tx_ms == 10 && link.trace_last_tx_ms == 74);
    poll(&link, &t, 1500); assert(link.phase == OMNI_MCU2_RUNNING);
    t.available = 32; poll(&link, &t, 1750);
    assert(link.trace_first_rx_ms == 750 && link.trace_last_rx_ms == 750);
    t.available = 64; poll(&link, &t, 1760);
    assert(link.trace_first_rx_ms == 750 && link.trace_last_rx_ms == 760);
    t.available = 1036; run(&link, &t, 1800);
    assert(link.phase == OMNI_MCU2_SUCCESS && link.trace_last_rx_ms == 800);
    assert(link.trace_rx_used == 64 && link.trace_rx_truncated == 972);
    poll(&link, &t, 5000);
    assert(!t.tx_calls && !t.rx_calls && link.trace_last_rx_ms == 800);

    /* Entirely silent RX times out once without a synthetic zero timestamp. */
    setup(&link, &t, 1000); t.available = 0; run(&link, &t, 1010);
    poll(&link, &t, 3999); assert(link.phase == OMNI_MCU2_RUNNING);
    poll(&link, &t, 4000); assert(link.phase == OMNI_MCU2_TIMEOUT);
    assert(link.attempts == 1 && t.sent == 1036 && link.rx_bytes == 0);
    assert(link.trace_first_tx_ms == 10 && link.trace_last_tx_ms == 10);
    assert(link.trace_first_rx_ms == UINT32_MAX && link.trace_last_rx_ms == UINT32_MAX);

    /* Record raw malformed prefix bytes, not just accepted protocol frames. */
    setup(&link, &t, 0);
    for (unsigned i = 0; i < 1036; ++i) t.rx[i] = (uint8_t)(i ^ 0x5aU);
    run(&link, &t, 12);
    assert(link.phase == OMNI_MCU2_RUNNING && link.rejected_frames == 1);
    assert(link.trace_rx_used == 64 && link.trace_rx_truncated == 972);
    assert(!memcmp(link.trace_rx, t.rx, 64));
    poll(&link, &t, 3000); assert(link.phase == OMNI_MCU2_TIMEOUT);
    assert(link.trace_first_rx_ms == 12 && link.trace_last_rx_ms == 12);

    /* A partial TX can expire; no automatic second query or extra TX occurs. */
    setup(&link, &t, 0); t.available = 0; poll(&link, &t, 5);
    assert(link.tx_bytes == 16); t.tx_full = true;
    poll(&link, &t, 2999); assert(link.phase == OMNI_MCU2_RUNNING);
    poll(&link, &t, 3000); assert(link.phase == OMNI_MCU2_TIMEOUT);
    assert(link.attempts == 1 && t.sent == 16);
    assert(link.trace_first_tx_ms == 5 && link.trace_last_tx_ms == 5);
    t.tx_full = false; poll(&link, &t, 3001);
    assert(!t.tx_calls && !t.rx_calls && t.sent == 16);

    /* Timestamp subtraction wraps across UINT32_MAX without looking unset. */
    setup(&link, &t, 0xfffffff0U); t.available = 0;
    poll(&link, &t, 0xfffffff5U); run(&link, &t, 0x10U);
    assert(link.trace_first_tx_ms == 5 && link.trace_last_tx_ms == 32);
    t.available = 32; poll(&link, &t, 0x200U);
    t.available = 1036; run(&link, &t, 0x220U);
    assert(link.phase == OMNI_MCU2_SUCCESS);
    assert(link.trace_first_rx_ms == 528 && link.trace_last_rx_ms == 560);

    /* Error and cancellation preserve preceding observations and raw bytes. */
    setup(&link, &t, 0); t.available = 16; run(&link, &t, 25);
    t.rx_error = -1; poll(&link, &t, 30);
    assert(link.phase == OMNI_MCU2_IO_ERROR && link.trace_rx_used == 16);
    assert(link.trace_first_rx_ms == 25 && link.trace_last_rx_ms == 25);
    poll(&link, &t, 40); assert(!t.tx_calls && !t.rx_calls);
    setup(&link, &t, 0); t.available = 16; run(&link, &t, 25);
    omni_mcu2_link_cancel(&link);
    assert(link.phase == OMNI_MCU2_CANCELLED && link.trace_rx_used == 16);
    assert(link.trace_first_rx_ms == 25 && link.trace_last_rx_ms == 25);
    test_detect();
    puts("MCU2 version/detect transaction contracts passed");
    return 0;
}
