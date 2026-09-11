#include "eq_nvm.h"
#include <string.h>

/* Layout: [0..3] magic, [4..7] seq (LE u32), [8..9] payload len (LE u16),
 * [10..11] reserved 0, [12..12+len) payload, [508..511] CRC32 over [0..507]. */
#define OMNI_EQ_NVM_MAGIC 0x51455131u /* "1QEQ" */

uint32_t omni_eq_nvm_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8u; ++b)
            crc = (crc >> 1) ^ (0xedb88320u & ((crc & 1u) ? 0xffffffffu : 0u));
    }
    return ~crc;
}

size_t omni_eq_nvm_build(uint8_t page[OMNI_EQ_NVM_PAGE], uint32_t seq,
                         const uint8_t *payload, size_t payload_len)
{
    if (!page || (payload_len && !payload) || payload_len > OMNI_EQ_NVM_MAX_PAYLOAD)
        return 0;
    memset(page, 0xff, OMNI_EQ_NVM_PAGE);
    uint32_t magic = OMNI_EQ_NVM_MAGIC;
    memcpy(page, &magic, 4);
    memcpy(page + 4, &seq, 4);
    uint16_t len = (uint16_t)payload_len;
    memcpy(page + 8, &len, 2);
    page[10] = 0; page[11] = 0;
    if (payload_len) memcpy(page + 12, payload, payload_len);
    uint32_t crc = omni_eq_nvm_crc32(page, OMNI_EQ_NVM_PAGE - 4u);
    memcpy(page + OMNI_EQ_NVM_PAGE - 4u, &crc, 4);
    return OMNI_EQ_NVM_PAGE;
}

bool omni_eq_nvm_parse(const uint8_t page[OMNI_EQ_NVM_PAGE], uint32_t *seq,
                       uint8_t *payload_out, size_t *payload_len)
{
    if (!page) return false;
    uint32_t magic;
    memcpy(&magic, page, 4);
    if (magic != OMNI_EQ_NVM_MAGIC) return false;
    uint32_t stored, computed = omni_eq_nvm_crc32(page, OMNI_EQ_NVM_PAGE - 4u);
    memcpy(&stored, page + OMNI_EQ_NVM_PAGE - 4u, 4);
    if (stored != computed) return false;
    uint16_t len;
    memcpy(&len, page + 8, 2);
    if (len > OMNI_EQ_NVM_MAX_PAYLOAD) return false;
    if (seq) memcpy(seq, page + 4, 4);
    if (payload_out) memcpy(payload_out, page + 12, len);
    if (payload_len) *payload_len = len;
    return true;
}

int omni_eq_nvm_pick(const uint8_t a[OMNI_EQ_NVM_PAGE], const uint8_t b[OMNI_EQ_NVM_PAGE],
                     uint32_t *next_seq)
{
    uint32_t sa = 0, sb = 0;
    bool va = omni_eq_nvm_parse(a, &sa, 0, 0);
    bool vb = omni_eq_nvm_parse(b, &sb, 0, 0);
    int pick = -1;
    uint32_t maxseq = 0;
    if (va) { pick = 0; maxseq = sa; }
    if (vb && (!va || sb > sa)) { pick = 1; maxseq = sb; }
    if (next_seq) *next_seq = (va || vb) ? maxseq + 1u : 1u;
    return pick;
}
