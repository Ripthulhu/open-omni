#include "audio_clock_lpc5528.h"
#include <stddef.h>

/* Single source of truth. Each enum member -> recovered LPC5528 MMIO address
 * (UM11126 register name in the comment). Every address is double-sourced from
 * firmware/rebuild-re/emulate_stock_audio_clock.py (stock write NAMES, the
 * protected-source dict, or the read hooks) AND src/audio_clock_snapshot.c
 * (snapshot word index), except the two write-only PMC aliases POWER_SET /
 * POWER_CLEAR, which the snapshot never samples (it reads PDRUNCFG0 state
 * 0x400200b8 instead) and are therefore single-sourced from the stock NAMES.
 *
 * Kept as SDK-free plain const data (no fsl_device_registers.h) so the host
 * test can compile and diff it. Designated initializers keep the mapping
 * correct even if the enum is reordered. */
static const uint32_t table[OMNI_AC_REGISTER_COUNT] = {
    [OMNI_AC_MAINCLKA]     = 0x40000280u, /* SYSCON.MAINCLKSELA   snap[11] */
    [OMNI_AC_MAINCLKB]     = 0x40000284u, /* SYSCON.MAINCLKSELB   snap[12] */
    [OMNI_AC_AHBDIV]       = 0x40000380u, /* SYSCON.AHBCLKDIV     snap[13] */
    [OMNI_AC_USB0SEL]      = 0x400002a8u, /* SYSCON.USB0CLKSEL    snap[14] */
    [OMNI_AC_USB0DIV]      = 0x40000398u, /* SYSCON.USB0CLKDIV    snap[15] */
    [OMNI_AC_FRO192M_CTRL] = 0x40013010u, /* ANACTRL.FRO192M_CTRL snap[6]  */
    [OMNI_AC_XO_CTRL]      = 0x40013020u, /* ANACTRL.XO32M_CTRL   snap[4]  */
    [OMNI_AC_CLOCK_CTRL]   = 0x40000a18u, /* SYSCON.CLOCK_CTRL    snap[10] */
    [OMNI_AC_MCLKIO]       = 0x40000420u, /* SYSCON.MCLKIO        snap[26] */
    [OMNI_AC_FC0SEL]       = 0x400002b0u, /* SYSCON.FCCLKSEL0     snap[27] */
    [OMNI_AC_FC2SEL]       = 0x400002b8u, /* SYSCON.FCCLKSEL2     snap[28] */
    [OMNI_AC_MCLKSEL]      = 0x400002e0u, /* SYSCON.MCLKCLKSEL    snap[24] */
    [OMNI_AC_XO_STATUS]    = 0x40013024u, /* ANACTRL.XO32M_STATUS snap[5]  */
    [OMNI_AC_PLL0SEL]      = 0x40000290u, /* SYSCON.PLL0CLKSEL    snap[16] */
    [OMNI_AC_PLL0CTRL]     = 0x40000580u, /* SYSCON.PLL0CTRL      snap[17] */
    [OMNI_AC_PLL0NDEC]     = 0x40000588u, /* SYSCON.PLL0NDEC      snap[19] */
    [OMNI_AC_PLL0PDEC]     = 0x4000058cu, /* SYSCON.PLL0PDEC      snap[20] */
    [OMNI_AC_PLL0SSCG0]    = 0x40000590u, /* SYSCON.PLL0SSCG0     snap[21] */
    [OMNI_AC_PLL0SSCG1]    = 0x40000594u, /* SYSCON.PLL0SSCG1     snap[22] */
    [OMNI_AC_PLL0STAT]     = 0x40000584u, /* SYSCON.PLL0STAT      snap[18] */
    [OMNI_AC_PLL0DIV]      = 0x400003c4u, /* SYSCON.PLL0CLKDIV    snap[23] */
    [OMNI_AC_MCLKDIV]      = 0x400003acu, /* SYSCON.MCLKDIV       snap[25] */
    [OMNI_AC_POWER_SET]    = 0x400200c0u, /* PMC.PDRUNCFGSET0  write-only alias */
    [OMNI_AC_POWER_CLEAR]  = 0x400200c8u, /* PMC.PDRUNCFGCLR0  write-only alias */
};

const uint32_t *omni_audio_clock_lpc5528_addresses(void)
{
    return table;
}

uint32_t omni_audio_clock_lpc5528_read_mmio(void *user, uint32_t address)
{
    (void)user;
    return *(volatile const uint32_t *)(uintptr_t)address;
}

