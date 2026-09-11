#include "eq_nvm.h"
#include "board_clock.h"
#include "fsl_iap.h"
#include "fsl_device_registers.h"
#include "fsl_clock.h"
#include <string.h>

/* Runtime EQ persistence to two 512-byte flash slots at 0x60000/0x60200, in the
 * unused flash gap. Same ROM-IAP discipline as boot_ack_lpc5528.c: CPU0 prefetch
 * off and interrupts off around every flash-controller command; ECC-safe
 * FLASH_Read on reload (a torn/erased page has uninitialised ECC and pointer
 * reads HardFault, AN12949). Called only from the main loop, never an ISR. */

extern void watchdog_feed(void);
static flash_config_t cfg;
static uint8_t nvm_buf[OMNI_EQ_NVM_PAGE] __attribute__((aligned(512)));

static bool ready(void)
{
    cfg.modeConfig.sysFreqInMHz = OMNI_CORE_CLOCK_MHZ;
    if (FLASH_Init(&cfg) != (status_t)0) return false;
    return cfg.PFlashTotalSize == 0x80000u && cfg.PFlashPageSize == 512u;
}
static uint32_t slot_addr(unsigned slot) { return slot ? OMNI_EQ_NVM_SLOT1 : OMNI_EQ_NVM_SLOT0; }

bool omni_eq_nvm_flash_read(unsigned slot, uint8_t page[OMNI_EQ_NVM_PAGE])
{
    if (slot > 1u || !page) return false;
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    uint32_t prefetch = SYSCON->FMCCR & SYSCON_FMCCR_PREFEN_MASK;
    SYSCON->FMCCR &= ~SYSCON_FMCCR_PREFEN_MASK;
    __DSB(); __ISB(); watchdog_feed();
    bool ok = ready() && FLASH_Read(&cfg, slot_addr(slot), page, OMNI_EQ_NVM_PAGE) == (status_t)0;
    SYSCON->FMCCR = (SYSCON->FMCCR & ~SYSCON_FMCCR_PREFEN_MASK) | prefetch;
    __DSB(); __ISB();
    __set_PRIMASK(mask);
    return ok;
}

bool omni_eq_nvm_flash_write(unsigned slot, const uint8_t page[OMNI_EQ_NVM_PAGE])
{
    if (slot > 1u || !page) return false;
    uint32_t addr = slot_addr(slot);
    memcpy(nvm_buf, page, OMNI_EQ_NVM_PAGE);
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    uint32_t prefetch = SYSCON->FMCCR & SYSCON_FMCCR_PREFEN_MASK;
    SYSCON->FMCCR &= ~SYSCON_FMCCR_PREFEN_MASK;
    __DSB(); __ISB(); watchdog_feed();
    bool ok = false;
    if (ready()) {
        uint32_t fa = 0, fd = 0;
        ok = FLASH_Erase(&cfg, addr, OMNI_EQ_NVM_PAGE, kFLASH_ApiEraseKey) == (status_t)0
          && FLASH_VerifyErase(&cfg, addr, OMNI_EQ_NVM_PAGE) == (status_t)0
          && FLASH_Program(&cfg, addr, nvm_buf, OMNI_EQ_NVM_PAGE) == (status_t)0
          && FLASH_VerifyProgram(&cfg, addr, OMNI_EQ_NVM_PAGE, nvm_buf, &fa, &fd) == (status_t)0;
    }
    watchdog_feed();
    SYSCON->FMCCR = (SYSCON->FMCCR & ~SYSCON_FMCCR_PREFEN_MASK) | prefetch;
    __DSB(); __ISB();
    __set_PRIMASK(mask);
    return ok;
}
