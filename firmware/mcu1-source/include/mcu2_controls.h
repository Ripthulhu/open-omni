#ifndef OMNI_MCU2_CONTROLS_H
#define OMNI_MCU2_CONTROLS_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
enum {
    OMNI_PEER_HAVE_VOLUME=1, OMNI_PEER_HAVE_BALANCE=2,
    OMNI_PEER_HAVE_MIC_STATE=4, OMNI_PEER_HAVE_MIC_LEVEL=8,
    OMNI_PEER_HAVE_LEGACY_D4=16, OMNI_PEER_HAVE_ALL=31
};
typedef struct {
    uint32_t known,generation,mic_generation,last_mic_ms,frames,rejected;
    uint8_t volume_percent,balance,mic_state,mic_level,legacy_d4,loudness_step;
} omni_mcu2_controls;
void omni_mcu2_controls_init(omni_mcu2_controls *);
/* Already decoded complete DSP DB frames, no UART reads or transmission. */
bool omni_mcu2_controls_observe(omni_mcu2_controls *,const uint8_t *,size_t,uint32_t now);
/* Verified DSP loudness step0..56, not Windows percentage or dB. Balance is
 * the serialized native mix percentage. Both are explicit owner inputs. */
bool omni_mcu2_controls_native(omni_mcu2_controls *,uint8_t loudness_step,uint8_t balance);
/* No defaults are invented: fails until every EF field has a known source.
 * Raw mic-state0=live/unmuted,1=muted OR physically retracted; those two causes
 * cannot be distinguished by this report. Last field uses legacy D4 only. */
bool omni_mcu2_controls_snapshot(const omni_mcu2_controls *,uint8_t out[5]);
void omni_mcu2_controls_status(const omni_mcu2_controls *,uint32_t out[15]);
#endif
