#include "interchip.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void packet(omni_link_parser *p, const uint8_t *bytes, size_t size,
                   uint32_t now, bool delivered)
{
    const uint8_t *frame; size_t length;
    for (size_t i = 0; i < size; ++i) {
        bool done = omni_link_feed(p, bytes[i], now, &frame, &length);
        assert(done == (delivered && i == size - 1U));
        if (done) { assert(length == size); assert(!memcmp(frame, bytes, size)); }
        else { assert(frame == NULL); assert(length == 0); }
    }
}
int main(void)
{
    omni_link_parser p;
    assert(!omni_link_init(NULL, 10));
    assert(!omni_link_init(&p, 0));
    assert(!omni_link_init(&p, 0x80000000U));
    assert(omni_link_init(&p, 20));
    uint8_t frame[204], output[204], payload[204];
    for (unsigned i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)i;
    /* Stock BD constants at linked 0x4e864 and0x4e86a. Exact encoding
     * does not imply their unverified startup/query semantics. */
    const uint8_t bd[] = {0xbd,6,0x51,1,1,0};
    assert(omni_link_encode_bd(output, sizeof(output), bd + 2, 4) == 6);
    assert(!memcmp(output, bd, sizeof(bd)));
    const uint8_t bd2[] = {0xbd,6,0x51,1,4,0};
    assert(omni_link_encode_bd(output, sizeof(output), bd2 + 2, 4) == 6);
    assert(!memcmp(output, bd2, sizeof(bd2)));

    /* Every accepted DB length, including embedded start markers. */
    for (unsigned n = 3; n <= OMNI_LINK_DB_MAX; ++n) {
        memcpy(frame, payload, n); frame[0] = 0xdb; frame[1] = (uint8_t)n;
        packet(&p, frame, n, 1, true);
    }
    for (unsigned start = 5; start <= 0x15; start += 0x10) {
        for (unsigned type = 0x5a; type <= 0x5d; ++type) {
            for (unsigned n = 0; n <= 200; ++n) {
                frame[0] = (uint8_t)start; frame[1] = (uint8_t)type;
                frame[2] = (uint8_t)n; frame[3] = 0;
                memcpy(frame + 4, payload, n);
                packet(&p, frame, n + 4U, 2, true);
                if (n <= 196) {
                    assert(omni_link_encode_race(output, sizeof(output),
                        (uint8_t)start, (uint8_t)type, payload, n) == n + 4U);
                    assert(!memcmp(frame, output, n + 4U));
                }
            }
        }
    }
    assert(p.frames == 198U + 2U * 4U * 201U);
    const uint8_t dd[] = {0xdd,0xdb,5,0x15}; packet(&p, dd, 4, 3, false);
    assert(p.ignored == 1);
    const uint8_t noise[] = {0,0xff,0xbd,0xbc,0xcb};
    packet(&p, noise, sizeof(noise), 4, false);
    assert(p.used == 0);
    const uint8_t invalid[][4] = {{0xdb,0,0,0},{0xdb,2,0,0},{0xdb,201,0,0},
        {5,0x59,0,0},{5,0x5e,0,0},{5,0x5a,201,0},{0x15,0x5c,0,1}};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        packet(&p, invalid[i], 4, 5, false); assert(p.used == 0);
    }
    assert(p.malformed == 7);
    /* Fragmentation, idle timeout, timestamp wrap, header resync. */
    const uint8_t *got; size_t size;
    assert(!omni_link_feed(&p, 0xdb, 0xfffffff8U, &got, &size));
    omni_link_expire(&p, 11); assert(p.used == 1);
    omni_link_expire(&p, 12); assert(!p.used && p.expired == 1);
    assert(!omni_link_feed(&p, 0xdb, 20, &got, &size));
    assert(!omni_link_feed(&p, 0xdb, 21, &got, &size)); /* invalid length + start */
    assert(!omni_link_feed(&p, 3, 22, &got, &size));
    assert(omni_link_feed(&p, 0x42, 23, &got, &size));
    assert(size == 3 && got[2] == 0x42 && p.malformed == 8);
    assert(!omni_link_feed(&p, 5, 30, &got, &size));
    assert(!omni_link_feed(&p, 0x5c, 31, &got, &size));
    assert(!omni_link_feed(&p, 0xdb, 51, &got, &size)); /* timeout, new header */
    assert(p.expired == 2 && p.used == 1 && p.bytes[0] == 0xdb);
    omni_link_expire(&p, 100);

    /* Invalid encode never modifies output, including length overflow. */
    memset(output, 0xaa, sizeof(output));
    assert(!omni_link_encode_bd(output, 1, payload, 1));
    assert(!omni_link_encode_bd(output, sizeof(output), payload, 199));
    assert(!omni_link_encode_bd(output, sizeof(output), NULL, 1));
    assert(!omni_link_encode_race(output, sizeof(output), 5, 0x5c, payload, SIZE_MAX));
    assert(!omni_link_encode_race(output, sizeof(output), 5, 0x5c, payload, 197));
    assert(!omni_link_encode_race(output, sizeof(output), 5, 0x59, payload, 1));
    assert(!omni_link_encode_race(output, sizeof(output), 0xdb, 0x5c, payload, 1));
    assert(!omni_link_encode_race(output, 3, 5, 0x5c, payload, 0));
    for (unsigned i = 0; i < sizeof(output); ++i) assert(output[i] == 0xaa);
    puts("DSP framing: exhaustive supported lengths/types, malformed frames, timeouts and stock TX bytes passed");
    return 0;
}
