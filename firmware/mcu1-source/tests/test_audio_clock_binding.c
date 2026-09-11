#include "board_clock.h"
#include "audio_clock.h"
#include "audio_clock_lpc5528.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Host golden-trace test for the LPC5528 audio-clock backend.
 *
 * It NEVER calls the backend's real volatile store: dereferencing e.g.
 * 0x40000580 on x86 would segfault. Instead it drives the sequencer through
 * its OWN recording ops that translate reg -> table[reg] via the backend's
 * public address table and record (address,value) pairs, then asserts the
 * emitted writes equal the adopted fractional PLL configuration, while keeping
 * the recovered stock XO/divider order. It also exercises the backend's read()
 * rejection of the write-only aliases and guard()'s isolation logic directly,
 * both of which are safe because they short-circuit / use an injected reader. */

static const uint32_t *T; /* backend address table */

/* ---- recording register model that drives the sequencer to LOCAL_READY ---- */
typedef struct {
    uint32_t regs[OMNI_AC_REGISTER_COUNT];
    uint32_t waddr[64], wval[64];
    unsigned nwrites;
    bool xo_ready, pll_lock, div_ready;
} rec_t;

static bool rec_guard(void *ctx) { (void)ctx; return true; }

static bool rec_read(void *ctx, omni_audio_clock_register_t reg, uint32_t *value)
{
    rec_t *r = ctx;
    /* The sequencer must never read the write-only PMC aliases. */
    assert(reg < OMNI_AC_REGISTER_COUNT &&
           reg != OMNI_AC_POWER_SET && reg != OMNI_AC_POWER_CLEAR);
    assert(T[reg] != 0u); /* address translation is defined for every read reg */
    uint32_t v = r->regs[reg];
    if (reg == OMNI_AC_XO_STATUS) v = r->xo_ready ? 1u : 0u;
    if (reg == OMNI_AC_PLL0STAT)  v = r->pll_lock ? 1u : 0u;
    if ((reg == OMNI_AC_PLL0DIV || reg == OMNI_AC_MCLKDIV) && !r->div_ready)
        v |= 0x80000000u;
    *value = v;
    return true;
}

static bool rec_write(void *ctx, omni_audio_clock_register_t reg, uint32_t value)
{
    rec_t *r = ctx;
    assert(reg < OMNI_AC_REGISTER_COUNT);
    assert(T[reg] != 0u);
    assert(r->nwrites < 64u);
    r->waddr[r->nwrites] = T[reg];
    r->wval[r->nwrites] = value;
    r->nwrites++;
    /* PMC set/clear aliases do not hold state; every other write is readable. */
    if (reg != OMNI_AC_POWER_SET && reg != OMNI_AC_POWER_CLEAR) r->regs[reg] = value;
    return true;
}

static void drive_to_ready(rec_t *r, omni_audio_clock_t *clock)
{
    memset(r, 0, sizeof(*r));
    r->xo_ready = r->pll_lock = r->div_ready = true;
    /* Preflight-valid inherited state, mirroring the stock emulator's initials
     * so the emitted XO/CLOCK_CTRL writes match its recorded values exactly. */
    r->regs[OMNI_AC_MAINCLKA]   = OMNI_CORE_MAIN_A;
    r->regs[OMNI_AC_USB0SEL]    = 3u;
    r->regs[OMNI_AC_USB0DIV]    = 1u;
    r->regs[OMNI_AC_XO_CTRL]    = 0x00c3459au; /* emulator initial_xo */
    r->regs[OMNI_AC_CLOCK_CTRL] = 0x03u;       /* emulator initial clock_ctrl */
    /* MAINCLKB, AHBDIV, MCLKIO, FC0SEL, FC2SEL, MCLKSEL stay0. */
    omni_audio_clock_ops_t ops = { r, rec_guard, rec_read, rec_write };
    assert(omni_audio_clock_init(clock, &ops));
    assert(omni_audio_clock_begin(clock, 16000000u,
                                  r->regs[OMNI_AC_XO_CTRL], true, 0u));
    uint32_t now = 0u;
    for (unsigned n = 0; n < 600u && clock->state != OMNI_AUDIO_CLOCK_LOCAL_READY;
         ++n, ++now) {
        assert(clock->state < OMNI_AUDIO_CLOCK_LOCAL_READY);
        omni_audio_clock_poll(clock, now);
    }
    assert(clock->state == OMNI_AUDIO_CLOCK_LOCAL_READY);
}

