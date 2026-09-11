# Silent UAC2 prototype

Current update: Windows Code 10 was traced to a stalled standard endpoint
clear-halt request and fixed in `omni-a-2843ee785dfbbf52`. Native volume/mute
passed in both directions. Duplex streaming remains faulty; the detailed
[cause and hardware report](UAC2-CODE10-ROOT-CAUSE-2026-09-08.md) supersedes the
earlier hypotheses and pending cold-boot notes below.

MCU1 source application, LPC5528 target 1 at 0xC000. Stock bootloader, MCU2 and
DSP remain unchanged. This milestone tests the Windows USB contract; it does not
route sound to the wireless headset or operate its microphone, dial or display.

## Interfaces

| Interface | Purpose | Endpoint |
| --- | --- | --- |
| 0 | UAC2 AudioControl, clock 10, master Feature Unit 5 | 0x82 interrupt, six-byte notifications |
| 1 | Stereo playback, alternate 0 off / 1 streaming | 0x03 isochronous OUT |
| 2 | Mono microphone, alternate 0 off / 1 streaming | 0x83 isochronous IN |
| 3 | Existing diagnostic/recovery HID | 0x81; feature report ID 1 |

One fixed 48 kHz, 16-bit PCM clock. Synthetic streams are USB-frame synchronous:
playback is discarded and microphone packets contain 48 zero-valued samples.
The wireless clock contract remains to be recovered; this prototype does not
establish that synchronous endpoints are suitable for the actual DSP audio path.

Volume is a synthetic -60..0 dB range in 1 dB steps, initially -30 dB and muted.
Mute is separate from remembered volume. No hardware attenuation is inferred.
USB callbacks serialize control access; main excludes interrupts while queuing
volume notifications. Notifications are retired only after successful completion,
and changed state remains pending when an older notification completes.

The adapter uses NXP device/class dispatch and IP3511 transport. It replaces the
SDK audio adapter's single-stream state with independent playback/microphone
alternate settings. No pinned vendor source is modified.

## Additional HID commands

Existing commands 1–7 remain supported; identity capability bit 5 means silent UAC2.

- 8: read state. Response bytes 4–5 signed volume in 1/256 dB, 6 mute, 7 pending;
  little-endian counters at 8 revision, 12 playback packets, 16 microphone
  packets, 20 errors, 24 completed notifications; bytes 28–30 are configured,
  playback alternate and microphone alternate.
- 9: simulated local control change. Request bytes 4–5 signed volume, 6 mute.
  Rejects out-of-range/unaligned volume and invalid mute without changing state.
- 10: bounded control trace. Request byte 4 selects slot 0–31. Response bytes
  4–7 total audio setup count; bytes 12–19 setup, 20 reply length (255 rejected),
  bytes 24–39 reply data. Ring slots use count modulo 32. Only audio-interface
  requests are recorded. This is a request/reply record, not a timestamped timing
  trace or proof of the bytes received by the Windows host controller.

## Offline validation and first hardware observations

Three CTest suites cover volume/control, boot metadata and descriptor topology.
Compiled ARM tests execute adapter configuration, independent stream activation,
silence, control requests including Windows' 256-byte RANGE request, and volume
notification completion with mocked transport. Existing 124 acknowledgement,
48 recovery and four startup regression cases pass for the tested candidates.

`omni-a-216faf3d75522e31` booted with full staged/runtime readback and ack status 1,
but Windows selected `usbaudio2` and returned Code 10 / status 0xC0000001. HID
and software recovery worked. Later trace builds `3c4d4487afa749d3` and
`35e2eaf81802c007` preserved those functions. The latter recorded valid replies:

- Volume RANGE: `01 00 00 C4 00 00 00 01` (-60..0 dB, 1 dB step).
- Clock RANGE: `01 00 80 BB 00 00 80 BB 00 00 00 00 00 00` (48 kHz only).

Windows stopped issuing audio queries after the clock RANGE in those builds.
`omni-a-1e3ea080f17d9faf` tests packet capacities of 196 bytes playback and 98
bytes capture (one additional complete audio slot). Actual microphone payload
remains 96 bytes. Hardware results must be recorded before claiming this fixes
the driver failure or completes milestone B.

## Sources

- [Microsoft UAC2 driver requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers): fixed clock controls, stream formats, and native volume/mute interrupts.
- [Espressif UAC changelog](https://components.espressif.com/components/espressif/usb_device_uac/versions/1.2.3/changelog?language=en): Windows Code 10 resolved by frame-aligned endpoint padding in that implementation; evidence for a hypothesis here, not proof of our cause.
- Pinned local NXP `usb_device_class.c`, `usb_device_ch9.c`, `usb_device_audio.h`,
  and `usb_device_dci.c` establish dispatch, request layout and endpoint APIs.

Host trial: `rebuild-re/test_uac2_windows.py RELEASE`. It selects only the uniquely
named prototype endpoints, never a fallback/default device. Ten seconds of silent
duplex traffic is a bring-up trial, not the two-hour final audio release gate.

## Packet-capacity trial result — 17:19 UTC

`omni-a-1e3ea080f17d9faf` was flashed with per-block ACKs, CRC checks and complete
staged readback. Runtime identity and all 16628 code bytes matched; startup ack
1 / driver 0, fault status 0. Image SHA256:
`87e17c34ca7bf606e948371020715c7cca124a2aa3ef9c5ef82d3fb6c996876c`.

Increasing capacity to 196/98 did NOT resolve Code 10. Windows again issued only
volume RANGE then clock RANGE, with the same valid device-side response bytes.
No native audio endpoint was created, so the Windows bidirectional volume and
silent-duplex tests did not run. Milestone B is NOT complete.

Current device: this prototype remains installed for the next user-assisted
cold-boot observation. Test a normal 30-second power removal, without dial hold,
then inspect runtime ack and Windows audio status. This checks whether the first
startup acknowledgement flash operation interacts with audio enumeration; it is
an unproven hypothesis. If this trial fails, restore diagnostic v2 as planned and
continue investigating control transport/Windows compatibility offline.

Evidence in `releases/omni-a-1e3ea080f17d9faf/`:
`hardware-20260908T171844Z.json`, `uac2-windows-20260908T171859.501775Z.json`,
`audio-health-and-trace.json`, plus compiled regression and file-verification
reports. Earlier candidate release folders retain their failed trials.

The first trace revision also recorded HID requests and overwrote useful audio
history during readback; the final trace filters to interface 0 and includes
reply data. A Windows device restart was unavailable to this process (access
 denied); no permissions or installed drivers were changed.
