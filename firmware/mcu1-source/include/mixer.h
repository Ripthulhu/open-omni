#ifndef OMNI_MIXER_H
#define OMNI_MIXER_H
#include <stdbool.h>
#include <stdint.h>
enum { OMNI_MIX_USB1, OMNI_MIX_USB2, OMNI_MIX_USB3, OMNI_MIX_LINE, OMNI_MIX_COUNT };
typedef struct { uint8_t level; bool linked, muted; } omni_mix_input;
typedef struct { omni_mix_input input[OMNI_MIX_COUNT]; uint32_t revision; } omni_mixer;
void omni_mixer_init(omni_mixer *);
bool omni_mixer_available(unsigned input);
bool omni_mixer_set(omni_mixer *,unsigned input,unsigned level,bool linked,bool muted);
/* Level is a normalized input fader. Linked faders multiply master percentage;
 * independent faders retain their level/mute regardless of master state. */
unsigned omni_mixer_effective(const omni_mixer *,unsigned input,unsigned master,bool muted);
#endif
