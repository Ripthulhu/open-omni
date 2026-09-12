#include "eq_nvm.h"
#include "dsp_settings.h"
#include "ui.h"
#include "audio_probe.h"
#include <string.h>

/* Glue: restore the persisted EQ into the cache at boot, and write it back to
 * the inactive A/B slot after the EQ changes and settles. Thin ARM-side wiring;
 * the page format (eq_nvm.c) and the EQ (de)serialisation (dsp_settings.c) are
 * host-tested. */

static uint32_t saved_gen, last_gen, next_seq = 1u, change_ms;
static bool have_saved, restored;
static unsigned active_slot;

void omni_eq_persist_restore(void)
{
    if (restored) return;
    restored = true;
    uint8_t a[OMNI_EQ_NVM_PAGE], b[OMNI_EQ_NVM_PAGE];
    if (!omni_eq_nvm_flash_read(0u, a)) memset(a, 0xff, OMNI_EQ_NVM_PAGE);
    if (!omni_eq_nvm_flash_read(1u, b)) memset(b, 0xff, OMNI_EQ_NVM_PAGE);
    int pick = omni_eq_nvm_pick(a, b, &next_seq);
    if (pick >= 0) {
        uint8_t payload[OMNI_EQ_NVM_MAX_PAYLOAD];
        size_t plen = 0;
        if (omni_eq_nvm_parse(pick ? b : a, 0, payload, &plen))
            omni_dsp_settings_eq_deserialize(payload, plen, omni_ui_milliseconds());
        active_slot = (unsigned)pick;
    }
    saved_gen = last_gen = omni_dsp_settings_eq_generation();
    have_saved = true;
}

void omni_eq_persist_poll(uint32_t now)
{
    uint32_t gen = omni_dsp_settings_eq_generation();
    if (!have_saved) { saved_gen = last_gen = gen; change_ms = now; have_saved = true; return; }
    if (gen != last_gen) { last_gen = gen; change_ms = now; }
    if (gen == saved_gen) return;
    /* Coalesce rapid edits: only write once the EQ has been stable ~2s. */
    if ((uint32_t)(now - change_ms) < 2000u) return;
    /* The flash erase/program runs with interrupts off (eq_nvm_lpc5528.c). On
     * Full-Speed USB that stall desyncs the isochronous playback ring and
     * corrupts audio until replug. Defer the write while playback is streaming;
     * saved_gen stays behind so poll() retries once the stream stops. */
    if (audio_probe_alternate(OMNI_PLAYBACK_AS_INTERFACE) != 0) return;
    uint8_t payload[DSP_EQ_NVM_PAYLOAD], page[OMNI_EQ_NVM_PAGE];
    size_t plen = omni_dsp_settings_eq_serialize(payload);
    if (!omni_eq_nvm_build(page, next_seq, payload, plen)) { saved_gen = gen; return; }
    unsigned target = active_slot ? 0u : 1u;   /* write the inactive slot (A/B) */
    if (omni_eq_nvm_flash_write(target, page)) {
        active_slot = target;
        ++next_seq;
        saved_gen = gen;
    }
}
