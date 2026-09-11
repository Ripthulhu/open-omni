#include "interchip.h"
#include <string.h>

static bool marker(uint8_t byte)
{ return byte == 0xdbU || byte == 0xddU || byte == 5U || byte == 0x15U; }
static bool race_type(uint8_t byte)
{ return byte >= 0x5aU && byte <= 0x5dU; }
static void clear(omni_link_parser *p)
{ p->used = 0; p->expected = 0; }

bool omni_link_init(omni_link_parser *p, uint32_t gap_ms)
{
    if (!p || !gap_ms || gap_ms >= 0x80000000U) return false;
    memset(p, 0, sizeof(*p)); p->gap_ms = gap_ms; return true;
}
void omni_link_expire(omni_link_parser *p, uint32_t now_ms)
{
    if (p->used && (uint32_t)(now_ms - p->last_ms) >= p->gap_ms) {
        p->expired++; clear(p);
    }
}
static void begin(omni_link_parser *p, uint8_t byte, uint32_t now_ms)
{
    if (marker(byte)) {
        p->bytes[0] = byte; p->used = 1; p->last_ms = now_ms;
        if (byte == 0xddU) p->expected = 4;
    }
}
static void reject(omni_link_parser *p, uint8_t byte, uint32_t now_ms)
{
    p->malformed++; clear(p);
    /* A malformed header byte may itself begin a new frame. Never scan
     * inside a length-validated payload: markers there are ordinary data. */
    begin(p, byte, now_ms);
}
bool omni_link_feed(omni_link_parser *p, uint8_t byte, uint32_t now_ms,
                    const uint8_t **frame, size_t *length)
{
    *frame = NULL; *length = 0;
    omni_link_expire(p, now_ms);
    if (!p->used) { begin(p, byte, now_ms); return false; }
    p->last_ms = now_ms;
    /* expected is bounded before the payload is consumed. This additional
     * guard also contains accidental parser-state corruption. */
    if (p->used >= OMNI_LINK_RX_MAX) { reject(p, byte, now_ms); return false; }
    p->bytes[p->used++] = byte;
    uint8_t first = p->bytes[0];
    if (first == 0xdbU && p->used == 2U) {
        /* Dispatcher consumes byte 2; total lengths below 3 are malformed. */
        if (byte < 3U || byte > OMNI_LINK_DB_MAX) {
            reject(p, byte, now_ms); return false;
        }
        p->expected = byte;
    } else if (first == 5U || first == 0x15U) {
        if (p->used == 2U && !race_type(byte)) {
            reject(p, byte, now_ms); return false;
        }
        if (p->used == 4U) {
            uint16_t payload = (uint16_t)((uint16_t)p->bytes[2] |
                                         (uint16_t)((uint16_t)byte << 8));
            if (payload > OMNI_LINK_RX_MAX - 4U) {
                reject(p, byte, now_ms); return false;
            }
            p->expected = (uint16_t)(payload + 4U);
        }
    }
    if (!p->expected || p->used != p->expected) return false;
    size_t complete = p->used; clear(p);
    if (first == 0xddU) { p->ignored++; return false; }
    p->frames++; *frame = p->bytes; *length = complete; return true;
}
size_t omni_link_encode_bd(uint8_t *out, size_t capacity,
                          const uint8_t *payload, size_t length)
{
    if (!out || !payload || !length || length > OMNI_LINK_TX_MAX - 2U ||
        capacity < length + 2U) return 0;
    out[0] = 0xbdU; out[1] = (uint8_t)(length + 2U);
    memcpy(out + 2, payload, length); return length + 2U;
}
size_t omni_link_encode_race(uint8_t *out, size_t capacity, uint8_t start,
                            uint8_t type, const uint8_t *payload, size_t length)
{
    if (!out || (length && !payload) || (start != 5U && start != 0x15U) ||
        !race_type(type) || length > OMNI_LINK_TX_MAX - 4U ||
        capacity < length + 4U) return 0;
    out[0] = start; out[1] = type; out[2] = (uint8_t)length; out[3] = 0;
    if (length) memcpy(out + 4, payload, length);
    return length + 4U;
}
