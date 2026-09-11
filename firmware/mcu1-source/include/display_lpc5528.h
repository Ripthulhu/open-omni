#ifndef OMNI_DISPLAY_LPC5528_H
#define OMNI_DISPLAY_LPC5528_H
#include "display.h"
/* Explicit bring-up only; not called by the current diagnostic application. */
omni_display_io omni_display_lpc5528_init(void);
/* Runtime SPI4 divider; SCK = 12MHz/(div+1), applied at the next idle
 * transfer. Default 29 (400kHz). Higher SCK gives faster full-frame blits. */
void omni_display_lpc5528_set_div(uint32_t div);
#endif