/* The old golden encoded stock's integer N=25/M=768 configuration and became
 * stale when the adopted firmware moved to fractional N=4/M=122.88. Retain the
 * stock ordering, but assert the chosen fractional values explicitly here.
 * UM11126 tables 121/123/124/126 define the control/divider/latch fields;
 * table 128 and section 4.6.6.3.1 define M*2^25 and Fout=Fin*M/(N*2*P).
 * SELP=3/SELI=4 are the adopted fractional-loop settings, not stock values.
 * These tests validate programming and arithmetic, not analog stability. */
typedef struct { uint32_t addr, val; } wr_t;
static const wr_t golden[] = {
    /* --- XO setup (local: emitted once; stock repeats it, see gap below) --- */
    {0x400200c8u, 0x100u},      /* PMC.PDRUNCFGCLR0        stock#0  */
    {0x400200c8u, 0x100000u},   /* PMC.PDRUNCFGCLR0        stock#1  */
    {0x40000a18u, 0x23u},       /* SYSCON.CLOCK_CTRL       stock#2  */
    {0x40013020u, 0x01c3459au}, /* ANACTRL.XO32M_CTRL      stock#3  */
    /* stock#4,#5,#6 (PDRUNCFGCLR0 x2 + CLOCK_CTRL 0x23) are a duplicate XO
     * prep the local sequencer OMITS -- it configures XO exactly once.       */
    {0x40000290u, 1u},          /* SYSCON.PLL0CLKSEL       stock#7  */
    {0x400200c0u, 0x200u},      /* PMC.PDRUNCFGSET0        stock#8  */
    {0x400200c0u, 0x800000u},   /* PMC.PDRUNCFGSET0        stock#9  */
    {0x40000580u, (3u<<10)|(4u<<4)|(1u<<17)|(1u<<21)}, /* SELP/SELI/LIMUPOFF/CLKEN */
    {0x40000588u, 4u},         /* NDIV=4, Fref=16 MHz/4 */
    {0x40000588u, 4u|(1u<<8)}, /* NDIV + NREQ */
    {0x4000058cu, 0x5u},        /* SYSCON.PLL0PDEC         stock#13 */
    {0x4000058cu, 0x25u},       /* SYSCON.PLL0PDEC (latch)  stock#14 */
    {0x40000590u, 0xf5c28f5cu}, /* floor(122.88 * 2^25) */
    {0x40000594u, 0u},          /* SEL_EXT=0; fractional, no spread spectrum */
    {0x40000594u, (1u<<26)|2u}, /* MREQ + MD_REQ */
    {0x400200c8u, 0x200u},      /* PMC.PDRUNCFGCLR0        stock#18 */
    {0x400200c8u, 0x800000u},   /* PMC.PDRUNCFGCLR0        stock#19 */
    /* --- divider setup (local phase; stock continues beyond writes[:20]) --- */
    {0x400003c4u, 0x20000000u}, /* SYSCON.PLL0CLKDIV (HALT)      */
    {0x400003c4u, 0x40000000u}, /* SYSCON.PLL0CLKDIV (REQ)       */
    {0x400003c4u, 0u},          /* SYSCON.PLL0CLKDIV (divide by 1) */
    {0x400003acu, 1u},          /* SYSCON.MCLKDIV                */
    {0x400002e0u, 1u},          /* SYSCON.MCLKCLKSEL (MCLKDIV/MCLKSEL local order) */
};

/* Read-only / snapshot cross-check: addresses transcribed independently from
 * src/audio_clock_snapshot.c (snapshot word index in the comment). Asserting
 * these equal the backend table double-sources all 24 registers in C; the
 * companion python script does the authoritative automated diff of all three
 * files. POWER_SET/POWER_CLEAR are intentionally absent: the snapshot samples
 * PMC PDRUNCFG0 state (0x400200b8), never the write-only set/clear aliases. */
