#ifndef OMNI_USB_ISO_BANKS_H
#define OMNI_USB_ISO_BANKS_H
#include <stdbool.h>
#include <stdint.h>

/* Source-owned LPCIP3511 Full-Speed packet banks. All calls are serialized by
 * the USB IRQ owner. These functions never access controller registers. The
 * DCI adapter alone owns endpoint interrupts, ACTIVE publication and EPSKIP.
 * The SDK logical-transfer engine must not operate a claimed endpoint. */
enum {
    OMNI_ISO_RX_ENDPOINT=0x03, OMNI_ISO_FEEDBACK_ENDPOINT=0x84,
    OMNI_ISO_RX_CAPACITY=196, OMNI_ISO_RX_MAX_CAPACITY=582,
    OMNI_ISO_RX_STORAGE=640, OMNI_ISO_FEEDBACK_BYTES=4,
    OMNI_ISO_NO_SLOT=255,
    OMNI_ISO_SLOT_FREE=0, OMNI_ISO_SLOT_HARDWARE=1, OMNI_ISO_SLOT_CPU=2
};
#define OMNI_ISO_ACTIVE UINT32_C(0x80000000)

typedef struct {
    void *context;
    /* Return one volatile Full-Speed endpoint command/status word. When it is
     * inactive, apply the acquire DMB before subsequent DMA payload reads. */
    uint32_t (*read)(void *context, uint8_t endpoint, uint8_t bank);
    /* Check ACTIVE again; reject without changing anything if still active.
     * Publish a COMPLETE new descriptor (address, capacity, ISO, ACTIVE) with
     * the necessary DMB. Validate 64-byte alignment and DATABUFSTART window.
     * Never preserve the address/NBYTES mutated by the completed transaction.
     * A false return must guarantee that the supplied buffer was not armed. */
    bool (*arm)(void *context, uint8_t endpoint, uint8_t bank,
                uint8_t *buffer, uint16_t length);
    /* Synchronous RX delivery. Must finish using buffer before returning.
     * Its bank has already been armed with a DIFFERENT, formerly free buffer.
     * It may call stop(), but must not open/quiesce/service recursively. */
    void (*consume)(void *context, const uint8_t *buffer, uint16_t length);
    /* Fill exactly four bytes in a CPU-owned feedback buffer. */
    void (*fill_feedback)(void *context, uint8_t *buffer);
} omni_iso_banks_ops_t;

typedef struct {
    omni_iso_banks_ops_t ops;
    _Alignas(64) union {
        uint8_t rx[3][OMNI_ISO_RX_STORAGE];
        uint8_t feedback[2][64];
    } payload;
    uint32_t generation, packets, bytes, arm_failures, malformed,
             ownership_errors, coalesced_services, discarded;
    uint8_t endpoint, expected_bank, bank_slot[2], slot_owner[3];
    uint16_t packet_capacity;
    uint8_t frame_bytes;
    bool claimed, running, servicing;
} omni_iso_banks_t;

void omni_iso_banks_init(omni_iso_banks_t *state,
                         const omni_iso_banks_ops_t *ops);
/* Both hardware descriptors must be inactive; the adapter must already have
 * selected first_bank in EPINUSE, enabled EPBUFCFG double mode, and configured
 * this endpoint as ISO. Partial arm failure leaves the endpoint claimed and
 * stopped: caller must cancel ACTIVE banks, then call quiesced(). */
bool omni_iso_banks_open(omni_iso_banks_t *state, uint8_t endpoint,
                         uint8_t first_bank);
/* Geometry is immutable until cancellation/quiescence. Only the four supported
 * playback formats are admitted:196/4,294/6,388/4,582/6. Feedback uses4/4. */
bool omni_iso_banks_open_format(omni_iso_banks_t *state,uint8_t endpoint,
    uint8_t first_bank,uint16_t packet_capacity,uint8_t frame_bytes);
/* At most two ordered packet completions per call, even if new tokens arrive
 * in consume(). Each short packet is independent, never a logical multi-
 * packet SDK transfer. Returns the number consumed/completed this call. */
unsigned omni_iso_banks_service(omni_iso_banks_t *state);
/* Stops rearming immediately, without freeing any hardware-owned storage. */
void omni_iso_banks_stop(omni_iso_banks_t *state);
/* Caller has canceled both banks (or reset the controller) and cleared their
 * pending interrupt. Refuses to release ownership while either ACTIVE=1.
 * No callbacks are made for canceled/stale completions. */
bool omni_iso_banks_quiesced(omni_iso_banks_t *state);

#endif
