#ifndef OMNI_SOURCE_MIX_H
#define OMNI_SOURCE_MIX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OMNI_SOURCE_MIX_CENTER 12u
#define OMNI_SOURCE_MIX_MAX_POSITION 24u
#define OMNI_SOURCE_MIX_UNITY 16384u
#define OMNI_SOURCE_MIX_RAMP_STEP 32u
#define OMNI_SOURCE_MIX_MAX_FRAMES 1024u

/* Main/UI owner only. Desired settings do not imply a remote ACK/readback. */
typedef struct {
    uint32_t revision;
    uint8_t position;               /*0 USB1 only,12 both unity,24 USB2 only. */
    bool bias_mode;
} omni_source_mix;
typedef struct {
    uint8_t index[2];               /* Original MCU coefficients/index0..12. */
    uint16_t q14[2];                /* index12 canonicalized to exact unity16384. */
} omni_source_mix_gains;

void omni_source_mix_init(omni_source_mix *);
bool omni_source_mix_configure(omni_source_mix *,unsigned position);
void omni_source_mix_toggle(omni_source_mix *);
/* False only outside bias mode/invalid state. True consumes even a clamped
 * boundary/no-op; caller must not fall through into master volume then. */
bool omni_source_mix_dial(omni_source_mix *,int steps);
bool omni_source_mix_snapshot(const omni_source_mix *,omni_source_mix_gains *);

/* A distinct instance per audio source, owned only by its DMA ISR. Main
 * publishes a target via an aligned32-bit mailbox; ISR snapshots it once.
 * Do not share/mutate this ramp with UI/main. Reset only when DMA is quiesced. */
typedef struct {uint16_t current_q14;} omni_source_mix_ramp;
bool omni_source_mix_ramp_init(omni_source_mix_ramp *,unsigned initial_q14);
/* In-place signed16 samples in the upper16 bits of interleaved stereo words.
 * Caller owns exactly2*frames words in a block released by DMA. One shared
 * coefficient per L/R pair; bounded +/-32/frame (~10.67ms full range at48k).
 * Unity preserves all32bits. Attenuation discards lower16 padding, rounds
 * symmetrically to nearest, and never clips a signed16 input. Target0 ramps
 * to exact digital silence; this does not change global master mute state.
 * Invalid arguments/oversized blocks return false without any mutation.
 * frames0 is a valid no-op, including wordsNULL. */
bool omni_source_mix_apply(omni_source_mix_ramp *,unsigned target_q14,
                           uint32_t *words,size_t frames);
/* Format-aware path: sample_bits16/24 in the upper16/24 bits of each I2S word;
 * frames_per_ms48/96. PCM24 attenuation preserves its low8 meaningful bits,
 * uses a signed64-bit product and symmetric rounding, and discards only the
 * low8 padding. Unity preserves all32 bits. Ramp step32@48 /16@96 gives the
 * same512/1024-frame (~10.67ms) full-range duration. Invalid format is rejected
 * before any sample/ramp mutation. Legacy apply is exactly16/48. */
bool omni_source_mix_apply_format(omni_source_mix_ramp *,unsigned target_q14,
                                  uint32_t *words,size_t frames,
                                  unsigned sample_bits,unsigned frames_per_ms);
#endif
