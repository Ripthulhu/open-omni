#ifndef OMNI_UI_H
#define OMNI_UI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
void omni_ui_clock_start(void);
uint32_t omni_ui_milliseconds(void);
void omni_ui_poll(void);
void omni_ui_status(uint8_t out[60]);
void omni_ui_buttons_status(unsigned page,uint32_t out[15]);
/* Source-owned settings; stock 83/85/88/89 meanings, no DSP/flash writes. */
bool omni_ui_settings_set(unsigned timeout_index,unsigned brightness,
                          unsigned saver_mode,unsigned home_view);
void omni_ui_settings_status(uint32_t out[15]);
/* Main-loop-only complete frames; queued with local controls, never in an ISR. */
bool omni_ui_remote_frame(const uint8_t *frame,size_t length);
#endif
