#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Characterization of the LPC3511 EPINUSE unrelated-endpoint selector race and
 * of the candidate mitigations (USB-OFFLINE-AUDIT-2026-09-09.md section 1).
 *
 * EPINUSE holds one hardware buffer-selector bit per physical endpoint. Hardware
 * SETS an endpoint's bit when it completes that endpoint's active buffer; software
 * may also write the register. The pinned vendor driver's endpoint init/deinit
 * (and the double-buffer cancel paths) clear one selector with a WHOLE-REGISTER
 * read-modify-write:  EPINUSE &= ~(1u << endpointIndex);
 * Disabling CPU interrupts does not stop USB hardware from setting a DIFFERENT
 * endpoint's selector bit between that read and that write. The stale write-back
 * then clobbers the hardware update for the unrelated endpoint.
 *
 * Physical selector bits used here: EP 0x82 (IN ep2) = bit 5; EP 0x83 (IN ep3) =
 * bit 7 -- matching the audit (deinit of 0x83 clears bit 7; 0x82 is the bystander).
 *
 * IMPORTANT SCOPE. This is a MODEL of the register interleaving only. It does NOT
 * prove the recorded notification/microphone faults were caused this way (that
 * remains hardware-gated: it needs a duplex-stress capture of EPINUSE selecting an
 * inactive buffer at the fault instant). Nothing here is firmware source, is wired
 * into any build, or is flash-eligible. The pinned vendor driver is not in this
 * repo; this models the pattern its whole-register write exhibits and the shape of
 * the section-1 "seed from hardware / no whole-register store" correction. */

#define EP82_BIT 5u /* the bystander endpoint hardware is completing */
#define EP83_BIT 7u /* the endpoint being deinit/reinit/canceled     */

typedef struct {
    uint32_t epinuse;
    bool hw_completion_armed; /* EP82 will complete and set its selector bit */
    bool hw_fired;
    unsigned reads, writes;
} epinuse_t;

static uint32_t rd(epinuse_t *m) { ++m->reads; return m->epinuse; }

/* Worst-case window: hardware completes EP82 in the gap immediately before the
 * CPU's write commits, so the CPU value (computed from an earlier read that did
 * not observe the completion) overwrites it. This is the hazard the audit shows;
 * it is independent of how many times the CPU re-read first. */
static void wr(epinuse_t *m, uint32_t v)
{
    if (m->hw_completion_armed && !m->hw_fired) { m->epinuse |= (1u << EP82_BIT); m->hw_fired = true; }
    m->epinuse = v;
    ++m->writes;
}

/* If the strategy performed no write at all, the hardware completion still lands. */
static void hw_settle(epinuse_t *m)
{
    if (m->hw_completion_armed && !m->hw_fired) { m->epinuse |= (1u << EP82_BIT); m->hw_fired = true; }
}

/* Vendor pattern: whole-register masked RMW. */
static void strat_whole_rmw(epinuse_t *m, unsigned idx)
{
    uint32_t v = rd(m);
    v &= ~(1u << idx);
    wr(m, v);
}

/* The tempting non-fix: re-read immediately before the write, then masked-clear.
 * There is STILL a read->write gap (no bit-band / no peripheral exclusive monitor
 * on M33/LPC55 to make it atomic), so it narrows but does not close the race. */
static void strat_reread_masked(epinuse_t *m, unsigned idx)
{
    (void)rd(m);
    uint32_t v = rd(m);
    v &= ~(1u << idx);
    wr(m, v);
}

/* Section-1 correction: seed the endpoint's software producer/consumer selector
 * from its CURRENT hardware selector and keep it; perform NO whole-register store
 * for the live unrelated endpoint. (Single-buffering with a matching driver
 * configuration is the other listed correction; both avoid the stale write-back.) */
static uint8_t seeded_selector;
static void strat_seed_from_hw(epinuse_t *m, unsigned idx)
{
    uint32_t v = rd(m);
    seeded_selector = (uint8_t)((v >> idx) & 1u); /* align software to hardware */
    /* no wr(): the register is not modified, so no other endpoint's bit is lost */
}

static uint32_t run(void (*strat)(epinuse_t *, unsigned), unsigned idx, uint32_t initial, bool race)
{
    epinuse_t m = {.epinuse = initial, .hw_completion_armed = race, .hw_fired = false, .reads = 0, .writes = 0};
    strat(&m, idx);
    hw_settle(&m);
    return m.epinuse;
}

int main(void)
{
    const uint32_t init = (1u << EP83_BIT); /* EP83 selector set; EP82 bit5 = 0 (pre-completion) */

    /* --- With the race: EP82 completes in the deinit's read->write window --- */
    /* Vendor whole-register RMW loses EP82's just-set selector bit. */
    assert((run(strat_whole_rmw, EP83_BIT, init, true) & (1u << EP82_BIT)) == 0u);      /* CLOBBERED */
    /* Re-read-then-masked-clear STILL loses it. */
    assert((run(strat_reread_masked, EP83_BIT, init, true) & (1u << EP82_BIT)) == 0u);  /* CLOBBERED */
    /* Seed-from-hardware (no whole-register store) PRESERVES it. */
    assert((run(strat_seed_from_hw, EP83_BIT, init, true) & (1u << EP82_BIT)) != 0u);   /* PRESERVED */
    assert(seeded_selector == 1u); /* it seeded EP83's own (hardware) selector */
    /* The whole-register variants still clear the intended EP83 bit. */
    assert((run(strat_whole_rmw, EP83_BIT, init, true) & (1u << EP83_BIT)) == 0u);
    assert((run(strat_reread_masked, EP83_BIT, init, true) & (1u << EP83_BIT)) == 0u);

    /* --- Without the race: EP82 completes BEFORE the deinit reads --- */
    const uint32_t pre = init | (1u << EP82_BIT);
    assert((run(strat_whole_rmw, EP83_BIT, pre, false) & (1u << EP82_BIT)) != 0u);
    assert((run(strat_reread_masked, EP83_BIT, pre, false) & (1u << EP82_BIT)) != 0u);
    assert((run(strat_seed_from_hw, EP83_BIT, pre, false) & (1u << EP82_BIT)) != 0u);

    /* --- Same hazard on init/reinit and cancel-double-buffer, which also
     * whole-register-RMW EPINUSE (here modelled by clearing a different index):
     * the whole-register write clobbers a racing EP82 completion, seed does not. */
    assert((run(strat_whole_rmw, 1u, init, true) & (1u << EP82_BIT)) == 0u);
    assert((run(strat_seed_from_hw, 1u, init, true) & (1u << EP82_BIT)) != 0u);

    printf("usb_selector: EPINUSE whole-register RMW (and re-read masked) race-clobber an "
           "unrelated endpoint selector; seed-from-hardware does not. MODEL only; fault "
           "causation is hardware-gated; nothing here is wired or flash-eligible. PASS\n");
    return 0;
}
