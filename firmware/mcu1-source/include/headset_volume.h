#ifndef OMNI_HEADSET_VOLUME_H
#define OMNI_HEADSET_VOLUME_H
#include <stdbool.h>
#include <stdint.h>
/* Original DSP E085/E011: LL1..56 is -55..0 nominal dB; LL0 is
 * the finite -120dB endpoint used for mute, not a hard-mute claim. */
bool omni_headset_volume_step(int16_t db,bool muted,uint8_t *step);
/* An accepted physical absolute zero selects USB minimum + mute. Outbound
 * Windows mute retains the master value; its echo is not a physical event. */
bool omni_headset_volume_db(uint8_t step,int16_t remembered_db,
                            int16_t *db,uint8_t *muted);
#endif
