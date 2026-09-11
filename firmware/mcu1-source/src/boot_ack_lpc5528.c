#include "board_clock.h"
#include "boot_ack.h"
#include "fsl_iap.h"
#include "fsl_device_registers.h"
#include "fsl_clock.h"
#include <stddef.h>

extern void watchdog_feed(void);

volatile uint32_t boot_ack_status = OMNI_ACK_WAITING;
volatile int32_t boot_ack_driver_status;
static flash_config_t flash_config;
static uint8_t metadata_page[OMNI_BOOT_METADATA_PAGE_SIZE] __attribute__((aligned(512)));

/* Fixed MCU1 ROM/API contract, corroborated by the captured stock RAM config.
 * No application, bootloader-code, factory-data or FFR writes are exposed. */
enum { DRIVER_ROM_VERSION = -1001, DRIVER_ROM_TABLE = -1002,
       DRIVER_CLOCK = -1003, DRIVER_GEOMETRY = -1004 };

static int32_t initialize_flash(void)
{
    _Static_assert(offsetof(flash_config_t, modeConfig) == 0x28, "ROM config ABI");
    _Static_assert(OMNI_BOOT_METADATA_ADDRESS % OMNI_BOOT_METADATA_PAGE_SIZE == 0, "page alignment");
    if (CLOCK_GetFreq(kCLOCK_CoreSysClk) != OMNI_CORE_CLOCK_HZ ||
        CLOCK_GetFreq(kCLOCK_BusClk) != OMNI_CORE_CLOCK_HZ) return DRIVER_CLOCK;
    if ((*(const uint32_t *)0x130010f4U >> 16 & 0xffU) != 3U) return DRIVER_ROM_VERSION;
    uint32_t table = *(const uint32_t *)0x13001100U;
    if ((table & 3U) || table < 0x13000000U || table > 0x13020000U - 28U)
        return DRIVER_ROM_TABLE;
    const uint32_t *api = (const uint32_t *)table;
    for (unsigned i = 1; i <= 6; ++i) {
        if (!(api[i] & 1U) || api[i] < 0x13000001U || api[i] >= 0x13020000U)
            return DRIVER_ROM_TABLE;
    }
    /* The CPU96 board and ROM timing agree BEFORE initialization.
     * Keep the checked ROM entry and constrained metadata-page policy. */
    flash_config.modeConfig.sysFreqInMHz = OMNI_CORE_CLOCK_MHZ;
    typedef status_t (*rom_init_fn)(flash_config_t *);
    int32_t status = ((rom_init_fn)api[1])(&flash_config);
    if (status != 0) return status;
    if (flash_config.PFlashBlockBase != 0 || flash_config.PFlashTotalSize != 0x80000U ||
        flash_config.PFlashBlockCount != 1 || flash_config.PFlashPageSize != 512U ||
        flash_config.PFlashSectorSize != 0x8000U || flash_config.modeConfig.sysFreqInMHz != OMNI_CORE_CLOCK_MHZ)
        return DRIVER_GEOMETRY;
    return 0;
}

static int32_t read_page(void *context, uint8_t *page)
{
    watchdog_feed();
    /* AHB reads of erased/bad-ECC flash can HardFault (NXP AN12949). */
    return FLASH_Read(context, OMNI_BOOT_METADATA_ADDRESS, page, OMNI_BOOT_METADATA_PAGE_SIZE);
}
static int32_t erase_page(void *context)
{
    watchdog_feed();
    return FLASH_Erase(context, OMNI_BOOT_METADATA_ADDRESS, OMNI_BOOT_METADATA_PAGE_SIZE, kFLASH_ApiEraseKey);
}
static int32_t verify_erased(void *context)
{
    watchdog_feed();
    return FLASH_VerifyErase(context, OMNI_BOOT_METADATA_ADDRESS, OMNI_BOOT_METADATA_PAGE_SIZE);
}
static int32_t program_page(void *context, const uint8_t *page)
{
    watchdog_feed();
    return FLASH_Program(context, OMNI_BOOT_METADATA_ADDRESS, page, OMNI_BOOT_METADATA_PAGE_SIZE);
}
static int32_t verify_page(void *context, const uint8_t *page)
{
    uint32_t failed_address = 0, failed_data = 0;
    watchdog_feed();
    return FLASH_VerifyProgram(context, OMNI_BOOT_METADATA_ADDRESS, OMNI_BOOT_METADATA_PAGE_SIZE,
                               page, &failed_address, &failed_data);
}

void boot_acknowledge_startup(void)
{
    if (boot_ack_status != OMNI_ACK_WAITING) return; /* One attempt per startup. */
    const omni_ack_io io = {&flash_config, read_page, erase_page, verify_erased, program_page, verify_page};
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    /* UM11126 rev2.8, Table 113 note [2] and 5.6.1.1: disable CPU0
     * prefetch before any flash-controller command, including ROM init. */
    uint32_t prefetch = SYSCON->FMCCR & SYSCON_FMCCR_PREFEN_MASK;
    SYSCON->FMCCR &= ~SYSCON_FMCCR_PREFEN_MASK;
    __DSB();
    __ISB();
    watchdog_feed();
    int32_t driver_status = initialize_flash();
    omni_ack_result result = OMNI_ACK_DRIVER_FAILED;
    if (driver_status == 0) result = omni_boot_ack_apply(&io, metadata_page, &driver_status);
    boot_ack_driver_status = driver_status;
    boot_ack_status = (uint32_t)result;
    watchdog_feed();
    /* Restore only PREFEN: ROM init may have changed flash timing fields. */
    SYSCON->FMCCR = (SYSCON->FMCCR & ~SYSCON_FMCCR_PREFEN_MASK) | prefetch;
    __DSB();
    __ISB();
    __set_PRIMASK(mask);
}

omni_ack_result boot_prepare_recovery(int32_t *driver_status)
{
    const omni_ack_io io = {&flash_config, read_page, erase_page, verify_erased, program_page, verify_page};
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    uint32_t prefetch = SYSCON->FMCCR & SYSCON_FMCCR_PREFEN_MASK;
    SYSCON->FMCCR &= ~SYSCON_FMCCR_PREFEN_MASK;
    __DSB(); __ISB();
    *driver_status = initialize_flash();
    omni_ack_result result = OMNI_ACK_DRIVER_FAILED;
    if (*driver_status == 0)
        result = omni_boot_recovery_apply(&io, metadata_page, driver_status);
    watchdog_feed();
    SYSCON->FMCCR = (SYSCON->FMCCR & ~SYSCON_FMCCR_PREFEN_MASK) | prefetch;
    __DSB(); __ISB();
    __set_PRIMASK(mask);
    return result;
}
