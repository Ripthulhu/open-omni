#ifndef OMNI_AUDIO_CLOCK_LPC5528_H
#define OMNI_AUDIO_CLOCK_LPC5528_H

#include "audio_clock.h"
#include <stdint.h>

/* Table-driven MMIO backend for the dormant audio-clock sequencer.
 *
 * This binds omni_audio_clock_ops_t to the 24 recovered LPC5528 register
 * addresses. It performs ZERO SDK clock/reset/power (CLOCK_, RESET_, fsl_power)
 * calls and enables no access clocks: the sequencer contract requires register
 * gates to already be on before it is bound. Nothing here is wired into
 * omni_mcu1.elf; the file stays dormant by construction.
 *
 * The register path is a plain volatile load/store through the address table.
 * To keep the whole file host-compilable (no fsl headers) AND to let the host
 * test exercise guard()'s isolation logic without dereferencing MMIO on x86,
 * the actual load/store goes through two accessor function pointers. On the
 * target, leave them NULL and the default hardware accessors (real volatile
 * load/store) are used. The host test installs recording accessors instead. */
typedef struct {
    /* NULL on target -> omni_audio_clock_lpc5528_read_mmio / _write_mmio. */
    uint32_t (*mmio_read)(void *user, uint32_t address);
    void (*mmio_write)(void *user, uint32_t address, uint32_t value);
    void *user;
} omni_audio_clock_lpc5528_t;

/* Single source of truth: pointer to table[OMNI_AC_REGISTER_COUNT], indexed by
 * omni_audio_clock_register_t, holding each enum member's recovered address. */
const uint32_t *omni_audio_clock_lpc5528_addresses(void);

/* Fills an ops struct bound to the given backend (must outlive the ops).
 * read() rejects OMNI_AC_POWER_SET/OMNI_AC_POWER_CLEAR (write-only aliases);
 * guard() performs real runtime isolation checks against downstream consumers. */
omni_audio_clock_ops_t omni_audio_clock_lpc5528_ops(omni_audio_clock_lpc5528_t *backend);

/* Default target accessors: real, SDK-free volatile load/store. Exercised only
 * by the ARM compile gate; the host test never calls these. */
uint32_t omni_audio_clock_lpc5528_read_mmio(void *user, uint32_t address);
void omni_audio_clock_lpc5528_write_mmio(void *user, uint32_t address, uint32_t value);

#endif
