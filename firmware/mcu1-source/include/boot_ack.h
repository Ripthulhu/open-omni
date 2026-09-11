#ifndef OMNI_BOOT_ACK_H
#define OMNI_BOOT_ACK_H
#include <stdint.h>

#define OMNI_BOOT_METADATA_ADDRESS UINT32_C(0x7f800)
#define OMNI_BOOT_METADATA_PAGE_SIZE 512U

typedef enum {
    OMNI_ACK_WAITING = 0,
    OMNI_ACK_WRITTEN = 1,
    OMNI_ACK_ALREADY_VALID = 2,
    OMNI_ACK_INVALID_METADATA = 3,
    OMNI_ACK_READ_FAILED = 4,
    OMNI_ACK_ERASE_FAILED = 5,
    OMNI_ACK_BLANK_CHECK_FAILED = 6,
    OMNI_ACK_PROGRAM_FAILED = 7,
    OMNI_ACK_VERIFY_FAILED = 8,
    OMNI_ACK_DRIVER_FAILED = 9
} omni_ack_result;

/* Fixed-page operations: the policy cannot request arbitrary flash addresses. */
typedef struct {
    void *context;
    int32_t (*read_page)(void *, uint8_t *);
    int32_t (*erase_page)(void *);
    int32_t (*verify_erased)(void *);
    int32_t (*program_page)(void *, const uint8_t *);
    int32_t (*verify_page)(void *, const uint8_t *);
} omni_ack_io;

omni_ack_result omni_boot_ack_apply(const omni_ack_io *io,
    uint8_t page[OMNI_BOOT_METADATA_PAGE_SIZE], int32_t *driver_status);
void boot_acknowledge_startup(void);
omni_ack_result omni_boot_recovery_apply(const omni_ack_io *io,
    uint8_t page[OMNI_BOOT_METADATA_PAGE_SIZE], int32_t *driver_status);
omni_ack_result boot_prepare_recovery(int32_t *driver_status);
extern volatile uint32_t boot_ack_status;
extern volatile int32_t boot_ack_driver_status;
#endif
