# MCU1 source software recovery candidate

Candidate: `omni-a-003daae989892621`.
Application SHA256: `fa0b02ed258921b8279b87e5b18f691ea1986f9431b6a29a48e41c9ca120c088`.
Now flashed and one software recovery/return cycle verified. See
[hardware results](SOFTWARE-RECOVERY-HARDWARE-2026-09-08.md). The sections below
record the implementation and original preflash checks.

## Implementation

HID identity capability bit 4 now advertises software recovery. Existing commands
1–4 retain their protocol. The new commands use the same 64-byte feature report:
report ID 1, opcode at byte 1, echoed sequence at byte 2, status at byte 3.

- Opcode 5: prepare; little-endian nonzero session token at bytes 4–7 and literal
  `BOOT` at bytes 8–11. Queues one attempt after successful startup acknowledgement.
  Same-token repeats do not repeat an erase; another token is rejected.
- Opcode 6: status; little-endian state/token/result/signed driver status at
  bytes 4/8/12/16. State 0 idle, 1 queued/in progress, 2 verified ready, 3 failed.
- Opcode 7: reset commit; matching token and `BOOT` guard required, state must be 2.
  Main performs CMSIS system reset. Disconnect may interrupt the commit transfer.

Main context performs all flash work. Only the fixed 512-byte loader metadata page
at 0x7F800 can be written. It validates magic, MCU1 application base, known force
word, and acknowledged running flags; changes only force bytes 4–7 to 0xABBABAAB;
preserves every other byte. Checked read/erase/blank-check/program/verify stop on
any error. The existing ROM version/table/geometry/12 MHz guards and prefetch
protection also apply. Failed preparation never authorizes reset.

This is a persistent preparation: if the host stops after preparing, the next
reset/power cycle requests recovery. There is deliberately no automatic retry or
cancel write. A power cut during page replacement is not an atomic transaction.

## Dump evidence

The captured bootloader's 0x721C metadata normalization and 0x6F72 recovery
decision route acknowledged states with force 0xABBABAAB to bootloader USB.
`rebuild-re/emulate_boot_metadata.py` now executes this path for all three
accepted flag combinations (1/0/1, 0/1/1, 1/1/1). This is our own request
implementation based on the loader contract; it does not call stock application
functions or claim to reproduce the complete stock software-reset sequence.

## Verification

- CTest: core and boot metadata suites passed, including recovery state matrix,
  preservation/idempotence, every I/O failure, and malformed header bytes.
- Python: 11 runtime-verifier/recovery-host tests passed.
- Compiled ARM: 124 acknowledgement and 48 recovery preparation cases passed,
  including incoming IRQ/prefetch state, ROM error handling and page preservation.
- Cold-start regression: four cases passed, retaining the old failing build as
  a negative control.
- Two build directories produced the same application hash.
- File verifier passed image CRC/hash, vectors/bounds, 21 source files and both
  stock/diagnostic restore hashes. Evidence is in the candidate release folder.

ROM/USB hardware and reset propagation are not emulated. The offline checks alone did not establish hardware recovery; see the later hardware report; UAC2, display/dial and wireless audio remain unimplemented.

## Hardware sequence

1. User enters physical MCU1 bootloader with all power disconnected for 30 seconds,
   holding the dial while reconnecting USB1 and releasing after five seconds.
2. Confirm unique 1038:2291 FFC0. Flash with `tools/flash_source.py` using the
   candidate release and `--execute`; require staged and runtime readback plus ack.
3. Run `tools/enter_recovery.py` with the candidate release and `--execute`.
   Default is file-only dry run. The tool first verifies installed identity/code,
   prepares and polls verified status, issues reset once, then independently
   requires unique 1038:2291 and matching full installed code readback. Logs include
   the loader metadata page. A sent reset alone is never success.
4. Return through the verified loader handoff; resolve disconnect independently
   with runtime verification. Repeat normal cold boot and physical recovery.
5. Demonstrate stock/diagnostic restoration to finish the outstanding restore gate.

If a hardware milestone fails, use the matching diagnostic-v2 restore package
through the verified loader. Preserve bootloader code, factory data, MCU2 and DSP.
