> Integrated candidate update: `omni-a-4d8ab9c0fb05338b` now links these drivers, a low-priority 1 ms SysTick, encoder sampling, dB/bar/mute-X rendering, and HID opcode13 UI status. Starts UI only after verified boot acknowledgement, and skips UI polling during recovery preparation. Two independent builds match SHA b94c7670326ad2b31c0a58ad95e01471e233e80427e6357dde6fdd94ebdf5702. 124 ACK,48 recovery,4 startup,USB HALT/audio/errortrace and integrated ARM UI tests pass;35 source hashes/bounds/restore packages verify. This candidate is prepared for MCU1 hardware trial; older object-only checkpoint below is historical. Encoder uses two edges per event provisionally; physical direction/detent calibration is outstanding. Buttons remain unused. No wireless audio is implemented.

# Source-owned dial/display drivers — implementation checkpoint

The installed transmitter still runs `omni-a-753a79269629ee0a`, with the passing
post-Windows-reboot USB baseline. No new application was flashed. The working
source tree now differs from that immutable release; do not claim its old
source hash describes these new files.

## Implemented

- `src/display.c`: cooperative startup and full-frame transfer state machine.
  Reproduces the recovered command bytes and pin transition order; separates
  queue acceptance from completion; retains a copied 1024-byte frame; rejects
  concurrent presentation; faults/cancels on transfer failure or a 100 ms stall.
  Timer comparisons handle unsigned wrap. No busy waits, allocation or logging.
- `src/display_lpc5528.c`: explicit SPI4 bring-up on the recovered pins,
  FRO12M source, divisor29 (400 kHz), mode0, eight-bit transfers, discarded RX.
  Services at most eight FIFO entries per call with interrupts left enabled.
  Waits for FIFO empty AND MSTIDLE before completion. No stock function calls.
  Failure cancels/disables SPI until explicit reinitialization.
- `src/rotary.c`: source-owned Gray-code decoder initialized from current
  phases, configurable two/four-edge reporting, reverse/bounce cancellation,
  invalid diagonal transition counter/reset. Physical direction and detent
  count are intentionally not hardcoded from an unmeasured assumption.

The CMake `omni_ui_bringup` object target compiles the ARM drivers but does NOT
link or activate them in the diagnostic application. Host test targets exercise
the portable logic. This is an implementation checkpoint, not milestone C pass.

## Validation

All five CTest suites pass: existing core/boot/audio descriptor contracts plus
new display-state and rotary-transition suites. Tests include startup ordering,
wire-completion gating, immutable in-flight frame copy, timer wrap, failed start,
controller failure, timeout, both directions from all starting phases, bounce
and invalid transitions. ARM compilation passes -Wall/-Wextra/-Werror/-Wconversion.

`firmware/rebuild-re/test_display_backend_arm.py` executes compiled backend
Thumb code with mocked clock/reset calls and FIFO/MMIO state. It passes pin
masks/divisor, bounded work, FIFO full, wire still busy, byte order, RX-ignore,
EOT placement, overlap rejection and error/cancel cases. The first harness run
omitted the ELF .rodata section; including it corrected the harness loading.
No firmware fix was inferred from that emulation setup error.

The test-only ELF is linked at 0x60000 with BSS at 0x20020000 against symbols
from the existing build, and .text PLUS .rodata are loaded into Unicorn. It is
NOT a flashable application and must never be passed to the flasher. Evidence:
`firmware/rebuild-re/evidence/display-backend-regression.json` and matching
`display-backend-test.elf`, `.bin`, `.symbols.txt`.

## Remaining integration gates

1. Establish a real millisecond timebase without disturbing USB or ROM flash
   operations. Stock main installs timer source `0x29CA1` with multiplier1 via
   `0x29E50`; `0x29CA0` scales its underlying counter by1000/1024. Verify the
   underlying RTC tick rate before describing the stock delay units as proven.
2. Add source-owned screen rendering and connect its snapshot to serialized
   volume/mute state. Keep the unimplemented headset/battery status explicit.
3. Add GPIO sampling/IRQ ownership for the verified encoder pair. Measure
   actual clockwise direction, transitions per detent and switch ownership.
   Do not connect an unverified ENTER input to mute/reset.
4. Run linker/vector/boot-ACK/recovery/USB regression checks on an integrated
   candidate, produce reproducible release/restore artifacts, and only then
   use the established MCU1-only flashing procedure.
