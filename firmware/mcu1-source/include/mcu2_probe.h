#ifndef OMNI_MCU2_PROBE_H
#define OMNI_MCU2_PROBE_H
#include <stdbool.h>
#include <stdint.h>
bool omni_mcu2_probe_request(uint32_t token);
/* Profile0 version, profile1 detect. Same token must retain its profile. */
bool omni_mcu2_probe_request_query(uint32_t token, uint8_t profile);
bool omni_mcu2_probe_busy(void);
void omni_mcu2_probe_poll(uint32_t now_ms, bool allowed);
/* Persistent UART7 service. Call once each loop before USB configuration.
 * allowed is false only for recovery/shutdown, independent of UART3/audio. */
void omni_mcu2_service(uint32_t now_ms,bool allowed);
bool omni_mcu2_forward_lifecycle(uint8_t state);
/* Exact forwarding of an observed DSP86 byte4; not USB configuration. */
bool omni_mcu2_forward_dsp_state86(uint8_t state);
bool omni_mcu2_forward_controls(uint8_t volume_percent,uint8_t balance,
    uint8_t mic_state,uint8_t mic_percent,uint8_t setting_percent);
bool omni_mcu2_select_input(uint8_t side);
/* Queues secondary USB source bias0..12; runtime page2 reports transmission,
 * never peer acknowledgement or readback (AA has neither recovered). */
bool omni_mcu2_set_source_gain(uint8_t index);
bool omni_mcu2_runtime_read(unsigned page,uint32_t out[15]);
void omni_mcu2_probe_status(uint8_t out[60]);
/* HID56: page0 timing/counters; pages1/2 contain the first 64 raw RX bytes. */
bool omni_mcu2_probe_trace(uint8_t page,uint32_t out[15]);
#endif
