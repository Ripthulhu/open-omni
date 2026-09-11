#ifndef OMNI_DSP_CAPTURE_PROBE_H
#define OMNI_DSP_CAPTURE_PROBE_H
#include <stdbool.h>
#include <stdint.h>

/* Explicit passive UART3 observation while USB audio interfaces are idle.
 * Main cooperatively finishes native gain, then capture sends no DSP payload.
 * Same-token submissions are idempotent; a new token may replace a frozen
 * capture. Only accepted rearm invalidates old evidence; there are no retries.
 * Main owns transport and parser; HID only enqueues and reads bounded data. */
bool omni_dsp_capture_probe_busy(void);
bool omni_dsp_capture_probe_request(uint32_t token, uint32_t duration_ms);
void omni_dsp_capture_probe_poll(uint32_t now_ms, bool allowed);
bool omni_dsp_capture_probe_status(unsigned page, uint8_t out[60]);
bool omni_dsp_capture_probe_record(uint32_t token, unsigned index, unsigned page,
                                   uint8_t out[60]);
#endif
