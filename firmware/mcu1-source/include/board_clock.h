#ifndef OMNI_BOARD_CLOCK_H
#define OMNI_BOARD_CLOCK_H
/* One supported CPU96 candidate; USB FRO96/2 and peripheral FRO12 remain unchanged. */
#define OMNI_CORE_CLOCK_HZ 96000000U
#define OMNI_CORE_CLOCK_MHZ (OMNI_CORE_CLOCK_HZ / 1000000U)
#define OMNI_CORE_MAIN_A 3U
#define OMNI_CORE_TICKS_PER_MS (OMNI_CORE_CLOCK_HZ / 1000U)
/* Retain conservative iteration headroom across the 8x CPU transition.
 * These are failure/observation caps, never calibrated time delays. */
#define OMNI_CPU_GUARD_ITERATIONS(old_count) ((old_count) * 8U)
_Static_assert(OMNI_CORE_CLOCK_HZ <= 100000000U, "ROM flash programming clock limit");
_Static_assert(OMNI_CORE_TICKS_PER_MS <= 0x1000000U, "SysTick reload range");
#endif
