#include "display_lpc5528.h"
#include "fsl_device_registers.h"
#include "fsl_clock.h"
#include "fsl_reset.h"

/* Recovered MCU1 SPI4 path, no stock application function calls. All writes
 * are confined to clocks, SPI4 and the five recovered display pins. */
static struct { const uint8_t *bytes; size_t length, sent; bool active; } tx;
static volatile uint32_t *reg(uint32_t address) { return (volatile uint32_t *)address; }
static void pin(unsigned port,unsigned number,bool high)
{
    *reg(0x4008c000U+(high?0x2200U:0x2280U)+port*4U)=1UL<<number;
}
static void pins(void *context,bool p0_15,bool p1_8)
{ (void)context; pin(0,15,p0_15); pin(1,8,p1_8); }
static void cancel(void *context)
{
    (void)context;
    SPI4->CFG &= ~SPI_CFG_ENABLE_MASK;
    SPI4->FIFOCFG=SPI_FIFOCFG_EMPTYTX_MASK;
    tx.active=false;
}
static int start(void *context,bool data,const uint8_t *bytes,size_t length)
{
    (void)context;
    if (tx.active || !bytes || !length || length>1024U ||
        !(SPI4->CFG & SPI_CFG_ENABLE_MASK) || !(SPI4->STAT & SPI_STAT_MSTIDLE_MASK)) return -1;
    pin(1,22,data);
    tx.bytes=bytes; tx.length=length; tx.sent=0; tx.active=true;
    return 0;
}
static int busy(void *context)
{
    (void)context;
    if (!tx.active) return 0;
    if (SPI4->FIFOSTAT & SPI_FIFOSTAT_TXERR_MASK) return -1;
    /* Bound work per poll; USB interrupts stay enabled. FIFO empty alone is
     * not completion: wait for the final serial bit via MSTIDLE as well. */
    for (unsigned budget=0;budget<8U && tx.sent<tx.length;budget++) {
        if (!(SPI4->FIFOSTAT & SPI_FIFOSTAT_TXNOTFULL_MASK)) break;
        uint32_t word=(uint32_t)tx.bytes[tx.sent] | SPI_FIFOWR_LEN(7) |
            SPI_FIFOWR_RXIGNORE_MASK | 0x000f0000U;
        if (++tx.sent==tx.length) word|=SPI_FIFOWR_EOT_MASK;
        SPI4->FIFOWR=word;
    }
    if (tx.sent==tx.length && (SPI4->FIFOSTAT & SPI_FIFOSTAT_TXEMPTY_MASK) &&
        (SPI4->STAT & SPI_STAT_MSTIDLE_MASK)) { tx.active=false; return 0; }
    return 1;
}
omni_display_io omni_display_lpc5528_init(void)
{
    CLOCK_EnableClock(kCLOCK_Iocon); CLOCK_EnableClock(kCLOCK_Gpio0); CLOCK_EnableClock(kCLOCK_Gpio1);
    pin(0,15,false); pin(1,8,false); pin(1,22,false);
    *reg(0x4000103cU)=0x4100U; *reg(0x400010a0U)=0x4100U; *reg(0x400010d8U)=0x4100U;
    *reg(0x4008e000U)|=1U<<15; *reg(0x4008e004U)|=(1U<<8)|(1U<<22);
    CLOCK_AttachClk(kFRO12M_to_FLEXCOMM4); CLOCK_EnableClock(kCLOCK_FlexComm4);
    RESET_PeripheralReset(kFC4_RST_SHIFT_RSTn);
    FLEXCOMM4->PSELID=(FLEXCOMM4->PSELID & ~7U)|2U;
    SPI4->CFG=SPI_CFG_MASTER_MASK; SPI4->DLY=0;
    SPI4->DIV=29U; /* 12 MHz / 30 = 400 kHz, matching stock request. */
    SPI4->FIFOCFG=SPI_FIFOCFG_EMPTYTX_MASK|SPI_FIFOCFG_ENABLETX_MASK;
    SPI4->FIFOSTAT=SPI_FIFOSTAT_TXERR_MASK;
    *reg(0x400010ccU)=0x4105U; *reg(0x400010d4U)=0x4105U;
    SPI4->CFG|=SPI_CFG_ENABLE_MASK; tx.active=false;
    omni_display_io io={0,pins,start,busy,cancel}; return io;
}
