#include "board_clock.h"
#include "audio_clock.h"
#include <stddef.h>
#include <string.h>

#define XO_SYSTEM_OUTPUT 0x01000000u
#define DIV_REQFLAG 0x80000000u
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
typedef struct { omni_audio_clock_register_t reg; uint32_t value; } write_t;

/* PLL0 brought up in FRACTIONAL (sigma-delta) mode (SEL_EXT=0) so the audio clock can be
 * fine-steered glitchlessly at runtime by a single PLL0SSCG0 write (USB sync mode / clock
 * recovery, per NXP's usb_device_audio_speaker for this chip). Output is unchanged: 16 MHz
 * XO32M / N=4 -> Fref=4 MHz (in the 3-5 MHz SS window); M_eff=122.88 (MD=0xF5C28F5C=122.88*2^25)
 * -> Fcco=491.52 MHz; P=5 -> 49.152 MHz -> /2 MCLK 24.576 MHz. Loop filter SELP=3/SELI=4/
 * LIMUPOFF=1 (PLL0CTRL 0x00220C40) is NXP's fixed fractional-mode value. Previous integer
 * config was N=25/M=768 (PLL0CTRL 0x00207CA0), which can't be steered without a relock.
 * All SDK-verified. Downstream (MCLK, DSP) sees the identical 24.576 MHz. */
static const write_t pll_setup[] = {
    {OMNI_AC_PLL0SEL,1u},
    {OMNI_AC_POWER_SET,0x200u},{OMNI_AC_POWER_SET,0x800000u},
    {OMNI_AC_PLL0CTRL,0x00220c40u},
    {OMNI_AC_PLL0NDEC,0x04u},{OMNI_AC_PLL0NDEC,0x104u},
    {OMNI_AC_PLL0PDEC,5u},{OMNI_AC_PLL0PDEC,0x25u},
    {OMNI_AC_PLL0SSCG0,0xf5c28f5cu},
    {OMNI_AC_PLL0SSCG1,0u},{OMNI_AC_PLL0SSCG1,0x04000002u},
    {OMNI_AC_POWER_CLEAR,0x200u},{OMNI_AC_POWER_CLEAR,0x800000u}
};
/* Nominal fractional multiplier word (49.152 MHz). Steering writes PLL0SSCG0 = this +/- delta;
 * MD_int=122 (<128) so +/-<=0.4% stays inside SSCG0 with no carry into SSCG1. */
#define OMNI_AC_PLL0_MD_NOMINAL 0xf5c28f5cu
static const write_t div_setup[] = {
    {OMNI_AC_PLL0DIV,0x20000000u},{OMNI_AC_PLL0DIV,0x40000000u},
    {OMNI_AC_PLL0DIV,0u},{OMNI_AC_MCLKDIV,1u},{OMNI_AC_MCLKSEL,1u}
};
typedef struct { omni_audio_clock_register_t reg; uint32_t expected, mask; } verify_t;
static const verify_t fractional_verify[] = {
    {OMNI_AC_PLL0SEL,1u,7u},
    {OMNI_AC_PLL0CTRL,0x00220c40u,0x01ffffffu},
    {OMNI_AC_PLL0NDEC,4u,0xffu},
    {OMNI_AC_PLL0PDEC,5u,0x1fu},
    {OMNI_AC_PLL0SSCG0,OMNI_AC_PLL0_MD_NOMINAL,0xffffffffu},
    /* Ratio/mode fields must match; request/acknowledge latch bits may differ. */
    {OMNI_AC_PLL0SSCG1,0u,0x1bfffffdu},
    {OMNI_AC_XO_STATUS,1u,1u},
    /* Preserve observed lock status for diagnostics, without trusting it as
     * a fractional-mode readiness criterion. */
    {OMNI_AC_PLL0STAT,0u,0u}
};