typedef struct { omni_audio_clock_register_t reg; uint32_t addr; } snap_t;
static const snap_t snap[] = {
    {OMNI_AC_MAINCLKA,     0x40000280u}, /* snap[11] */
    {OMNI_AC_MAINCLKB,     0x40000284u}, /* snap[12] */
    {OMNI_AC_AHBDIV,       0x40000380u}, /* snap[13] */
    {OMNI_AC_USB0SEL,      0x400002a8u}, /* snap[14] */
    {OMNI_AC_USB0DIV,      0x40000398u}, /* snap[15] */
    {OMNI_AC_FRO192M_CTRL, 0x40013010u}, /* snap[6]  */
    {OMNI_AC_XO_CTRL,      0x40013020u}, /* snap[4]  */
    {OMNI_AC_CLOCK_CTRL,   0x40000a18u}, /* snap[10] */
    {OMNI_AC_MCLKIO,       0x40000420u}, /* snap[26] */
    {OMNI_AC_FC0SEL,       0x400002b0u}, /* snap[27] */
    {OMNI_AC_FC2SEL,       0x400002b8u}, /* snap[28] */
    {OMNI_AC_MCLKSEL,      0x400002e0u}, /* snap[24] */
    {OMNI_AC_XO_STATUS,    0x40013024u}, /* snap[5]  */
    {OMNI_AC_PLL0SEL,      0x40000290u}, /* snap[16] */
    {OMNI_AC_PLL0CTRL,     0x40000580u}, /* snap[17] */
    {OMNI_AC_PLL0NDEC,     0x40000588u}, /* snap[19] */
    {OMNI_AC_PLL0PDEC,     0x4000058cu}, /* snap[20] */
    {OMNI_AC_PLL0SSCG0,    0x40000590u}, /* snap[21] */
    {OMNI_AC_PLL0SSCG1,    0x40000594u}, /* snap[22] */
    {OMNI_AC_PLL0STAT,     0x40000584u}, /* snap[18] */
    {OMNI_AC_PLL0DIV,      0x400003c4u}, /* snap[23] */
    {OMNI_AC_MCLKDIV,      0x400003acu}, /* snap[25] */
};

/* ---- controlled MMIO for the guard() isolation checks ---- */
typedef struct {
    uint32_t fc0sel, fc2sel, mclksel, i2s0, i2s2, dma_en, mclkio;
    uint32_t uart0, uart2;
} guardvals_t;

static uint32_t guard_read(void *user, uint32_t address)
{
    guardvals_t *g = user;
    switch (address) {
    case 0x400002b0u: return g->fc0sel;
    case 0x400002b8u: return g->fc2sel;
    case 0x400002e0u: return g->mclksel;
    /* UM11126 tables 681/682 place I2S CFG1 at +0xC00. Deliberately model
     * USART's +0x400 bank separately so reading it cannot pass this guard. */
    case 0x40086c00u: return g->i2s0;
    case 0x40088c00u: return g->i2s2;
    case 0x40086400u: return g->uart0;
    case 0x40088400u: return g->uart2;
    case 0x40082020u: return g->dma_en;
    case 0x40000420u: return g->mclkio;
    default: assert(0); return 0u; /* guard must touch only documented addrs */
    }
}

