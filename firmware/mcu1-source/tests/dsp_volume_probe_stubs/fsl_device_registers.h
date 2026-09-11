#ifndef OMNI_TEST_VOLUME_REGISTERS_H
#define OMNI_TEST_VOLUME_REGISTERS_H
#include <stdint.h>
typedef struct { volatile uint32_t STAT,FIFOSTAT; } test_usart;
extern test_usart test_uart3;
#define USART3 (&test_uart3)
#define USART_STAT_TXIDLE_MASK 8u
#define USART_FIFOSTAT_TXEMPTY_MASK 16u
#define USART_FIFOSTAT_TXERR_MASK 1u
#define USART_FIFOSTAT_RXERR_MASK 2u
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t);
#define __DMB() ((void)0)
#endif