static void fail(omni_audio_clock_t *clock, omni_audio_clock_error_t error)
{
    clock->error=error; clock->state=OMNI_AUDIO_CLOCK_FAILED;
}
static bool read_reg(omni_audio_clock_t *clock, omni_audio_clock_register_t reg,
                     uint32_t *value)
{
    if (clock->ops.read(clock->ops.context,reg,value)) return true;
    fail(clock,OMNI_AUDIO_CLOCK_ERROR_IO); return false;
}
static bool write_reg(omni_audio_clock_t *clock, write_t operation)
{
    if (!clock->ops.write(clock->ops.context,operation.reg,operation.value)) {
        fail(clock,OMNI_AUDIO_CLOCK_ERROR_IO); return false;
    }
    ++clock->successful_writes; return true;
}
static void next(omni_audio_clock_t *clock, omni_audio_clock_state_t state, uint32_t now)
{
    clock->state=state; clock->step=0; clock->phase_ms=now;
}
bool omni_audio_clock_init(omni_audio_clock_t *clock, const omni_audio_clock_ops_t *ops)
{
    if (!clock || !ops || !ops->guard || !ops->read || !ops->write) return false;
    memset(clock,0,sizeof(*clock)); clock->ops=*ops; clock->initialized=true; return true;
}
bool omni_audio_clock_begin(omni_audio_clock_t *clock, uint32_t reference_hz,
                            uint32_t observed_xo_control, bool oscillator_verified,
                            uint32_t now_ms)
{
    if (!clock || !clock->initialized || clock->state!=OMNI_AUDIO_CLOCK_IDLE) return false;
    if (!oscillator_verified || reference_hz!=OMNI_AUDIO_CLOCK_REFERENCE_HZ) {
        fail(clock,OMNI_AUDIO_CLOCK_ERROR_PREREQUISITE); return false;
    }
    clock->expected_xo=observed_xo_control&OMNI_AUDIO_CLOCK_XO_MASK;
    clock->started_ms=now_ms; next(clock,OMNI_AUDIO_CLOCK_PREFLIGHT,now_ms); return true;
}
static bool preflight_valid(const omni_audio_clock_t *clock)
{
    /* Conservative support for board.c's existing CPU/USB setup. Divider
     * RESET/HALT/REQFLAG must also be clear. Reserved selector bits ignored. */
    return (clock->initial[OMNI_AC_MAINCLKA]&7u)==OMNI_CORE_MAIN_A &&
        (clock->initial[OMNI_AC_MAINCLKB]&3u)==0u &&
        (clock->initial[OMNI_AC_AHBDIV]&0xe00000ffu)==0u &&
        (clock->initial[OMNI_AC_USB0SEL]&7u)==3u &&
        (clock->initial[OMNI_AC_USB0DIV]&0xe00000ffu)==1u &&
        (clock->initial[OMNI_AC_XO_CTRL]&OMNI_AUDIO_CLOCK_XO_MASK)==clock->expected_xo &&
        (clock->initial[OMNI_AC_MCLKIO]&1u)==0u &&
        (clock->initial[OMNI_AC_FC0SEL]&7u)!=1u &&
        (clock->initial[OMNI_AC_FC2SEL]&7u)!=1u &&
        (clock->initial[OMNI_AC_MCLKSEL]&7u)!=1u;
}
void omni_audio_clock_poll(omni_audio_clock_t *clock, uint32_t now_ms)
{
    if (!clock || !clock->initialized || clock->state==OMNI_AUDIO_CLOCK_IDLE ||
        clock->state>=OMNI_AUDIO_CLOCK_LOCAL_READY) return;
    if ((uint32_t)(now_ms-clock->started_ms)>=OMNI_AUDIO_CLOCK_TOTAL_TIMEOUT_MS) {
        fail(clock,OMNI_AUDIO_CLOCK_ERROR_TOTAL_TIMEOUT); return;
    }
    if (!clock->ops.guard(clock->ops.context)) {
        fail(clock,OMNI_AUDIO_CLOCK_ERROR_OWNERSHIP); return;
    }
    uint32_t value=0;
    switch (clock->state) {
    case OMNI_AUDIO_CLOCK_PREFLIGHT:
        if (!read_reg(clock,(omni_audio_clock_register_t)clock->step,&value)) return;
        clock->initial[clock->step]=value;
        if (++clock->step==COUNT(clock->initial)) {
            if (!preflight_valid(clock)) { fail(clock,OMNI_AUDIO_CLOCK_ERROR_PREREQUISITE); return; }
            next(clock,OMNI_AUDIO_CLOCK_XO_SETUP,now_ms);
        }
        break;
    case OMNI_AUDIO_CLOCK_XO_SETUP: {
        /* Preserve every documented XO field, including USB output, mode and
         * trim. Reserved readback bits are not copied into these writes. */
        const write_t setup[]={
            {OMNI_AC_POWER_CLEAR,0x100u},{OMNI_AC_POWER_CLEAR,0x100000u},
            {OMNI_AC_CLOCK_CTRL,(clock->initial[OMNI_AC_CLOCK_CTRL]&0x3ffu)|0x20u},
            {OMNI_AC_XO_CTRL,clock->expected_xo|XO_SYSTEM_OUTPUT}
        };
        if (!write_reg(clock,setup[clock->step])) return;
        if (++clock->step==COUNT(setup)) next(clock,OMNI_AUDIO_CLOCK_WAIT_XO,now_ms);
        break;
    }
    case OMNI_AUDIO_CLOCK_WAIT_XO:
        if ((uint32_t)(now_ms-clock->phase_ms)>=OMNI_AUDIO_CLOCK_XO_TIMEOUT_MS) {
            fail(clock,OMNI_AUDIO_CLOCK_ERROR_XO_TIMEOUT); return;
        }
        if (!read_reg(clock,OMNI_AC_XO_STATUS,&value)) return;
        clock->last_status=value;
        if (value&1u) next(clock,OMNI_AUDIO_CLOCK_PLL_SETUP,now_ms);
        break;
    case OMNI_AUDIO_CLOCK_PLL_SETUP:
        if (!write_reg(clock,pll_setup[clock->step])) return;
        if (++clock->step==COUNT(pll_setup)) next(clock,OMNI_AUDIO_CLOCK_WAIT_PLL,now_ms);
        break;
    case OMNI_AUDIO_CLOCK_WAIT_PLL:
        /* Start timing on a subsequent poll: the final power-up store has
         * already returned. Its own poll timestamp could precede a lengthy
         * interrupt, so using that timestamp could shorten real settling. */
        if (clock->step==0u) { clock->phase_ms=now_ms; clock->step=1u; }
        if ((uint32_t)(now_ms-clock->phase_ms)>=OMNI_AUDIO_CLOCK_PLL_TIMEOUT_MS) {
            fail(clock,OMNI_AUDIO_CLOCK_ERROR_PLL_TIMEOUT); return;
        }
        if (!read_reg(clock,OMNI_AC_PLL0STAT,&value)) return;
        clock->last_status=value;
        /* UM11126 4.5.69.1.2 warns LOCK is unreliable with fractional enabled.
         * Section 4.6.6.2.1 and pinned SDK CLOCK_SetupPLL0Prec (SEL_EXT==0)
         * require 6 ms settling instead. Seven integer-ms ticks guarantee that
         * interval despite timestamp quantization. Source/configuration and
         * XO readiness are verified separately before publishing LOCAL_READY. */
        if ((uint32_t)(now_ms-clock->phase_ms)>=OMNI_AUDIO_CLOCK_FRACTIONAL_SETTLE_MS)
            next(clock,OMNI_AUDIO_CLOCK_DIV_SETUP,now_ms);
        break;
    case OMNI_AUDIO_CLOCK_DIV_SETUP:
        if (!write_reg(clock,div_setup[clock->step])) return;
        if (++clock->step==COUNT(div_setup)) next(clock,OMNI_AUDIO_CLOCK_WAIT_DIV,now_ms);
        break;
    case OMNI_AUDIO_CLOCK_WAIT_DIV:
        if ((uint32_t)(now_ms-clock->phase_ms)>=OMNI_AUDIO_CLOCK_DIV_TIMEOUT_MS) {
            fail(clock,OMNI_AUDIO_CLOCK_ERROR_DIV_TIMEOUT); return;
        }
        if (!read_reg(clock,clock->step?OMNI_AC_MCLKDIV:OMNI_AC_PLL0DIV,&value)) return;
        clock->last_status=value;
        if (value&DIV_REQFLAG) { clock->step=0; return; }
        if ((value&0x600000ffu)!=(uint32_t)clock->step) {
            clock->mismatch_reg=(uint32_t)(clock->step?OMNI_AC_MCLKDIV:OMNI_AC_PLL0DIV);
            clock->mismatch_expected=clock->step;
            clock->mismatch_actual=value; clock->mismatch_mask=0x600000ffu;
            fail(clock,OMNI_AUDIO_CLOCK_ERROR_STATE_CHANGED); return;
        }
        if (++clock->step==2u) next(clock,OMNI_AUDIO_CLOCK_VERIFY,now_ms);
        break;
    case OMNI_AUDIO_CLOCK_VERIFY: {
        omni_audio_clock_register_t reg=(omni_audio_clock_register_t)clock->step;
        uint32_t expected=clock->step<12u?clock->initial[clock->step]:0u;
        uint32_t mask=0xffffffffu;
        if (clock->step>=12u) {
            const verify_t *v=&fractional_verify[clock->step-12u];
            reg=v->reg; expected=v->expected; mask=v->mask;
        }
        if (!read_reg(clock,reg,&value)) return;
        if (reg==OMNI_AC_FRO192M_CTRL) {
            /* UM11126 11.5.4/Table279: with USBCLKADJ=1, DAC_TRIM[23:16]
             * changes autonomously with USB SOF; USBMODCHG[25] is live status.
             * Protect output enables14/30, mandatory Flash bit15 and auto-
             * adjustment selection24. Protect DAC_TRIM only in manual mode.
             * WRTRIM31 is a write command; undefined reserved bits are not
             * configuration. No FRO register is written by this sequencer. */
            mask=0x4100c000u;
            if (!(expected&(1u<<24))) mask|=0x00ff0000u;
        }
        else if (reg==OMNI_AC_XO_CTRL) { mask=OMNI_AUDIO_CLOCK_XO_MASK; expected|=XO_SYSTEM_OUTPUT; }
        else if (reg==OMNI_AC_CLOCK_CTRL) { mask=0x3ffu; expected|=0x20u; }
        else if (reg==OMNI_AC_MCLKSEL) { mask=7u; expected=1u; }
        else if (reg==OMNI_AC_MCLKIO) mask=1u;
        if (reg==OMNI_AC_PLL0STAT) clock->last_status=value;
        if ((value&mask)!=(expected&mask)) {
            clock->mismatch_reg=(uint32_t)reg; clock->mismatch_expected=expected;
            clock->mismatch_actual=value; clock->mismatch_mask=mask;
            fail(clock,OMNI_AUDIO_CLOCK_ERROR_STATE_CHANGED); return;
        }
        if (++clock->step==12u+COUNT(fractional_verify)) next(clock,OMNI_AUDIO_CLOCK_LOCAL_READY,now_ms);
        break;
    }
    default: fail(clock,OMNI_AUDIO_CLOCK_ERROR_STATE_CHANGED); break;
    }
}
void omni_audio_clock_cancel(omni_audio_clock_t *clock)
{
    if (clock && clock->initialized && clock->state>OMNI_AUDIO_CLOCK_IDLE &&
        clock->state<OMNI_AUDIO_CLOCK_LOCAL_READY) clock->state=OMNI_AUDIO_CLOCK_CANCELED;
}
