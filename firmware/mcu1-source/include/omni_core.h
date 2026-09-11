#ifndef OMNI_CORE_H
#define OMNI_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Callers must serialize access (one owner, or IRQ exclusion for shared state).
 * dB values use the signed UAC 1/256 dB unit. No Windows scalar assumptions.
 */
typedef struct {
    int16_t minimum, maximum, step, current;
    bool muted;
    uint8_t pending; /* bit0: volume, bit1: mute */
    uint32_t revision;
    /* Cached staged 0..56 hardware-volume index (0=loud, 56=silent), refreshed
     * from omni_volume_attenuation() at each serialized mutation (init/set).
     * A provisional UI index, NOT a calibrated gain law; no wire command is
     * emitted and no consumer reads it yet (staged for a future routing step). */
    uint8_t attenuation;
} omni_volume;
bool omni_volume_init(omni_volume *, int16_t minimum, int16_t maximum, int16_t step, int16_t initial);
bool omni_volume_set(omni_volume *, int16_t db, bool local);
void omni_volume_dial(omni_volume *, int steps);
/* Pure: derive the staged 0..56 attenuation index from the current level --
 * 0 at maximum (loudest), 56 at minimum (silent), linear in the dB domain.
 * The real stock dB<->0..56 quantization curve is not yet recovered, so this is
 * a provisional staged index only. Returns 0 for NULL or a degenerate range. */
uint8_t omni_volume_attenuation(const omni_volume *);
void omni_mute_set(omni_volume *, bool mute, bool local);
/* Notification is only consumed after successful USB completion. */
size_t omni_volume_notification(const omni_volume *, uint8_t out[6]);
void omni_volume_notification_complete(omni_volume *, uint8_t selector, uint32_t sent_revision);

typedef struct { uint8_t type, request; uint16_t value, index, length; } omni_setup;
typedef struct { uint32_t playback_rate; bool playback_valid; } omni_uac2_clocks;
/* Playback AC0/clock10 is programmable 48/96k; microphone AC2/clock11 fixed 48k.
 * Caller commits a changed clock selection only after quiescing old USB banks. */
int omni_uac2_control_format(omni_volume *, omni_uac2_clocks *, omni_setup,
                            const uint8_t *payload, size_t payload_length,
                            uint8_t *response, size_t capacity);
/* Returns response length, 0 for accepted OUT, -1 for STALL. Caller provides
 * actual OUT bytes and response capacity. Entity 5=playback FU, 10=clock, IF0.
 */
int omni_uac2_control(omni_volume *, omni_setup, const uint8_t *payload,
                     size_t payload_length, uint8_t *response, size_t capacity);
#endif
