#ifndef OMNI_AUDIO_CLOCK_H
#define OMNI_AUDIO_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* Inactive clock sequencer. No MMIO binding is supplied: board oscillator
 * mode/trim and physical 16 MHz reference must be established before binding.
 * Enum values select fixed registers, never an arbitrary caller address. */
typedef enum {
    OMNI_AC_MAINCLKA, OMNI_AC_MAINCLKB, OMNI_AC_AHBDIV,
    OMNI_AC_USB0SEL, OMNI_AC_USB0DIV, OMNI_AC_FRO192M_CTRL,
    OMNI_AC_XO_CTRL, OMNI_AC_CLOCK_CTRL, OMNI_AC_MCLKIO,
    OMNI_AC_FC0SEL, OMNI_AC_FC2SEL, OMNI_AC_MCLKSEL,
    OMNI_AC_XO_STATUS, OMNI_AC_PLL0SEL, OMNI_AC_PLL0CTRL,
    OMNI_AC_PLL0NDEC, OMNI_AC_PLL0PDEC, OMNI_AC_PLL0SSCG0,
    OMNI_AC_PLL0SSCG1, OMNI_AC_PLL0STAT, OMNI_AC_PLL0DIV,
    OMNI_AC_MCLKDIV, OMNI_AC_POWER_SET, OMNI_AC_POWER_CLEAR,
    OMNI_AC_REGISTER_COUNT
} omni_audio_clock_register_t;

typedef struct {
    void *context;
    /* Must remain true for the entire sequence: exclusive PLL0 and MCLK
     * ownership; every downstream PLL0 consumer disconnected; external audio
     * pins isolated; I2S/DMA stopped; required register-access clocks enabled.
     * It must not claim success from a software intent flag alone. This module
     * prepares MCLK internally, so its own MCLK selector is exempt after setup.
     * No callback may block, change ownership, or enable an external clock. */
    bool (*guard)(void *context);
    bool (*read)(void *context, omni_audio_clock_register_t reg, uint32_t *value);
    bool (*write)(void *context, omni_audio_clock_register_t reg, uint32_t value);
} omni_audio_clock_ops_t;

typedef enum {
    OMNI_AUDIO_CLOCK_IDLE,
    OMNI_AUDIO_CLOCK_PREFLIGHT,
    OMNI_AUDIO_CLOCK_XO_SETUP,
    OMNI_AUDIO_CLOCK_WAIT_XO,
    OMNI_AUDIO_CLOCK_PLL_SETUP,
    OMNI_AUDIO_CLOCK_WAIT_PLL,
    OMNI_AUDIO_CLOCK_DIV_SETUP,
    OMNI_AUDIO_CLOCK_WAIT_DIV,
    OMNI_AUDIO_CLOCK_VERIFY,
    /* Local 49.152 MHz PLL / 24.576 MHz MCLK configuration only. MCLKIO stays
     * input, FC0/FC2 stay disconnected from PLL0, audio pins stay isolated. */
    OMNI_AUDIO_CLOCK_LOCAL_READY,
    OMNI_AUDIO_CLOCK_FAILED,
    OMNI_AUDIO_CLOCK_CANCELED
} omni_audio_clock_state_t;

typedef enum {
    OMNI_AUDIO_CLOCK_ERROR_NONE,
    OMNI_AUDIO_CLOCK_ERROR_PREREQUISITE,
    OMNI_AUDIO_CLOCK_ERROR_OWNERSHIP,
    OMNI_AUDIO_CLOCK_ERROR_IO,
    OMNI_AUDIO_CLOCK_ERROR_STATE_CHANGED,
    OMNI_AUDIO_CLOCK_ERROR_XO_TIMEOUT,
    OMNI_AUDIO_CLOCK_ERROR_PLL_TIMEOUT,
    OMNI_AUDIO_CLOCK_ERROR_DIV_TIMEOUT,
    OMNI_AUDIO_CLOCK_ERROR_TOTAL_TIMEOUT
} omni_audio_clock_error_t;

typedef struct {
    omni_audio_clock_ops_t ops;
    omni_audio_clock_state_t state;
    omni_audio_clock_error_t error;
    uint32_t initial[12];
    uint32_t expected_xo;
    uint32_t started_ms, phase_ms;
    uint32_t last_status;
    uint32_t successful_writes;
    /* Retained readback evidence when ERROR_STATE_CHANGED is raised. Values
     * are raw; compare them using mismatch_mask. Init clears all fields. */
    uint32_t mismatch_reg, mismatch_expected, mismatch_actual, mismatch_mask;
    uint8_t step;
    bool initialized;
} omni_audio_clock_t;

#define OMNI_AUDIO_CLOCK_REFERENCE_HZ 16000000u
#define OMNI_AUDIO_CLOCK_PLL_HZ 49152000u
#define OMNI_AUDIO_CLOCK_MCLK_HZ 24576000u
#define OMNI_AUDIO_CLOCK_XO_MASK 0x01fffffeu
#define OMNI_AUDIO_CLOCK_XO_TIMEOUT_MS 100u
#define OMNI_AUDIO_CLOCK_PLL_TIMEOUT_MS 20u
/* The fractional mode requires 6 ms settling (UM11126 4.6.6.2.1 and pinned
 * CLOCK_SetupPLL0Prec). Seven millisecond ticks cover timestamp quantization. */
#define OMNI_AUDIO_CLOCK_FRACTIONAL_SETTLE_MS 7u
#define OMNI_AUDIO_CLOCK_DIV_TIMEOUT_MS 20u
#define OMNI_AUDIO_CLOCK_TOTAL_TIMEOUT_MS 500u

/* Software only. Reinitialize only after an external owner has restored safe
 * isolated hardware state; an interrupted configuration is not rolled back. */
bool omni_audio_clock_init(omni_audio_clock_t *clock, const omni_audio_clock_ops_t *ops);
/* oscillator_verified requires independent board evidence of mode/trim and
 * physical 16 MHz reference. observed_xo_control is that validated capture,
 * not a convenient reset value. The system-output bit may already be set.
 * Only the current FRO12 CPU/FRO96 USB0 source configuration is supported. */
bool omni_audio_clock_begin(omni_audio_clock_t *clock, uint32_t reference_hz,
                            uint32_t observed_xo_control, bool oscillator_verified,
                            uint32_t now_ms);
/* One bounded guard callback and at most one register callback per poll.
 * Unsigned elapsed time handles wrap. Timer must advance independently of
 * PLL0. No retries, pin connection, stock function calls, or busy waits. */
void omni_audio_clock_poll(omni_audio_clock_t *clock, uint32_t now_ms);
/* Cancels without register access. Clocks already powered remain powered;
 * outputs remain isolated. External recovery must inspect the partial state. */
void omni_audio_clock_cancel(omni_audio_clock_t *clock);

#endif
