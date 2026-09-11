# Omni MCU1 source firmware

Experimental LPC5528JBD100 application, update target 1, linked at 0xC000.
MCU2, DSP, bootloader code and factory storage are outside its write scope.
One fixed loader-owned metadata page at 0x7F800 is updated for the verified startup acknowledgement contract.
This is a research prototype, not daily-use firmware. Current installed build
and hardware limitations are recorded in `../../docs/status.md`.

## Implemented

- Independent startup, vectors, stack reservation, inherited watchdog feed.
- USB0 full-speed device setup on the transmitter's USB1 connector, using NXP USB middleware.
- HID feature-report identity and bounded read-only code access.
- Strict MCU1 image packaging and flash verification, including running build identity and code readback.
- Cold-start USB clock ordering: functional clock running before USBFSH PORTMODE access.
- One startup acknowledgement after host USB configuration, using the NXP ROM flash API and full-page verification.
- Native UAC2 playback through DSP47, serialized volume/mute, Windows percentage display and corrected dial direction. USB1 microphone uses I2S0 RX DMA and asynchronous mono16/48 capture.
- USB1 playback accepts packed stereo PCM16 or PCM24 at48 or96kHz through separate alternates and a programmable clock. Microphone has its own fixed16/48 UAC2 function. Format epochs isolate USB packets and quiesced DMA restarts; HID74 reports requested and applied formats independently. Exact-build hardware results belong in `../../docs/status.md`.
- Source-owned SPI OLED driver, rotary input and millisecond event loop.
- Hold the transmitter dial for one second to open the menu; short clicks select inside it. Headset dial direction follows the user-confirmed native calibration.
- Two-phase software recovery with verified metadata, explicit USB detach intervals, and a bounded lost-SETUP interrupt guard.
- Fixed read-only USB error, clock, UI and endpoint diagnostics.
- Independent UART3/DSP and UART7/MCU2 listeners, persistent bounded MCU2 discovery and captured lifecycle forwarding; no peer reset or flash.
- Native settings for limiter, microphone level/noise/sidetone/LED, ANC/transparency, Bluetooth startup/call policy, auto-off and wireless/mic/Bluetooth EQ. Built-in coefficients come from the original preset ROM.
- Serialized setting requests, exact ACK matching, bounded custom-EQ staging and coherent read-only caches over HID. Mode2 is set and read back before native audio gain becomes ready.
- One bounded, verified mode2 repair per UART acquisition if a complete initial gain query reports the known mode1 rollback. Other failures remain distinct and do not trigger a blind retry.
- Local display brightness, timeout, off/dim and simple/detailed home views. Serialized headset menu entry/exit with captured 91/92 acknowledgements, duplicate suppression and bounded timeouts.
- Audio endpoint ownership retained across failed cancellation, bounded endpoint skip waits, and verified DMA quiescence before restarting playback.

Playback and native gain were hardware-tested on the earlier transmitter. Each new
candidate requires separate validation on the replacement. MCU2 status communication
does not establish USB2/USB3 audio routing. Submenu-specific
headset prompts, setting persistence and full lifecycle/endurance validation remain
incomplete. The inherited watchdog can reset on faults; fault status currently does
not survive that reset.

`tools/native_controls.py RELEASE status` reads native diagnostics on Windows or
Linux using hidapi. Its `set`, `preset`, `eq-blob`, `cache`, `display`, `mixer` and
`input` commands require the matching installed build identity. Settings completion
means DSP acknowledgement unless the control also has verified independent readback;
it is not an acoustic test. The current audio path requires output mode2.

## Build

Use GNU Arm Embedded 13.2.1, CMake 3.28 and Ninja in WSL. `dependencies.lock.json`
records exact clean upstream revisions. Each dependency is checked before building.
The local lab's vendor directory is `/srv/ai-agent/work/steelseries-arctis-nova-pro-omni-firmware/mcu1-source/vendor`.

```sh
python3 tools/build.py --vendor /path/to/vendor --build /path/to/build-arm
```

The release directory contains ELF, linker map, symbol addresses, application image,
source snapshot/hash inventory, dependency revisions, hash-pinned stock/diagnostic restore images and manifest. The application image uses
the proven 285612-byte envelope, 0xff padding and a big-endian CRC32 trailer. Padding
is not reported as executing code. Repeated builds with identical source identity must
produce the same image bytes or packaging fails.

The prototype reuses the physical device's VID/PID for private experiments, with a distinct
product string, bcdDevice and build serial. This is not a USB identifier allocation for distribution.

## Flash and restore

`tools/flash_source.py RELEASE` validates without writing. `--execute` requires the unique
MCU1 1038:2291 bootloader, validates each block acknowledgement, queries both CRC values,
reads back the full image, commits, then verifies build identity, reads back running code,
and requires a successful boot acknowledgement from builds advertising that capability.
`--verify-only` performs just the running check. Any failure is recorded in the release folder.