5. Verify screen, dial->Windows and Windows->screen plus silent duplex on real
   hardware. The five-minute USB baseline does not validate these new drivers.

See `firmware/rebuild-re/DISPLAY-INPUT-MAP-2026-09-08.md` for stock evidence.

## First hardware installation

753a software recovery verified installed code and reached MCU1 bootloader.
Candidate4d8a received per-block ACK/CRC and full staged readback verification.
Runtime identity matched; running-code verification then failed on a HID write.
This is not a complete runtime pass. Evidence: candidate
`hardware-20260908T212522Z.json`; old release
`software-recovery-20260908T212233.053703Z.json`.
Requested full transmitter power removal30seconds and normal directUSB1return,
plus a screen observation. Do not reflash just to reconnect.

## Live display/dial and direction correction

After full power removal,4d8a runtime19196bytes and ack2/driver0 verified
(`hardware-20260908T212647Z.json`). User confirmed visible digits/bar/muteX.
HID UI reports initialized1/displayREADY8, no transfer failure. During manual
turning, encoder phase/event counters and display frames advance with zero
invalid transitions. User explicitly confirms left/counter-clockwise increased
volume and right/clockwise decreased: mapping is reversed. This test did not
establish exact transitions per physical click because the user exercised the
range in both directions rather than the requested five-click sequence.

Initial notification-delay and later manually-interfered control tests were
retained as failures. After explicit user "hands off", all ten native Windows
volume/mute cases and short silent duplex pass on4d8a. Do not relabel earlier
failures as successful or claim reconnect reliability from this one pass.

Correction candidate `omni-a-f0ce0a12c67dead5` negates the decoded step before
changing shared volume. Raw diagnostic positive/negative counts still denote
phase direction, not clockwise. Image SHA
`66a66b8573358cd9b7a2b4c46df5a9e3531d3d073bc70240bfae5596a3d759cb`;
two builds match, startup/ACK/recovery/USB and integrated UI tests pass, source
and restore packages verify. Commit7bad8e7.4d8a software recovery succeeded;
correction is undergoing full staged readback. No unrelated firmware changes.


## Verified dial/display checkpoint — 2026-09-08 21:42 UTC

Installed MCU1 build `omni-a-f0ce0a12c67dead5`, application SHA256 `66a66b8573358cd9b7a2b4c46df5a9e3531d3d073bc70240bfae5596a3d759cb`. Full staged and runtime 19200-byte code readback passed; boot ACK1 / driver0 (`hardware-20260908T213550Z.json`). User confirmed corrected physical dial direction. Prior 4d8a cold startup displayed digits/bar/mute X and passed a settled hands-off ten-case native control test plus ten-second duplex. Exact edges per physical detent remain unmeasured.

Current f0ce hands-off native test `uac2-windows-20260908T213911.038181Z.json` FAILED device-to-Windows notification: all five Windows-to-firmware cases passed, but pending=3 / notifications=0. One-minute silent duplex then PASSED: 5999 callbacks, 2879520 frames, zero host flags/nonzero microphone bytes, playback60004 and microphone60011 packets, firmware errors0 throughout. Notifications remained pending after streaming. Evidence `firmware/rebuild-re/evidence/wsl-audio/stream-comparison-20260908T214025Z.json`. Final read-only audio diagnostic at214204Z retained errors0, notifications0; controls were used during the separately confirmed direction check. No wireless audio is implemented.

Investigated the hypothesis that CLEAR_FEATURE cancels a notification without releasing the adapter busy flag. Pinned usb_device_lpcip3511.c EndpointUnstall calls Cancel; Cancel calls USB_DeviceNotificationTrigger with USB_CANCELLED_TRANSFER_LENGTH when a transfer is active. Our callback clears notify_busy and preserves pending state for retry. New compiled-ARM `test_notification_cancel_arm.py` passes cancellation/requeue and duplicate suppression. This does not prove all controller races absent, but does not support a blind busy-reset patch. No firmware changed or flashed after f0ce. Next capture the notification endpoint host/controller state around the stall, preserving the current working display/audio transport baseline. Warm restart notification reliability, cold startup of f0ce, physical detent calibration, headset status, wireless playback/mic and lifecycle release gates remain open.

No active stream/capture/flasher remains. Dial direction is physically confirmed. User may operate controls again; a future automated bidirectional test needs a fresh hands-off window.

