#include "board_clock.h"
#include "fsl_device_registers.h"
#include "fsl_clock.h"
#include "fsl_power.h"
#include "fsl_reset.h"
#include "fsl_common.h"
#include <stdint.h>

uint32_t SystemCoreClock = 12000000U;
volatile uint32_t omni_fault;
void watchdog_feed(void);

/* A deliberate detached interval lets the host observe device removal before
 * a new identity appears. Feed the inherited watchdog throughout the wait.
 * This is a reconnect policy, not a claim that host teardown finishes in 250ms. */
void board_usb_detach_wait(void)
{
    for (unsigned i = 0; i < 25; ++i) {
        watchdog_feed();
        SDK_DelayAtLeastUs(10000U, SystemCoreClock);
    }
}

void watchdog_feed(void)
{
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    *(volatile uint8_t *)0x4000c008U = 0xaa;
    *(volatile uint8_t *)0x4000c008U = 0x55;
    __set_PRIMASK(mask);
}

void Fault_Handler(void)
{
    __disable_irq();
    omni_fault = __get_IPSR();
    /* Let the inherited watchdog reset the application after a fault. */
    for (;;) { __NOP(); }
}

void board_init(void)
{
    for (unsigned i = 0; i < 2; ++i) {
        NVIC->ICER[i] = 0xffffffffU;
        NVIC->ICPR[i] = 0xffffffffU;
    }
    watchdog_feed();
    POWER_DisablePD(kPDRUNCFG_PD_FRO192M);
    CLOCK_SetupFROClocking(12000000U);
    CLOCK_AttachClk(kFRO12M_to_MAIN_CLK);
    CLOCK_SetClkDiv(kCLOCK_DivAhbClk, 1, false);
    CLOCK_SetupFROClocking(96000000U);
    /* Pinned BOARD_BootClockFROHF96M and stock 0x297f4 order: prepare
     * voltage and Flash timing at FRO12 BEFORE raising CPU/AHB. */
    POWER_SetVoltageForFreq(OMNI_CORE_CLOCK_HZ);
    CLOCK_SetFLASHAccessCyclesForFreq(OMNI_CORE_CLOCK_HZ);
    CLOCK_SetClkDiv(kCLOCK_DivAhbClk, 1, false);
    CLOCK_AttachClk(kFRO_HF_to_MAIN_CLK);
    __DSB(); __ISB();
    if (CLOCK_GetFreq(kCLOCK_CoreSysClk)!=OMNI_CORE_CLOCK_HZ ||
        CLOCK_GetFreq(kCLOCK_BusClk)!=OMNI_CORE_CLOCK_HZ) Fault_Handler();
    SystemCoreClock=OMNI_CORE_CLOCK_HZ;
    POWER_DisablePD(kPDRUNCFG_PD_USB0_PHY);
    RESET_PeripheralReset(kUSB0D_RST_SHIFT_RSTn);
    RESET_PeripheralReset(kUSB0HSL_RST_SHIFT_RSTn);
    RESET_PeripheralReset(kUSB0HMR_RST_SHIFT_RSTn);
    /* PORTMODE also requires the USB functional clock. A warm updater handoff
     * hides USB0CLKDIV.HALT=1 at reset; establish the clock before that access. */
    CLOCK_SetClkDiv(kCLOCK_DivUsb0Clk, 1, false);
    CLOCK_AttachClk(kFRO_HF_to_USB0_CLK);
    CLOCK_EnableClock(kCLOCK_Usbhsl0);
    USBFSH->PORTMODE |= USBFSH_PORTMODE_DEV_ENABLE_MASK;
    CLOCK_DisableClock(kCLOCK_Usbhsl0);
    CLOCK_EnableUsbfs0DeviceClock(kCLOCK_UsbfsSrcFro, 96000000U);
    for (volatile uint32_t *p = (volatile uint32_t *)0x40100000U;
         p < (volatile uint32_t *)0x40104000U; ++p) { *p = 0; }
    watchdog_feed();
}
