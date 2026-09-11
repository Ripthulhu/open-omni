# MCU1 startup corrections and returned recovery — 2026-09-08

## Result

Implemented both established defects in the source-based LPC5528 MCU1 application:

- Start the USB functional divider and FRO96 mux before USBFSH PORTMODE access.
- Implement the stock loader's successful-startup acknowledgement after USB runs
  and the host configures the device. Use checked NXP ROM flash operations on the
  fixed metadata page 0x7F800, preserving every unrelated byte. Initialize ROM
  flash timing with the actual 12MHz CPU frequency instead of the SDK's 96MHz default.

The acknowledgement is attempted once in main context, with interrupts masked and
restored, inherited watchdog feeds between operations, geometry/API validation,
and checked read/erase/blank/program/verify results. Invalid metadata stops the
operation. Already-valid metadata causes no erase. HID opcode 4 exposes the result
and driver status. The runtime verifier now rejects missing/failed acknowledgement.

## Candidate

`releases/omni-a-10c7e4e03dfb3749`

- Application image: 285612 bytes; SHA-256
  `0b9e32ee877df3dbc47c16701d56e6cf387cd5c76efddb11af82a708d4cf5969`.
- Actual code/data image: 13296 bytes; SHA-256
  `fdd40298531f35f2cc8282b15ca36b4e4089ad2e90ed46290029874d1e6594ed`.
- Contains ELF, map, symbols, manifest, 19 hashed source files, dependency lock,
  stock/diagnostic application restore images and manifests, and regression evidence.
- Rebuilt in a clean ARM build directory; application bytes identical. The final ELF
  also matches the ELF hash used by the clock regression.
- **Not flashed. No new source build has been hardware-validated.** Milestone A
  remains incomplete, including software recovery and persistent fault evidence.

## Offline verification

| Check | Result |
|---|---|
| CTest volume/UAC control and boot acknowledgement contract suites | 2/2 suites passed |
| Runtime identity/readback/acknowledgement verifier | 6 tests passed |
| Existing update image/ACK/timeout/boundary tests | 8 tests passed |
| Compiled ARM USB startup | 4 cases passed, including frozen failed build as negative control |
| Compiled ARM acknowledgement policy and SDK dispatch | 62 cases passed |
| Image CRC/SHA, all vectors, RAM/flash bounds, source snapshots, restore references, evidence hashes | Passed |
| Clean independent build | Identical application SHA-256 |

USB regression executes the actual compiled board/clock/reset/watchdog instructions,
with modeled MMIO semantics. The old cold case detects the unsafe access at 0xC248;
the corrected cold and warm cases finish with the required clock/gate state.
The acknowledgement regression executes compiled application and SDK wrappers while
modeling ROM bodies, clock queries and watchdog. It covers fixed page/length/key,
12MHz pre-init, both SDK read-dispatch addresses, page preservation, each failure,
ROM/geometry/clock rejection, one attempt, stack and interrupt restoration.

These tests do not emulate physical USB, ROM internals, flash timing or power loss.
The inherited watchdog's timing during flash work still needs hardware validation.
The metadata erase/program sequence is not atomic against power loss.

Evidence is in the release's `startup-regression.json`, `boot-ack-regression.json`
and `file-verification.json`. Clean build output is
`../rebuild-re/evidence/startup-fix-clean-build.log`. Scripts are
`../rebuild-re/test_startup_fix.py`, `emulate_source_boot_ack.py`, and
`verify_candidate_files.py`. The latter is strictly a local-file checker.

Automatic approval review rejected invocation of the existing flash utility's default
dry-run path because the command lacked an explicit dry-run flag. No flash utility
execution occurred. A standalone checker with no USB/subprocess imports completed
the file validation instead; this report does not claim that CLI dry-run passed.

## Hardware returned during implementation

The user reported recovery after a longer unplug and/or opening SteelSeries GG.
Read-only HID enumeration confirmed **1038:2291, SteelSeries Bootloader**, vendor
usage FFC0 on interface 0. No reset, erase, program, commit or restore commands were
sent during this implementation turn.

Captured through the already-recovered target-1 opcode 0x83 reader:

- All 304 vector bytes match the frozen failed source release.
- The full 285612-byte installed application exactly matches the frozen failed
  `omni-a-29585df231c9362f` image, SHA-256
  `ac52d45e6f5186f0dc43e175bcecca8e2a686da17410a7c25acc6a0b90a10b85`.
- Metadata page SHA-256
  `e40ab2b054b52ca1df02ef4e3f98e146e7cc65a130218f5af60bb1f9005c7e34`.
- Metadata: magic B00710AD, force 0, application base C000, eligible/pending/ack
  **1/0/1**. This is consistent with the previously recovered dial transitions;
  it does not show that the old application implemented an acknowledgement.

Binary captures and full HID transcripts are in
`../rebuild-re/evidence/recovered-usb-20260908/`.
The recovery mechanism itself is still uncertain. We did not observe the original
failed PC or the trigger that restored bootloader USB. The bounded reader does not
read the boot code below 0xC000, so this is not a fresh complete boot-integrity check.
The device was left in its returned bootloader state, with the old application intact.

## Documentation and next hardware work

See `../rebuild-re/official-docs/INDEX.md` for exact sources and download status.
NXP-authored UM11126 rev2.5 (2023-06-19) was downloaded from Farnell; current rev2.8
(2025-06-23) is account-gated and has not been downloaded. Microsoft UAC2 HTML is
local. NXP data sheet, errata and application notes were browsed; their direct PDF
downloads failed. Pinned NXP headers, USB middleware and board examples are local.

Preserve these captures before further hardware work. Recover a known baseline and
collect any still-needed boot/ROM evidence through the documented mechanisms.
The corrected candidate still needs actual acknowledgement readback, cold boots,
and a demonstrated return through physical recovery before being accepted.

MCU2, the Airoha DSP and the headset were not modified. The volume bridge remains
paused. Audio, dial/display support and inter-chip startup remain future milestones.
