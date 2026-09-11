#include "control_uart.h"
#include "fsl_device_registers.h"
#include "fsl_clock.h"
#include "fsl_reset.h"
#include <string.h>

#define RX_SIZE 2048U
typedef struct {
    volatile uint32_t received,transmitted,fifo_errors,line_errors,dropped,failed;
    volatile uint32_t head,tail;
    uint8_t bytes[RX_SIZE];
    bool initialized,active;
} port_stats;
static port_stats stats[2];
static int port_index(unsigned port) { return port==3U?0:port==7U?1:-1; }
static USART_Type *uart_for(unsigned port) { return port==3U?USART3:USART7; }
static IRQn_Type irq_for(unsigned port) { return port==3U?FLEXCOMM3_IRQn:FLEXCOMM7_IRQn; }
/* Keep the two hardware pin contracts as separate literal write sequences.
 * This also avoids the emulator's multi-instruction IT/MMIO tracking issue;
 * that modeling defect is not evidence of an on-device firmware defect. */
static __attribute__((noinline)) void dsp_uart_pins(void)
{
    *(volatile uint32_t *)0x40001008U=0x121U; /* DSP TX PIO0_2/mux1. */
    *(volatile uint32_t *)0x4000100cU=0x121U; /* DSP RX PIO0_3/mux1. */
}
static __attribute__((noinline)) void mcu2_uart_pins(void)
{
    *(volatile uint32_t *)0x40001050U=0x127U; /* MCU2 RX PIO0_20/mux7. */
    *(volatile uint32_t *)0x400010f8U=0x121U; /* MCU2 TX PIO1_30/mux1. */
}

