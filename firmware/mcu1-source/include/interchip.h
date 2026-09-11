#ifndef OMNI_INTERCHIP_H
#define OMNI_INTERCHIP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MCU1 USART3 <-> DSP byte framing, recovered from stock at 0x23c60,
 * 0x2406c, 0x240c0 and 0x241bc. This module never touches hardware.
 * RX: DB/total-length/data, DD + three bytes (ignored), or
 * 05|15 / type(5a..5d) / payload-length LE16 / payload.
 * The low-level stock framing has no checksum. RACE payload stays opaque.
 * Strict rejection replaces stock's unsafe length clamping/truncation.
 */
#define OMNI_LINK_DB_MAX 200U
#define OMNI_LINK_RX_MAX 204U
#define OMNI_LINK_TX_MAX 200U
typedef struct {
    uint8_t bytes[OMNI_LINK_RX_MAX];
    uint16_t used, expected;
    uint32_t last_ms, gap_ms;
    uint32_t frames, ignored, malformed, expired;
} omni_link_parser;

/* gap_ms is application policy, not a recovered stock timeout; nonzero and
 * <2^31. Call expire during idle as well as feed. One owner, no ISR sharing.
 * Completed bytes remain valid until the next feed; copy before then.
 */
bool omni_link_init(omni_link_parser *, uint32_t gap_ms);
void omni_link_expire(omni_link_parser *, uint32_t now_ms);
bool omni_link_feed(omni_link_parser *, uint8_t byte, uint32_t now_ms,
                    const uint8_t **frame, size_t *length);

/* Encoders support the conservative 200-byte stock queued-TX envelope.
 * Failure returns zero and leaves out unchanged. Input/output must not
 * overlap. No implied command semantics, retries, or application ACKs.
 */
size_t omni_link_encode_bd(uint8_t *out, size_t capacity,
                          const uint8_t *payload, size_t length);
size_t omni_link_encode_race(uint8_t *out, size_t capacity, uint8_t start,
                            uint8_t type, const uint8_t *payload, size_t length);
#endif
