#ifndef OMNI_ROTARY_H
#define OMNI_ROTARY_H
#include <stdbool.h>
#include <stdint.h>
typedef struct { uint8_t previous, edges; int8_t accumulated; uint32_t invalid; } omni_rotary;
/* Select 2 or 4 edges only after physical detent characterization. */
bool omni_rotary_init(omni_rotary *, uint8_t phase, uint8_t edges);
/* +1 is sequence 0,1,3,2,0; physical clockwise remains a board-level choice. */
int omni_rotary_sample(omni_rotary *, uint8_t phase);
#endif
