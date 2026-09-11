#include "boot_ack.h"

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

omni_ack_result omni_boot_recovery_apply(const omni_ack_io *io,
    uint8_t page[OMNI_BOOT_METADATA_PAGE_SIZE], int32_t *driver_status)
{
    *driver_status = io->read_page(io->context, page);
    if (*driver_status != 0) return OMNI_ACK_READ_FAILED;
    uint32_t force = read32(page + 4);
    /* Accept only acknowledged running states. The captured loader's 0x721c
     * path normalizes pending; its 0x6f72 decision honors this force word.
     * Preserve all flags and the complete page, including reserved bytes. */
    if (read32(page) != UINT32_C(0xb00710ad) || read32(page + 8) != UINT32_C(0xc000) ||
        (force != 0 && force != UINT32_C(0xabbabaab)) ||
        page[12] > 1 || page[13] > 1 || page[14] != 1 ||
        (page[12] == 0 && page[13] == 0)) return OMNI_ACK_INVALID_METADATA;
    if (force == UINT32_C(0xabbabaab)) return OMNI_ACK_ALREADY_VALID;
    page[4] = 0xab; page[5] = 0xba; page[6] = 0xba; page[7] = 0xab;
    *driver_status = io->erase_page(io->context);
    if (*driver_status != 0) return OMNI_ACK_ERASE_FAILED;
    *driver_status = io->verify_erased(io->context);
    if (*driver_status != 0) return OMNI_ACK_BLANK_CHECK_FAILED;
    *driver_status = io->program_page(io->context, page);
    if (*driver_status != 0) return OMNI_ACK_PROGRAM_FAILED;
    *driver_status = io->verify_page(io->context, page);
    return *driver_status == 0 ? OMNI_ACK_WRITTEN : OMNI_ACK_VERIFY_FAILED;
}

omni_ack_result omni_boot_ack_apply(const omni_ack_io *io,
    uint8_t page[OMNI_BOOT_METADATA_PAGE_SIZE], int32_t *driver_status)
{
    *driver_status = io->read_page(io->context, page);
    if (*driver_status != 0) return OMNI_ACK_READ_FAILED;
    uint32_t force = read32(page + 4);
    /* Do not synthesize metadata or try to repair an unknown boot contract. */
    if (read32(page) != UINT32_C(0xb00710ad) || read32(page + 8) != UINT32_C(0xc000) ||
        (force != 0 && force != UINT32_C(0xabbabaab)) ||
        page[12] > 1 || page[13] > 1 || page[14] > 1)
        return OMNI_ACK_INVALID_METADATA;
    if (page[13] == 0)
        return page[12] == 1 && page[14] == 1 && force == 0 ?
            OMNI_ACK_ALREADY_VALID : OMNI_ACK_INVALID_METADATA;
    if (page[14] == 1 && force == 0) return OMNI_ACK_ALREADY_VALID;

    /* Same transition as stock 0x22680. Preserve eligibility and pending flags,
     * and every other byte of the 512-byte page, including its unused tail. */
    for (unsigned i = 4; i < 8; ++i) page[i] = 0;
    page[14] = 1;
    *driver_status = io->erase_page(io->context);
    if (*driver_status != 0) return OMNI_ACK_ERASE_FAILED;
    *driver_status = io->verify_erased(io->context);
    if (*driver_status != 0) return OMNI_ACK_BLANK_CHECK_FAILED;
    *driver_status = io->program_page(io->context, page);
    if (*driver_status != 0) return OMNI_ACK_PROGRAM_FAILED;
    *driver_status = io->verify_page(io->context, page);
    if (*driver_status != 0) return OMNI_ACK_VERIFY_FAILED;
    return OMNI_ACK_WRITTEN;
}