void omni_audio_clock_lpc5528_write_mmio(void *user, uint32_t address, uint32_t value)
{
    (void)user;
    *(volatile uint32_t *)(uintptr_t)address = value;
}

static uint32_t (*resolve_read(const omni_audio_clock_lpc5528_t *b))(void *, uint32_t)
{
    return b->mmio_read ? b->mmio_read : omni_audio_clock_lpc5528_read_mmio;
}
static void (*resolve_write(const omni_audio_clock_lpc5528_t *b))(void *, uint32_t, uint32_t)
{
    return b->mmio_write ? b->mmio_write : omni_audio_clock_lpc5528_write_mmio;
}

static bool ac_read(void *context, omni_audio_clock_register_t reg, uint32_t *value)
{
    /* PMC write-one aliases must never be read back (they read as zero on real
     * hardware and are meaningless): the sequencer contract forbids it. */
    if (reg == OMNI_AC_POWER_SET || reg == OMNI_AC_POWER_CLEAR) return false;
    if (reg >= OMNI_AC_REGISTER_COUNT || !value) return false;
    omni_audio_clock_lpc5528_t *b = context;
    *value = resolve_read(b)(b->user, table[reg]);
    return true;
}

static bool ac_write(void *context, omni_audio_clock_register_t reg, uint32_t value)
{
    if (reg >= OMNI_AC_REGISTER_COUNT) return false;
    omni_audio_clock_lpc5528_t *b = context;
    resolve_write(b)(b->user, table[reg], value);
    return true;
}

/* Real runtime isolation check (not a software flag). Reads the live selectors
 * and downstream-consumer enables directly, and refuses ownership if any
 * consumer of PLL0/MCLK or any audio datapath is active. Extra addresses (not
 * in the sequencer's enum) are documented with their UM11126 name. */
static bool ac_guard(void *context)
{
    omni_audio_clock_lpc5528_t *b = context;
    uint32_t (*rd)(void *, uint32_t) = resolve_read(b);
    void *u = b->user;

    /* Flexcomm0 / Flexcomm2 audio clock selectors must not source PLL0
     * (selector field value 1) -- no external audio Flexcomm consumer may be fed
     * from the PLL we are about to own. SYSCON.FCCLKSEL0 / FCCLKSEL2.
     * SYSCON.MCLKCLKSEL is deliberately NOT checked: the sequencer owns MCLK and
     * routes PLL0 -> MCLK during DIV_SETUP, and audio_clock.h exempts the
     * module's own MCLK selector after setup -- guarding it here would reject the
     * very state the sequence establishes and fail the run at DIV_SETUP. */
    if ((rd(u, 0x400002b0u) & 7u) == 1u) return false; /* SYSCON.FCCLKSEL0 */
    if ((rd(u, 0x400002b8u) & 7u) == 1u) return false; /* SYSCON.FCCLKSEL2 */

    /* I2S0 (Flexcomm0, 0x40086000) and I2S2 (Flexcomm2, 0x40088000) must be
     * stopped: CFG1.MAINENABLE (bit 0) clear. UM11126 section 36.7 tables 681/682
     * locate I2S CFG1 at Flexcomm base + 0xC00; +0x400 is the USART CFG bank.
     * Stock init stores at 0x2326C/0x23688 independently confirm these addresses. */
    if (rd(u, 0x40086c00u) & 1u) return false; /* FLEXCOMM0 I2S0 CFG1.MAINENABLE */
    if (rd(u, 0x40088c00u) & 1u) return false; /* FLEXCOMM2 I2S2 CFG1.MAINENABLE */

    /* DMA0 must have the two I2S DMA channels (4 and 11) disabled.
     * DMA0.ENABLESET0 = DMA base 0x40082000 + 0x020. */
    if (rd(u, 0x40082020u) & ((1u << 4) | (1u << 11))) return false; /* DMA0.ENABLESET0 */

    /* MCLKIO must remain an input (DIR bit0 == 0) so the pin is isolated. */
    if (rd(u, 0x40000420u) & 1u) return false; /* SYSCON.MCLKIO */

    return true;
}

omni_audio_clock_ops_t omni_audio_clock_lpc5528_ops(omni_audio_clock_lpc5528_t *backend)
{
    omni_audio_clock_ops_t ops;
    ops.context = backend;
    ops.guard = ac_guard;
    ops.read = ac_read;
    ops.write = ac_write;
    return ops;
}
