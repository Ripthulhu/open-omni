# Bounded MCU2/DSP control UARTs

The compiled candidate `omni-a-1f282d1790403701` passed the offline checks below. Its application SHA256 is `8d1299f5d17b622493171353bd253e68caac100d02c40a607b90cda32373e430`. These are modeled peripheral tests of the actual linked ARM code; they do not establish physical baud accuracy, peer response, or interrupt timing. No live hardware operations were performed by these tests.

## Backend contract

`control_uart.c` accepts only port 3 (DSP, 921600 8N1) or port 7 (MCU2, 921600 8N2). Both use their own FRO12 selector and fractional-divider bypass, OSR=12, BRG=0. One active port shares a 2048-byte RX ring; completed statistics remain available separately for each port. The compatibility MCU2 API is retained.

The interrupt handler drains at most 16 bytes. FIFO/line errors and ring overflow mark the selected port failed. Stop disables the selected UART and IRQ sources. Callbacks from a stopped or different port return failure without touching peripherals. Invalid or overlapping starts return zeroed callbacks. PSELID is read back after selection; a retained non-USART selection aborts before accessing USART registers or writing UART pins.

The two UART pin initializers use separate fixed address sequences. This made their generated MMIO stores easier to audit and removed a Unicorn multi-instruction IT-block modeling failure seen with the previous compiler layout. That emulator failure was not evidence of a hardware firmware defect.

## Read-only snapshot

The fixed 20-word version-1 snapshot returns false without peripheral access for an invalid or never-initialized port. It preserves PRIMASK and never reads FIFO data. GPIO values are digital samples, not voltage measurements. Hardware may change while the values are sampled; separate HID pages are separate snapshots.

| Word | Meaning |
|---|---|
| 0–2 | Version, requested port, current active port |
| 3–6 | PSELID, CFG, OSR, BRG |
| 7–9 | FIFOSTAT, FIFOTRIG, FIFOINTSTAT |
| 10–13 | Selected FCCLKSEL, FRG, TX IOCON, RX IOCON |
| 14–17 | GPIO0 PIN, GPIO1 PIN, GPIO0 DIR, GPIO1 DIR |
| 18–19 | AHBCLKCTRL0, valid GPIO mask (bit 0 GPIO0, bit 1 GPIO1) |

GPIO registers are read only when their AHB clock gate is enabled. The UART backend itself does not write GPIO0_17 or GPIO1_7. The DSP probe's separate, explicitly requested preparation owns those two pins.

## DSP probe and preparation

The query is queued explicitly and can run only with USB configured, no recovery action, both audio alternates zero, and a valid boot acknowledgement. A queued MCU2 probe and queued/settling/active DSP probe exclude each other. One token is accepted per boot; polling a completed request never retries it.

Before its query, the DSP probe captures the previous GPIO PIN/DIR and IOCON values, sets GPIO1_7's HIGH latch before enabling its output direction, and selects GPIO0_17 input. Its IOCON writes use documented function-0 digital-enable bits (`0x100`); stock's reserved bit 14 is left clear. The order comes from the independently recovered stock startup. Pin semantics remain unresolved; setting HIGH may release a held peer reset. No LOW pulse is issued.

Preparation has a cooperative 2000 ms settling phase. The UART remains stopped during that phase. The query then transmits `BD 04 E1 02` once and accepts the recovered response `DB 07 E1 03 00 36 00`. It stops after a matching reply, I/O error, cancellation, or 250 ms timeout. An unexpected frame is retained for inspection without causing a second query. Cancellation while queued performs no MMIO; cancellation during settling prevents UART startup.

## Executable evidence

Run from the repository with the existing Python analysis dependencies:

```text
python firmware/rebuild-re/test_dsp_probe_arm.py firmware/mcu1-source/releases/omni-a-1f282d1790403701
python firmware/rebuild-re/test_mcu2_uart_arm.py firmware/mcu1-source/releases/omni-a-1f282d1790403701
python firmware/rebuild-re/test_usb_endpoints_arm.py firmware/mcu1-source/releases/omni-a-1f282d1790403701
```

The first command also runs `test_control_uart_arm.py`. Matching JSON files in the release record the tested image hash. Checks cover both port vectors/configurations, FIFO order and backpressure, bounded IRQ service, ring overflow, cross-port stale callbacks, retained per-port counts, locked-PSEL rejection, read-only snapshots and GPIO clock guards, GPIO prestate/order, settling and timeout across time-counter wrap, terminal shutdown, wrong-response retention, HID exclusion during settling, and eight actual compiled main-loop admission conditions.

The inherited MCU2 test separately records its existing limitation: two HID stream-active rejection branches have source/disassembly coverage because Unicorn mishandles their taken backward branch. The actual compiled main-loop stream/disconnect admission checks pass for both probes. The endpoint snapshot suite passes 24 valid page/PRIMASK combinations, 5 invalid requests, 19 invalid pointer/controller cases, and hardware-change marking without USB register writes.
