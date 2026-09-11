#include "eq_nvm.h"
#include "dsp_settings.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void)
{
    /* --- page format: build/parse round-trip --- */
    uint8_t payload[DSP_EQ_NVM_PAYLOAD], page[OMNI_EQ_NVM_PAGE], page2[OMNI_EQ_NVM_PAGE];
    for (unsigned i = 0; i < DSP_EQ_NVM_PAYLOAD; ++i) payload[i] = (uint8_t)(i * 7u + 1u);
    assert(omni_eq_nvm_build(page, 5u, payload, DSP_EQ_NVM_PAYLOAD) == OMNI_EQ_NVM_PAGE);
    uint32_t seq = 0; uint8_t out[OMNI_EQ_NVM_MAX_PAYLOAD]; size_t olen = 0;
    assert(omni_eq_nvm_parse(page, &seq, out, &olen));
    assert(seq == 5u && olen == DSP_EQ_NVM_PAYLOAD && !memcmp(out, payload, DSP_EQ_NVM_PAYLOAD));

    /* corrupt any byte -> CRC rejects */
    uint8_t bad[OMNI_EQ_NVM_PAGE]; memcpy(bad, page, OMNI_EQ_NVM_PAGE); bad[20] ^= 0xffu;
    assert(!omni_eq_nvm_parse(bad, 0, 0, 0));
    /* over-long payload rejected */
    assert(omni_eq_nvm_build(page, 1u, payload, OMNI_EQ_NVM_MAX_PAYLOAD + 1u) == 0);

    /* --- A/B slot selection: newest valid wins; blank => none --- */
    assert(omni_eq_nvm_build(page2, 9u, payload, DSP_EQ_NVM_PAYLOAD) == OMNI_EQ_NVM_PAGE);
    uint32_t ns = 0;
    assert(omni_eq_nvm_pick(page, page2, &ns) == 1 && ns == 10u);   /* page2 seq 9 is newer */
    assert(omni_eq_nvm_pick(page2, page, &ns) == 0 && ns == 10u);
    uint8_t blank[OMNI_EQ_NVM_PAGE]; memset(blank, 0xff, OMNI_EQ_NVM_PAGE);
    assert(omni_eq_nvm_pick(blank, blank, &ns) == -1 && ns == 1u);  /* neither valid */
    assert(omni_eq_nvm_pick(page, blank, &ns) == 0 && ns == 6u);    /* page seq 5 */

    /* --- dsp_settings EQ serialize/deserialize round-trip --- */
    uint8_t rec[DSP_EQ_NVM_PAYLOAD]; memset(rec, 0, sizeof(rec));
    rec[0] = 1u;              /* wireless present */
    rec[1] = 4u;              /* blob[0] = custom preset id (4) for wireless */
    for (unsigned i = 1; i < 128u; ++i) rec[1u + i] = (uint8_t)(i + 3u);
    /* mic (offset 129) and bt (offset 208) absent */
    assert(omni_dsp_settings_eq_generation() == 0u);
    omni_dsp_settings_eq_deserialize(rec, DSP_EQ_NVM_PAYLOAD, 1000u);
    uint8_t custom[128];
    assert(omni_dsp_settings_custom(12u, custom) == 128u && !memcmp(custom, rec + 1u, 128u));
    assert(omni_dsp_settings_custom(13u, custom) == 0u); /* mic absent */
    uint8_t round[DSP_EQ_NVM_PAYLOAD];
    assert(omni_dsp_settings_eq_serialize(round) == DSP_EQ_NVM_PAYLOAD);
    assert(round[0] == 1u && !memcmp(round + 1u, rec + 1u, 128u));
    assert(round[129] == 0u && round[208] == 0u); /* mic/bt still absent */
    assert(omni_dsp_settings_eq_generation() > 0u);

    puts("EQ NVM page format, A/B selection and dsp_settings round-trip pass");
    return 0;
}
