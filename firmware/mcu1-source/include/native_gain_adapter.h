#ifndef OMNI_NATIVE_GAIN_ADAPTER_H
#define OMNI_NATIVE_GAIN_ADAPTER_H
#include <stdbool.h>
#include <stdint.h>
typedef void (*omni_native_rx_observer)(uint8_t byte,uint32_t now);
void omni_native_gain_observer(omni_native_rx_observer);
bool omni_native_gain_release(uint32_t now);
void omni_native_gain_service(uint32_t now,bool online);
/* Gain page2 words 11..14: mode-repair flags(used1,pending2,blocked4),
 * total repair attempts, verified repairs, last failed GET43 mode.
 * Used/block reset only at physical acquisition; totals remain diagnostic. */
bool omni_native_gain_read(unsigned page,uint32_t out[15]);
bool omni_native_headset_gain_read(unsigned page,uint32_t out[15]);
/* Main-loop only: queue explicit mode plus opaque captured context 0/1/2.
 * No UART bytes are sent by request/read; use snapshots for IRQ readers. */
bool omni_native_menu_request(uint32_t token,bool open,unsigned context,uint32_t now);
bool omni_native_menu_read(unsigned page,uint32_t out[15]);
#endif