Physical recovery was demonstrated with stock restoration before the first source trial:
fully disconnect transmitter power, hold its dial, reconnect USB1, release at about four
seconds (use five seconds for the repeatedly observed MCU1 entry). The actual MCU1 bootloader enumerates 1038:2291. A short hold showing
FIRMWARE UPDATE AVAILABLE while audio still works is not that bootloader.

From `../rebuild-re`, run `flash_verified_mcu1.py diagnostic --execute` after confirmed
bootloader entry to restore the hash-pinned diagnostic v2 image. Use `stock` for the stock
reference. Do not use a combined flash capture as an update image. Hardware logs are the
authority for whether any particular build has run; successful compilation is insufficient.

The first source build `omni-a-29585df231c9362f` ran after update but failed subsequent
USB startup. It omitted the stock boot acknowledgement and accessed PORTMODE before
starting its functional clock. Both are corrected in current source and covered by
offline regressions. The transmitter subsequently returned to MCU1's bootloader;
the failed application vectors and live metadata were captured without flash writes.
The exact recovery trigger (longer power-off or GG activity) is not established.
Do not count that recovery as hardware validation of this new candidate.

## Boot acknowledgement

After the USB stack runs and the host selects configuration 1, main performs one attempt.
The ROM initializer receives the actual 12MHz CPU frequency; the pinned SDK's public
initializer would instead hardcode 96MHz. ROM major 3, table pointers, frequency and
the captured 512KiB / 512-byte-page geometry must match before flash operations proceed.
Reads use ROM FLASH_Read to report ECC errors rather than directly loading possibly
erased or corrupted flash. Interrupts are masked during the operation and restored afterward.
CPU flash prefetch is disabled before ROM commands as required by UM11126 rev2.8;
its original enable state is restored afterward while preserving any ROM-updated
flash timing fields.

Only the page [0x7F800,0x7FA00) can be rewritten. Given valid pending metadata, clear
the force word at +4 and set acknowledged byte +14, preserving +12/+13 and every other
page byte. Check erase, blank, program and full-page verify results in sequence. Already
acknowledged metadata causes no erase. Invalid metadata or any driver error stops the
attempt; there is no automatic retry or repair of invalid metadata. This erase/program
sequence is not atomic against power loss, matching a limitation of the recovered loader.

ROM/controller timing, cold boots and software recovery have hardware evidence for
specific releases; new candidates still need their own verification. Full lifecycle
release gates and persistent fault tracing remain open.

## HID protocol v1

Vendor usage page FFC0, report ID 1, 64 bytes total, Feature SET followed by Feature GET.
Bytes 0–3: report ID, opcode, sequence, request reserved / response status.
Status 0 means success, 1 unknown command, 2 invalid address or size.

| Opcode | Request | Response |
|---|---|---|
| 1 | none | `OMNI` at 4, protocol 1 at 8, MCU 1 at 9, capability bits at 10, code base/end LE32 at 12/16, NUL-terminated build ID at 20 |
| 2 | address LE32 at 4, count at 8 (1–56) | count at 4, bytes at 8; limited to the programmed source code/data image |
| 3 | none | current fault field LE32 at 4; not persistent |
| 4 | none | boot acknowledgement result LE32 at 4; signed driver status LE32 at 8 |
| 5–7 | session token / BOOT guard | prepare, inspect and commit software recovery; see SOFTWARE-RECOVERY-2026-09-08.md |
| 8–12 | audio command/record selector | volume state, explicit local control, request/error ring and clock snapshots |
| 13 | none | UI version2 state, including percentage |
| 14 | none | lost-SETUP guard counters and retained first snapshot |
| 15 | fixed endpoint/page selectors | bounded endpoint diagnostics; see USB-OFFLINE-AUDIT-2026-09-09.md |
| 16–17 | token / MCU2 guard, or none | enqueue fixed MCU2 query/read result; see INTERCHIP-IMPLEMENTATION-2026-09-09.md |
| 18–19 | token / DSP1 guard, or none | enqueue fixed DSP status inquiry/read result, with recorded stock GPIO startup |
| 20 | UART port3/7 and page0/1 | fixed read-only configuration/pin snapshot after that port was initialized |
| 21 | none | DSP parser counters, first unexpected header and pre-start GPIO state |
| 71 | page0/1 | coherent read-only headset menu transaction, accepted context and timeout/ACK counters |

Capability bits: 0 identity, 1 code readback, 3 boot acknowledgement status,
4 recovery, 5 UAC2, 6 audio trace, 7 UI. Additional capabilities are enumerated
in each release manifest; do not assume an older build supports newer opcodes.
Acknowledgement results: 0 waiting; 1 written and verified; 2 already valid; 3 invalid
metadata; 4 read failed; 5 erase failed; 6 blank check failed; 7 program failed;
8 verification failed; 9 driver initialization rejected/failed. Driver status 0 means
success, positive values are NXP status codes; -1001 ROM version, -1002 ROM table,
-1003 clock, -1004 geometry are local initialization rejections. The host only accepts
results 1 or 2 with driver status 0.

No arbitrary memory/peripheral writes are exposed. Fixed diagnostics include
bounded peripheral reads; recovery and peer probes are explicit guarded actions.
Readback and multi-page snapshots are sequential.

