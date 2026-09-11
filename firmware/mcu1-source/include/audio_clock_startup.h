#include "board_clock.h"
#ifndef OMNI_AUDIO_CLOCK_STARTUP_H
#define OMNI_AUDIO_CLOCK_STARTUP_H

#include "audio_clock.h"

/* Bound the caller even if its independent millisecond interrupt stops. This
 * is a failure limit, never an oscillator/PLL settling-time substitute. */
#define OMNI_AUDIO_CLOCK_STARTUP_POLL_LIMIT OMNI_CPU_GUARD_ITERATIONS(200000u)

/* XO_READY does not establish the board's required mode/output/trim settings.
 * The observed cold-start state was ready with XO_CTRL=0x0021428a. Restore the
 * independently captured stock configuration only with isolated consumers and
 * the supported FRO-based CPU/USB clocks. The PLL sequencer's preflight remains
 * strict and runs after this separate board-preparation step. */
static inline omni_audio_clock_error_t omni_audio_clock_startup_prepare_xo(
    const omni_audio_clock_ops_t *ops, bool *changed)
{
    if (!ops || !ops->read || !ops->write || !ops->guard || !changed)
        return OMNI_AUDIO_CLOCK_ERROR_PREREQUISITE;
    *changed=false;
    uint32_t status, control, clock_control;
    if (!ops->read(ops->context,OMNI_AC_XO_STATUS,&status) ||
        !ops->read(ops->context,OMNI_AC_XO_CTRL,&control) ||
        !ops->read(ops->context,OMNI_AC_CLOCK_CTRL,&clock_control))
        return OMNI_AUDIO_CLOCK_ERROR_IO;
    if ((status&1u) && (control&OMNI_AUDIO_CLOCK_XO_MASK)==0x01c3459au &&
        (clock_control&0x20u)) return OMNI_AUDIO_CLOCK_ERROR_NONE;
    if (!ops->guard(ops->context)) return OMNI_AUDIO_CLOCK_ERROR_OWNERSHIP;
    static const struct { omni_audio_clock_register_t reg; uint32_t mask, value; } safe[]={
        {OMNI_AC_MAINCLKA,7u,OMNI_CORE_MAIN_A},{OMNI_AC_MAINCLKB,3u,0u},
        {OMNI_AC_AHBDIV,0xe00000ffu,0u},{OMNI_AC_USB0SEL,7u,3u},
        {OMNI_AC_USB0DIV,0xe00000ffu,1u},{OMNI_AC_MCLKIO,1u,0u}
    };
    for (unsigned i=0;i<sizeof(safe)/sizeof(safe[0]);++i) {
        uint32_t value;
        if (!ops->read(ops->context,safe[i].reg,&value)) return OMNI_AUDIO_CLOCK_ERROR_IO;
        if ((value&safe[i].mask)!=safe[i].value) return OMNI_AUDIO_CLOCK_ERROR_PREREQUISITE;
    }
    uint32_t mclk;
    if (!ops->read(ops->context,OMNI_AC_MCLKSEL,&mclk)) return OMNI_AUDIO_CLOCK_ERROR_IO;
    if ((mclk&7u)==1u) return OMNI_AUDIO_CLOCK_ERROR_OWNERSHIP;
    clock_control=(clock_control&0x3ffu)|0x20u;
    const struct { omni_audio_clock_register_t reg; uint32_t value; } writes[]={
        {OMNI_AC_POWER_CLEAR,0x100u},{OMNI_AC_POWER_CLEAR,0x100000u},
        {OMNI_AC_CLOCK_CTRL,clock_control},{OMNI_AC_XO_CTRL,0x01c3459au}
    };
    for (unsigned i=0;i<sizeof(writes)/sizeof(writes[0]);++i) {
        if (!ops->guard(ops->context)) return OMNI_AUDIO_CLOCK_ERROR_OWNERSHIP;
        if (!ops->write(ops->context,writes[i].reg,writes[i].value)) return OMNI_AUDIO_CLOCK_ERROR_IO;
        *changed=true;
    }
    uint32_t actual_clock_control;
    if (!ops->read(ops->context,OMNI_AC_XO_CTRL,&control) ||
        !ops->read(ops->context,OMNI_AC_CLOCK_CTRL,&actual_clock_control))
        return OMNI_AUDIO_CLOCK_ERROR_IO;
    if ((control&OMNI_AUDIO_CLOCK_XO_MASK)!=0x01c3459au ||
        (actual_clock_control&0x3ffu)!=clock_control) return OMNI_AUDIO_CLOCK_ERROR_STATE_CHANGED;
    return OMNI_AUDIO_CLOCK_ERROR_NONE;
}

/* Reusing a clock requires a successful owned initialization in this boot.
 * Register equality alone does not prove an inherited PLL has settled.
 * Playback teardown restores nominal MD before testing this fast path. */
static inline bool omni_audio_clock_startup_ready(bool owned_and_settled,
    const uint32_t registers[OMNI_AC_REGISTER_COUNT], uint32_t power_state)
{
    if (!owned_and_settled || !registers) return false;
    return (power_state & 0x900300u) == 0u &&
        (registers[OMNI_AC_XO_STATUS] & 1u) != 0u &&
        (registers[OMNI_AC_XO_CTRL] & OMNI_AUDIO_CLOCK_XO_MASK) == 0x01c3459au &&
        (registers[OMNI_AC_PLL0SEL] & 7u) == 1u &&
        (registers[OMNI_AC_PLL0CTRL] & 0x01ffffffu) == 0x00220c40u &&
        (registers[OMNI_AC_PLL0NDEC] & 0xffu) == 4u &&
        (registers[OMNI_AC_PLL0PDEC] & 0x1fu) == 5u &&
        registers[OMNI_AC_PLL0SSCG0] == 0xf5c28f5cu &&
        (registers[OMNI_AC_PLL0SSCG1] & 0x1bfffffdu) == 0u &&
        (registers[OMNI_AC_MCLKSEL] & 7u) == 1u &&
        (registers[OMNI_AC_PLL0DIV] & 0xe00000ffu) == 0u &&
        (registers[OMNI_AC_MCLKDIV] & 0xe00000ffu) == 1u;
}

/* Run the existing guarded sequencer with a single absolute timebase. The
 * callbacks run with the caller's interrupt state unchanged; service feeds the
 * watchdog. Failure keeps the sequencer's exact reason for the caller. */
static inline bool omni_audio_clock_startup_run(omni_audio_clock_t *clock,
    uint32_t observed_xo_control, uint32_t (*milliseconds)(void),
    void (*service)(void))
{
    if (!clock || !milliseconds || !service) return false;
    uint32_t started = milliseconds();
    if (!omni_audio_clock_begin(clock, OMNI_AUDIO_CLOCK_REFERENCE_HZ,
            observed_xo_control, true, started)) return false;
    for (uint32_t polls = 0; polls < OMNI_AUDIO_CLOCK_STARTUP_POLL_LIMIT; ++polls) {
        omni_audio_clock_poll(clock, milliseconds());
        service();
        if (clock->state >= OMNI_AUDIO_CLOCK_LOCAL_READY)
            return clock->state == OMNI_AUDIO_CLOCK_LOCAL_READY;
    }
    clock->error = OMNI_AUDIO_CLOCK_ERROR_TOTAL_TIMEOUT;
    clock->state = OMNI_AUDIO_CLOCK_FAILED;
    return false;
}

#endif
