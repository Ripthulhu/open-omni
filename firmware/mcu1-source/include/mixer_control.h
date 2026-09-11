#ifndef OMNI_MIXER_CONTROL_H
#define OMNI_MIXER_CONTROL_H
#include <stdbool.h>
#include <stdint.h>
/* The shared backend moved on before its terminal result was observed. This
 * is never reported as an ACK and never retries an unchanged desired mode. */
#define OMNI_MIXER_CONTROL_RESULT_REPLACED 0x100u
#define OMNI_MIXER_CONTROL_WAIT_MS 3000u
/* Single main-loop bridge from desired source bias to PCM/MCU2/D209 owners. */
void omni_mixer_control_service(uint32_t now,bool allowed);
/* Single IRQ producer publishes one complete desired tuple; main applies it. */
bool omni_mixer_control_request(unsigned mode,unsigned position);
/* Main-only snapshot: native USB publishes this in an inactive bank. */
void omni_mixer_control_status(uint32_t out[15]);
#endif
