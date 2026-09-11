#ifndef OMNI_AUDIO_CLOCK_SNAPSHOT_H
#define OMNI_AUDIO_CLOCK_SNAPSHOT_H
#include <stdbool.h>
#include <stdint.h>

#define OMNI_AUDIO_CLOCK_SNAPSHOT_WORDS 40u
/* Fixed read-only version 1 snapshot; null returns false without MMIO.
 * Word 1: bit0 SYSCON valid, bit1 PMC valid, bit2 ANACTRL valid,
 * bit8 gates and analog reset state matched before/after. This is not an
 * atomic analog measurement. Pages obtained separately are separate samples.
 *  0 version, 1 validity, 2 AHBCLKCTRL2 before, 3 AHBCLKCTRL2 after,
 *  4 XO32M_CTRL, 5 XO32M_STATUS before, 6 FRO192M_CTRL, 7 LDO_XO32M,
 *  8 PDRUNCFG0, 9 PDRUNCFG1, 10 CLOCK_CTRL, 11 MAINCLKA, 12 MAINCLKB,
 * 13 AHBDIV, 14 USB0SEL, 15 USB0DIV, 16 PLL0SEL, 17 PLL0CTRL,
 * 18 PLL0STAT, 19 PLL0NDEC, 20 PLL0PDEC, 21 PLL0SSCG0, 22 PLL0SSCG1,
 * 23 PLL0DIV, 24 MCLKSEL, 25 MCLKDIV, 26 MCLKIO, 27 FC0SEL, 28 FC2SEL,
 * 29 AHBCLKCTRL0, 30 AHBCLKCTRL1, 31 PRESETCTRL2 before, 32 FROHFDIV,
 * 33 ADCCLKSEL, 34 SCTCLKSEL, 35 SDIOCLKSEL, 36 FRG0, 37 FRG2,
 * 38 PRESETCTRL2 after, 39 XO32M_STATUS after.
 * ANACTRL fields are zero/invalid if its bus clock is disabled or reset is
 * asserted. No clock enabling, power writes, FIFO reads, or guessed SYSOSCCTRL.
 * Raw oscillator configuration cannot by itself measure input frequency or
 * prove the physical crystal/input mode is correct. */
bool omni_audio_clock_snapshot(uint32_t out[OMNI_AUDIO_CLOCK_SNAPSHOT_WORDS]);
#endif
