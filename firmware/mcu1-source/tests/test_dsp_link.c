#include "dsp_link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t tx[4], rx[256];
    unsigned sent, received, available, tx_calls, rx_calls;
    int tx_error, rx_error;
    bool full;
} transport;
static int tx(void *context, uint8_t byte)
{
    transport *t = context; ++t->tx_calls;
    if (t->tx_error) return t->tx_error;
    if (t->full) return 0;
    assert(t->sent < 4); t->tx[t->sent++] = byte; return 1;
}
static int rx(void *context, uint8_t *byte)
{
    transport *t = context; ++t->rx_calls;
    if (t->rx_error) return t->rx_error;
    if (t->received == t->available) return 0;
    *byte = t->rx[t->received++]; return 1;
}
static void setup(omni_dsp_link *link, transport *t, uint32_t now)
{
    /* First eight bytes are observed hardware evidence. The final five
     * deliberately distinct bytes are synthetic, testing opaque retention. */
    const uint8_t response[] = {0xdb,13,0xe1,3,0,0x36,0,0,0x11,0x22,0x33,0x44,0x55};
    memset(t, 0, sizeof(*t)); memcpy(t->rx, response, sizeof(response));
    t->available = (unsigned)sizeof(response);
    mcu2_link_io io = {t, tx, rx};
    assert(omni_dsp_link_init(link, io));
    assert(omni_dsp_link_start(link, now));
    assert(!omni_dsp_link_start(link, now));
}
static void poll(omni_dsp_link *link, transport *t, uint32_t now)
{
    t->tx_calls = t->rx_calls = 0; omni_dsp_link_poll(link, now);
    assert(t->tx_calls <= 16 && t->rx_calls <= 32);
}
int main(void)
{
    omni_dsp_link link; transport t;
    mcu2_link_io invalid = {0};
    assert(!omni_dsp_link_init(NULL, invalid));
    assert(!omni_dsp_link_init(&link, invalid));
    memset(&link, 0, sizeof(link)); assert(!omni_dsp_link_start(&link, 0));
    setup(&link, &t, 0); poll(&link, &t, 0);
    assert(link.phase == OMNI_DSP_SUCCESS);
    const uint8_t query[] = {0xbd,4,0xe1,2}; assert(!memcmp(t.tx, query, 4));
    assert(link.reply[0] == 0 && link.reply[1] == 0x36 && link.reply[2] == 0);
    assert(link.reply_length == 13 && !memcmp(link.reply_tail, t.rx + 7, 6));
    poll(&link, &t, 1); assert(!t.tx_calls && !t.rx_calls);
    /* Fragmentation is accepted, including split opcode and final byte. */
    setup(&link, &t, 0); t.available = 1;
    for (unsigned n = 1; n <= 13; ++n) {
        t.available = n; poll(&link, &t, n);
        assert(link.phase == (n == 13 ? OMNI_DSP_SUCCESS : OMNI_DSP_RUNNING));
    }
    /* Wrong opcode, wrong subcommand and short length cannot pass. */
    for (unsigned n = 0; n < 3; ++n) {
        setup(&link, &t, 0);
        if (n == 0) t.rx[2] = 0xe2;
        if (n == 1) t.rx[3] = 2;
        if (n == 2) { t.rx[1] = 6; t.available = 6; }
        poll(&link, &t, 0); assert(link.phase == OMNI_DSP_RUNNING);
        assert(link.rejected_frames == 1 && link.first_unexpected_length == t.available);
        unsigned retained = t.available < sizeof(link.first_unexpected) ? t.available : (unsigned)sizeof(link.first_unexpected);
        assert(!memcmp(link.first_unexpected, t.rx, retained));
        poll(&link, &t, 250); assert(link.phase == OMNI_DSP_TIMEOUT);
    }
    /* Stock reads through byte12. Seven bytes are not a complete status;
     * even a valid declared DB length must satisfy this command's shape. */
    const unsigned wrong_lengths[] = {7, 8, 12, 14};
    for (unsigned n = 0; n < sizeof(wrong_lengths)/sizeof(wrong_lengths[0]); ++n) {
        setup(&link, &t, 0); t.rx[1] = (uint8_t)wrong_lengths[n];
        t.available = wrong_lengths[n]; poll(&link, &t, 0);
        assert(link.phase == OMNI_DSP_RUNNING && link.rejected_frames == 1);
        assert(link.reply_length == 0 && link.first_unexpected_length == wrong_lengths[n]);
        poll(&link, &t, 250); assert(link.phase == OMNI_DSP_TIMEOUT);
    }
    /* DD and unrelated complete packets do not preclude the later response. */
    setup(&link, &t, 0); uint8_t response[13]; memcpy(response, t.rx, sizeof(response));
    const uint8_t preceding[] = {0xdd,1,2,3, 0xdb,4,0xe4,3, 5,0x5b,0,0};
    memcpy(t.rx, preceding, sizeof(preceding)); memcpy(t.rx+sizeof(preceding), response, sizeof(response));
    t.available = (unsigned)(sizeof(preceding)+sizeof(response)); poll(&link, &t, 0);
    assert(link.phase == OMNI_DSP_SUCCESS && link.parser.ignored == 1 && link.rejected_frames == 2);
    assert(link.first_unexpected_length == 4 && link.first_unexpected[2] == 0xe4);
    /* Truncation and inter-byte timeout drop the partial frame. */
    setup(&link, &t, 0); t.available = 12; poll(&link, &t, 0);
    poll(&link, &t, 20); assert(link.parser.expired == 1);
    t.available = 13; poll(&link, &t, 21); assert(link.phase == OMNI_DSP_RUNNING);
    poll(&link, &t, 250); assert(link.phase == OMNI_DSP_TIMEOUT);
    setup(&link, &t, 0xfffffff0U); t.available = 0;
    poll(&link, &t, 233); assert(link.phase == OMNI_DSP_RUNNING);
    poll(&link, &t, 234); assert(link.phase == OMNI_DSP_TIMEOUT);
    /* Bounded noise traffic and malformed lengths; no unbounded draining. */
    setup(&link, &t, 0); memset(t.rx, 0xff, sizeof(t.rx)); t.available = 256;
    poll(&link, &t, 0); assert(link.rx_bytes == 32 && link.phase == OMNI_DSP_RUNNING);
    setup(&link, &t, 0); t.rx[1] = 0; poll(&link, &t, 0);
    assert(link.parser.malformed == 1 && link.phase == OMNI_DSP_RUNNING);
    setup(&link, &t, 0); t.full = true; t.available = 0; poll(&link, &t, 0);
    assert(!link.tx_bytes); t.full = false; t.available = 13; poll(&link, &t, 1);
    assert(link.phase == OMNI_DSP_SUCCESS);
    assert(omni_dsp_link_start(&link, 2));
    assert(link.reply_length == 0);
    const uint8_t zeros[6] = {0}; assert(!memcmp(link.reply_tail, zeros, 6));
    setup(&link, &t, 0); t.full = true; poll(&link, &t, 0);
    assert(link.phase == OMNI_DSP_RUNNING && link.rejected_frames == 1);
    setup(&link, &t, 0); t.tx_error = -1; poll(&link, &t, 0);
    assert(link.phase == OMNI_DSP_IO_ERROR);
    setup(&link, &t, 0); t.rx_error = -1; poll(&link, &t, 0);
    assert(link.phase == OMNI_DSP_IO_ERROR);
    setup(&link, &t, 0); omni_dsp_link_cancel(&link); poll(&link, &t, 0);
    assert(link.phase == OMNI_DSP_CANCELLED && !t.tx_calls && !t.rx_calls);
    puts("DSP status transaction contracts passed");
    return 0;
}
