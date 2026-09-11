#ifndef OMNI_EQ_NVM_H
#define OMNI_EQ_NVM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Persistent EQ store in the unused MCU1 flash gap [0x51C00,0x7D800): two
 * 512-byte A/B slots, versioned + CRC32'd, so a torn write keeps the prior
 * good copy. Outside the app image, its CRC, and every stock partition; it
 * survives both power cycles and app reflashes. */
#define OMNI_EQ_NVM_PAGE 512u
#define OMNI_EQ_NVM_SLOT0 0x60000u
#define OMNI_EQ_NVM_SLOT1 0x60200u
#define OMNI_EQ_NVM_MAX_PAYLOAD (OMNI_EQ_NVM_PAGE - 16u)

/* ---- Pure page (de)serialization: no flash access, host-testable ---- */
uint32_t omni_eq_nvm_crc32(const uint8_t *data, size_t length);
/* Build a 512-byte slot page {magic, seq, len, payload, crc}; pads with 0xff.
 * Returns 512 on success, 0 if payload too big. */
size_t omni_eq_nvm_build(uint8_t page[OMNI_EQ_NVM_PAGE], uint32_t seq,
                         const uint8_t *payload, size_t payload_len);
/* Validate magic+crc and extract seq + payload. */
bool omni_eq_nvm_parse(const uint8_t page[OMNI_EQ_NVM_PAGE], uint32_t *seq,
                       uint8_t *payload_out, size_t *payload_len);
/* Pick the newest valid slot (0 or 1), or -1 if neither is valid. next_seq is
 * the sequence to stamp on the next write (max valid seq + 1, or 1). */
int omni_eq_nvm_pick(const uint8_t a[OMNI_EQ_NVM_PAGE], const uint8_t b[OMNI_EQ_NVM_PAGE],
                     uint32_t *next_seq);

/* ---- Flash access (eq_nvm_lpc5528.c, ARM only) ---- */
/* ECC-safe FLASH_Read of slot 0/1 into page. false on driver failure. */
bool omni_eq_nvm_flash_read(unsigned slot, uint8_t page[OMNI_EQ_NVM_PAGE]);
/* Erase+verify+program+verify slot 0/1 (PREFEN off, IRQs off). false on failure. */
bool omni_eq_nvm_flash_write(unsigned slot, const uint8_t page[OMNI_EQ_NVM_PAGE]);

/* ---- Orchestration (eq_persist.c, ARM only) ---- */
/* Once at startup: read both slots, restore the newest valid EQ into the cache. */
void omni_eq_persist_restore(void);
/* Main-loop tick: persist the EQ to the inactive slot when it has changed and
 * settled. Never runs in an ISR; brief IRQ-off during the flash op. */
void omni_eq_persist_poll(uint32_t now);
#endif
