#include "mcu2_uart_lpc5528.h"
#include "control_uart.h"

mcu2_link_io omni_mcu2_uart_init(void)
{
    mcu2_link_io io={0};
    (void)omni_control_uart_start(7U,&io);
    return io;
}
void omni_mcu2_uart_stop(void) { omni_control_uart_stop(7U); }
void omni_mcu2_uart_stats(uint32_t out[6]) { omni_control_uart_stats(7U,out); }
