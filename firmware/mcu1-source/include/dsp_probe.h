#ifndef OMNI_DSP_PROBE_H
#define OMNI_DSP_PROBE_H
#include <stdbool.h>
#include <stdint.h>
bool omni_dsp_probe_request(uint32_t token);
bool omni_dsp_probe_busy(void);
void omni_dsp_probe_poll(uint32_t now_ms, bool allowed);
void omni_dsp_probe_status(uint8_t out[60]);
void omni_dsp_probe_trace(uint8_t out[60]);
void omni_dsp_probe_reply(uint8_t out[60]);
#endif
