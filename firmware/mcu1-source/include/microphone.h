#ifndef OMNI_MICROPHONE_H
#define OMNI_MICROPHONE_H
#include <stdbool.h>
#include <stdint.h>
bool omni_microphone_start(uint32_t descriptor[4], uint32_t rate);
bool omni_microphone_stop(void);
void omni_microphone_irq(void);
unsigned omni_microphone_packet(uint8_t out[98]);
void omni_microphone_status(uint32_t out[15]);
#endif