static void receive_irq(unsigned port)
{
    if(!stats[port_index(port)].active) return; /* Stopped port: no MMIO/ring work. */
    USART_Type *uart=uart_for(port);
    port_stats *s=&stats[port_index(port)];
    uint32_t flags=uart->FIFOSTAT;
    if(flags&(USART_FIFOSTAT_RXERR_MASK|USART_FIFOSTAT_TXERR_MASK)) {
        uart->FIFOSTAT=flags&(USART_FIFOSTAT_RXERR_MASK|USART_FIFOSTAT_TXERR_MASK);
        ++s->fifo_errors; s->failed=1;
    }
    for(unsigned budget=0;budget<16U;++budget) {
        if(!(uart->FIFOSTAT&USART_FIFOSTAT_RXNOTEMPTY_MASK)) break;
        uint32_t word=uart->FIFORD;
        ++s->received;
        if(word&(USART_FIFORD_FRAMERR_MASK|USART_FIFORD_PARITYERR_MASK|USART_FIFORD_RXNOISE_MASK)) {
            ++s->line_errors; s->failed=1; continue;
        }
        uint32_t h=s->head;
        if((uint32_t)(h-s->tail)>=RX_SIZE) { ++s->dropped; s->failed=1; continue; }
        s->bytes[h&(RX_SIZE-1U)]=(uint8_t)word;
        __DMB(); s->head=h+1U;
    }
    if(s->failed) uart->FIFOINTENCLR=0xfU;
}
void FLEXCOMM3_IRQHandler(void) { receive_irq(3U); }
void FLEXCOMM7_IRQHandler(void) { receive_irq(7U); }
static int tx(void *context,uint8_t byte)
{
    unsigned port=(unsigned)(uintptr_t)context;
    int index=port_index(port);
    if(index<0 || !stats[index].active || stats[index].failed) return -1;
    USART_Type *uart=uart_for(port);
    uint32_t flags=uart->FIFOSTAT;
    if(flags&USART_FIFOSTAT_TXERR_MASK) {
        ++stats[index].fifo_errors; stats[index].failed=1; return -1;
    }
    if(!(flags&USART_FIFOSTAT_TXNOTFULL_MASK)) return 0;
    uart->FIFOWR=byte; ++stats[index].transmitted; return 1;
}
static int rx(void *context,uint8_t *byte)
{
    unsigned port=(unsigned)(uintptr_t)context;
    int index=port_index(port);
    if(!byte || index<0 || !stats[index].active || stats[index].failed) return -1;
    port_stats *s=&stats[index];
    uint32_t t=s->tail;
    if(t==s->head) return 0;
    __DMB(); *byte=s->bytes[t&(RX_SIZE-1U)];
    __DMB(); s->tail=t+1U; return 1;
}
bool omni_control_uart_start(unsigned port,mcu2_link_io *io)
{
    if(!io) return false;
    *io=(mcu2_link_io){0};
    int index=port_index(port);
    if(index<0 || stats[index].active) return false;
    USART_Type *uart=uart_for(port);
    IRQn_Type irq=irq_for(port);
    NVIC_DisableIRQ(irq);
    stats[index].initialized=false;
    CLOCK_EnableClock(kCLOCK_Iocon);
    CLOCK_AttachClk(port==3U?kFRO12M_to_FLEXCOMM3:kFRO12M_to_FLEXCOMM7);
    SYSCON->FLEXFRGXCTRL[port]=0xffU; /* Local FRG bypass; main/USB clocks unchanged. */
    CLOCK_EnableClock(port==3U?kCLOCK_FlexComm3:kCLOCK_FlexComm7);
    RESET_PeripheralReset(port==3U?kFC3_RST_SHIFT_RSTn:kFC7_RST_SHIFT_RSTn);
    FLEXCOMM_Type *flex=port==3U?FLEXCOMM3:FLEXCOMM7;
    flex->PSELID=(flex->PSELID&~7U)|1U;
    if((flex->PSELID&FLEXCOMM_PSELID_PERSEL_MASK)!=1U) return false;
    uart->CFG=0;
    uart->INTENCLR=0xffffffffU; uart->FIFOINTENCLR=0xfU;
    uart->CTL=0; uart->OSR=12U; uart->BRG=0;
    uart->FIFOCFG=USART_FIFOCFG_EMPTYTX_MASK|USART_FIFOCFG_EMPTYRX_MASK|
        USART_FIFOCFG_ENABLETX_MASK|USART_FIFOCFG_ENABLERX_MASK;
    uart->FIFOSTAT=USART_FIFOSTAT_TXERR_MASK|USART_FIFOSTAT_RXERR_MASK;
    uart->FIFOTRIG=USART_FIFOTRIG_RXLVLENA_MASK|USART_FIFOTRIG_RXLVL(0);
    if(port==3U) dsp_uart_pins(); else mcu2_uart_pins();
    memset(&stats[index],0,sizeof(stats[index]));
    stats[index].initialized=true; stats[index].active=true;
    uart->CFG=USART_CFG_DATALEN(1)|USART_CFG_ENABLE_MASK|
        (port==7U?USART_CFG_STOPLEN_MASK:0U);
    uart->FIFOINTENSET=USART_FIFOINTENSET_RXERR_MASK|USART_FIFOINTENSET_RXLVL_MASK;
    NVIC_SetPriority(irq,5);
    NVIC_ClearPendingIRQ(irq); NVIC_EnableIRQ(irq);
    *io=(mcu2_link_io){(void *)(uintptr_t)port,tx,rx}; return true;
}
void omni_control_uart_stop(unsigned port)
{
    int index=port_index(port);
    if(index<0 || !stats[index].active) return;
    IRQn_Type irq=irq_for(port);
    USART_Type *uart=uart_for(port);
    NVIC_DisableIRQ(irq);
    uart->FIFOINTENCLR=0xfU; uart->CFG&=~USART_CFG_ENABLE_MASK;
    NVIC_ClearPendingIRQ(irq); stats[index].active=false;
}
bool omni_control_uart_tx_idle(unsigned port)
{
    int index=port_index(port);
    if(index<0 || !stats[index].active) return false;
    return (uart_for(port)->STAT&USART_STAT_TXIDLE_MASK)!=0U;
}
void omni_control_uart_stats(unsigned port,uint32_t out[6])
{
    int index=port_index(port);
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    if(index<0) memset(out,0,24);
    else {
        out[0]=stats[index].received; out[1]=stats[index].transmitted;
        out[2]=stats[index].fifo_errors; out[3]=stats[index].line_errors;
        out[4]=stats[index].dropped; out[5]=stats[index].failed;
    }
    __set_PRIMASK(mask);
}
bool omni_control_uart_snapshot(unsigned port,uint32_t out[20])
{
    int index=port_index(port);
    if(!out || index<0 || !stats[index].initialized) return false;
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    USART_Type *uart=uart_for(port);
    FLEXCOMM_Type *flex=port==3U?FLEXCOMM3:FLEXCOMM7;
    uint32_t gates=SYSCON->AHBCLKCTRL.AHBCLKCTRL0;
    memset(out,0,80);
    out[0]=2; out[1]=port; out[2]=stats[index].active?port:0U; out[3]=flex->PSELID;
    out[4]=uart->CFG; out[5]=uart->OSR; out[6]=uart->BRG;
    out[7]=uart->FIFOSTAT; out[8]=uart->FIFOTRIG; out[9]=uart->FIFOINTSTAT;
    out[10]=SYSCON->FCCLKSELX[port]; out[11]=SYSCON->FLEXFRGXCTRL[port];
    out[12]=*(volatile const uint32_t *)(port==3U?0x40001008U:0x400010f8U);
    out[13]=*(volatile const uint32_t *)(port==3U?0x4000100cU:0x40001050U);
    if(gates&SYSCON_AHBCLKCTRL0_GPIO0_MASK) {
        out[14]=GPIO->PIN[0]; out[16]=GPIO->DIR[0]; out[19]|=1U;
    }
    if(gates&SYSCON_AHBCLKCTRL0_GPIO1_MASK) {
        out[15]=GPIO->PIN[1]; out[17]=GPIO->DIR[1]; out[19]|=2U;
    }
    out[18]=gates;
    __set_PRIMASK(mask); return true;
}
