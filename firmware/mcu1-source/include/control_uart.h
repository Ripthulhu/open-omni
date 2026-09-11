#ifndef OMNI_CONTROL_UART_H
#define OMNI_CONTROL_UART_H
#include "mcu2_link.h"
#include <stdbool.h>
#include <stdint.h>
/* Port allowlist: FC3 DSP 921600 8N1, FC7 MCU2 921600 8N2. Each has its own
 * 2048-byte IRQ RX ring and failure state. Both may run concurrently;
 * one main-loop consumer owns each port's start/stop/RX. */
bool omni_control_uart_start(unsigned port,mcu2_link_io *io);
void omni_control_uart_stop(unsigned port);
void omni_control_uart_stats(unsigned port,uint32_t out[6]);
/* Read-only hardware TX-idle observation; false for an inactive port. */
bool omni_control_uart_tx_idle(unsigned port);
/* False before initialization or unsupported port; no peripheral access then.
 * Words: version(2),port,port_if_active_else_zero,PSELID,CFG,OSR,BRG,FIFOSTAT,FIFOTRIG,
 * FIFOINTSTAT,FCCLKSEL,FRG,TX_IOCON,RX_IOCON,GPIO0_PIN,GPIO1_PIN,
 * GPIO0_DIR,GPIO1_DIR,AHBCLKCTRL0,gpio_valid_mask(bit0=GPIO0,bit1=GPIO1).
 * GPIO is sampled only when its bus clock is enabled. Digital pin reads do
 * not measure voltage. Hardware may change during this read-only snapshot. */
bool omni_control_uart_snapshot(unsigned port,uint32_t out[20]);
void FLEXCOMM3_IRQHandler(void);
void FLEXCOMM7_IRQHandler(void);
#endif
