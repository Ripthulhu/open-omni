#ifndef OMNI_MCU2_UART_LPC5528_H
#define OMNI_MCU2_UART_LPC5528_H
#include "mcu2_link.h"
#include <stdint.h>
/* Explicit research transaction only. Never reset or reflash the peer. */
mcu2_link_io omni_mcu2_uart_init(void);
void omni_mcu2_uart_stop(void);
void omni_mcu2_uart_stats(uint32_t out[6]);
void FLEXCOMM7_IRQHandler(void);
#endif
