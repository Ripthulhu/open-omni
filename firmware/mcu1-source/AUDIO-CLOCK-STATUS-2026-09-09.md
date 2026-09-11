# Inactive audio clock preparation and passive snapshot

`audio_clock.c/.h` supplies a source-owned, fixed-register clock sequencer through bounded callbacks. It has no live MMIO binding and is not called by the firmware. Physical activation remains blocked: the recovered stock routine inherits oscillator SLAVE/bypass/trim fields, and its 16 MHz software constant does not establish this board's physical crystal versus driven-input arrangement or actual reference frequency. A valid register capture helps preserve inherited configuration but does not alone resolve those physical facts.

Before any future activation, the caller must independently validate the 16 MHz reference and captured XO mode/trim, establish exclusive PLL0/MCLK ownership, disconnect every downstream PLL0 consumer, isolate external audio pins, stop I2S/DMA, and enable access clocks. The guard must continue validating ownership throughout execution. The component rejects unknown oscillator configuration, unsupported reference frequency, a changed captured XO configuration, inherited active PLL0 consumers on FC0/FC2/MCLK, enabled MCLK output, and CPU/USB routing other than the current FRO12 CPU / FRO96 USB0 setup.

Each poll performs one guard callback and at most one register callback. It powers the external oscillator/LDO through write-one aliases, preserves all documented XO mode/trim/USB-output fields, enables only the system output and CLKIN route, and requires XO_READY. It then performs the instruction-proven PLL0 N/P/M latch sequence for nominal 49.152 MHz: N=25, M=768, P=5 from a 16 MHz reference. The PLL0 divider is /1 and prepared MCLK is /2, nominal 24.576 MHz. FC0/FC2 sources, shared-clock routing, MCLK pin-output enable, pin mux, DMA and I2S are intentionally outside this component and remain untouched.

The limits are source policy: 100 ms XO readiness, 20 ms PLL lock, 20 ms divider readiness, and 500 ms overall. PLL lock is mandatory; a two-millisecond minimum also exceeds the pinned SDK's conservative 500 us + 400/Fref interval at Fref=640 kHz. All waits return to the caller and handle timer wrap. Final checks verify protected CPU/USB/FRO state, oscillator configuration, MCLK direction, FC selectors and PLL lock. Success means local configuration readiness, not measured clock accuracy or DSP acceptance.

Failure or cancellation stops subsequent operations, keeps external output disabled, and performs no speculative rollback. Already powered clocks can remain powered and partially programmed. The external owner must inspect and restore a safe isolated state before reinitializing. This component neither repairs a lost external ownership guarantee nor enables pins after failure.

## Evidence and verification

The register sequence comes from `../rebuild-re/emulate_stock_audio_clock.py` and its exact 53-write stock trace, independently checked against pinned NXP `fsl_clock.c` and official UM11126 Rev2.8 sections 4.6.6.5.2, 11.5.7/11.5.8 and 12.4.1. The source subset performs 22 writes: four XO power/routing writes, the exact 13 PLL-input/power/parameter/latch writes, then three PLL-divider writes and two MCLK divider/source writes. It omits stock's repeated oscillator helper setup, downstream activation and shared-control writes. MCLK is divided before its internal source is attached, while MCLKIO remains input.

`tests/test_audio_clock.c` passed strict host C11 compilation with `-Wall -Wextra -Werror -Wconversion` and AddressSanitizer/UndefinedBehaviorSanitizer. Tests cover unsupported prerequisites before writes, bounded callbacks, reserved-bit hygiene, preserved oscillator mode/trim and unrelated power, protected clocks, XO/PLL/divider/overall deadlines, the minimum lock interval, timer wrap, write failures at all 22 positions, read failures, ownership loss/cancellation in every active phase, and post-programming state/lock changes. These are callback-model tests, not analog or electrical validation.

Reproduce from `firmware/mcu1-source` using a host GCC-compatible compiler:

```text
cc -std=c11 -Wall -Wextra -Werror -Wconversion -fsanitize=undefined,address -I include src/audio_clock.c tests/test_audio_clock.c -o /tmp/omni-test-audio-clock
/tmp/omni-test-audio-clock
```

## Passive snapshot in prepared candidate

`audio_clock_snapshot.c/.h` supplies a separate read-only 40-word snapshot. Its header documents every fixed field. ANACTRL is read only when AHBCLKCTRL2 bit 27 is enabled and PRESETCTRL2 bit 27 is clear. Metadata explicitly marks valid domains and whether gate/reset values matched before and after. No clocks are enabled, no registers are written, and unsupported `SYSOSCCTRL` from other LPC families is not accessed. Independent HID pages are independent samples.

Prepared candidate `omni-a-13bb75b67f0a11a5`, SHA256 `00828ba1b1c7055855de1102f4a064c7711f1d9226027a3785c87c993efeed1b`, includes this snapshot at opcode 25. The clock sequencer remains inactive. `../rebuild-re/test_audio_clock_snapshot_arm.py` passes six clock/reset/PRIMASK combinations, null rejection, changing-gate detection, four valid HID pages, two invalid pages, and zero MMIO writes. Evidence is `releases/omni-a-13bb75b67f0a11a5/audio-clock-snapshot-regression.json`.

One coverage limitation is explicit: Unicorn skips the valid final three-instruction ITTT-PL block at candidate addresses `0xE7B8`–`0xE7BE`. Consequently the trailing duplicate XO_STATUS sample has source/disassembly coverage only. The first XO_STATUS sample and gated analog reads execute correctly in the compiled test. This is a known emulator modeling issue, not evidence of a firmware hardware failure, and the source was not changed to disguise it.