int main(void)
{
    T = omni_audio_clock_lpc5528_addresses();
    assert(T);

    /* 1. Golden trace: adopted fractional setup in recovered register order. */
    rec_t r;
    omni_audio_clock_t clock = {0};
    drive_to_ready(&r, &clock);
    assert(clock.successful_writes == 22u);
    assert(r.nwrites == sizeof(golden) / sizeof(golden[0]));
    for (unsigned i = 0; i < r.nwrites; ++i) {
        assert(r.waddr[i] == golden[i].addr);
        assert(r.wval[i] == golden[i].val);
    }
    /* Independently derive MD from the requested output frequency and the
     * selected N=4/P=5, rather than only copying a firmware hex constant. */
    const uint64_t q25=UINT64_C(1)<<25;
    const uint64_t md=(UINT64_C(49152000)*4u*2u*5u*q25)/UINT64_C(16000000);
    assert(md==r.regs[OMNI_AC_PLL0SSCG0]);
    assert((r.regs[OMNI_AC_PLL0SSCG1] & ((1u<<28)|1u))==0u);
    const uint64_t requested=UINT64_C(49152000)*4u*2u*5u*q25;
    const uint64_t produced=UINT64_C(16000000)*md;
    assert(requested>=produced && requested-produced<UINT64_C(16000000));

    /* 2. All 24 addresses double-sourced against the snapshot layout. */
    for (unsigned i = 0; i < sizeof(snap) / sizeof(snap[0]); ++i)
        assert(T[snap[i].reg] == snap[i].addr);
    /* The two write-only aliases are the documented single-source exceptions. */
    assert(T[OMNI_AC_POWER_SET] == 0x400200c0u);
    assert(T[OMNI_AC_POWER_CLEAR] == 0x400200c8u);

    /* 3. Backend read() rejects the write-only aliases without dereferencing. */
    omni_audio_clock_lpc5528_t noio = {0, 0, 0};
    omni_audio_clock_ops_t ops = omni_audio_clock_lpc5528_ops(&noio);
    uint32_t sentinel = 0xdeadbeefu;
    assert(!ops.read(ops.context, OMNI_AC_POWER_SET, &sentinel));
    assert(!ops.read(ops.context, OMNI_AC_POWER_CLEAR, &sentinel));
    assert(sentinel == 0xdeadbeefu); /* left untouched */

    /* 4. guard(): true only when every downstream consumer is isolated. */
    guardvals_t g;
    omni_audio_clock_lpc5528_t gb = {guard_read, 0, &g};
    omni_audio_clock_ops_t gops = omni_audio_clock_lpc5528_ops(&gb);

    memset(&g, 0, sizeof(g));
    assert(gops.guard(gops.context)); /* fully isolated -> owned */

    memset(&g, 0, sizeof(g)); g.fc0sel = 1u;  assert(!gops.guard(gops.context));
    memset(&g, 0, sizeof(g)); g.fc2sel = 1u;  assert(!gops.guard(gops.context));
    memset(&g, 0, sizeof(g)); g.i2s0 = 1u;    assert(!gops.guard(gops.context));
    memset(&g, 0, sizeof(g)); g.i2s2 = 1u;    assert(!gops.guard(gops.context));
    /* Active I2S with an idle USART bank must reject PLL ownership; enabled
     * USART-bank bits with disabled I2S must not be mistaken for audio use. */
    memset(&g, 0, sizeof(g)); g.uart0 = 1u; g.uart2 = 1u;
    assert(gops.guard(gops.context));
    memset(&g, 0, sizeof(g)); g.dma_en = 1u << 4;  assert(!gops.guard(gops.context));
    memset(&g, 0, sizeof(g)); g.dma_en = 1u << 11; assert(!gops.guard(gops.context));
    memset(&g, 0, sizeof(g)); g.mclkio = 1u;  assert(!gops.guard(gops.context));
    /* SYSCON.MCLKCLKSEL == PLL0(1) is the module's OWN selector -- exempt after
     * setup per audio_clock.h -- so guard() must NOT reject it. */
    memset(&g, 0, sizeof(g)); g.mclksel = 1u; assert(gops.guard(gops.context));
    /* selector value 1 is rejected only for the external FCn selectors; a
     * non-PLL0 FCn selection (and any MCLK selection) passes. */
    memset(&g, 0, sizeof(g)); g.fc0sel = 2u; g.mclksel = 7u;
    assert(gops.guard(gops.context));

    printf("{\"passed\":true,\"writes\":%u,\"registers\":%u}\n",
           r.nwrites, (unsigned)OMNI_AC_REGISTER_COUNT);
    return 0;
}
