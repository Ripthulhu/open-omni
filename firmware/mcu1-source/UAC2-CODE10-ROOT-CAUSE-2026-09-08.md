# UAC2 Code 10: missing notification endpoint halt handling

## Observed failure

Installed build `omni-a-1e3ea080f17d9faf` still produced Windows Code 10 after
the user's cold boot and after removing/rescanning its composite device entry.
The shared Microsoft USB audio driver package was not removed. A new audio
device instance was created and still reported `CM_PROB_FAILED_START`.

Linux (Ubuntu 24.04 on WSL2, kernel 6.18.33.2) enumerated this same build with
the standard `snd_usb_audio` driver. It exposed 48 kHz S16_LE stereo playback,
mono capture, playback volume/mute, and clock validity. Five seconds of
simultaneous silent aplay/arecord completed without application errors. This
was a synthetic smoke test, not wireless audio or endurance validation.

Direct Linux libusb controls confirmed that a 256-byte clock-range request
returns the correct 14-byte reply in about 2 ms. usbmon recorded successful
completion. The archived Windows driver's compiled clock-range parser also
accepts those bytes under emulation with a successful transport mock.

## Windows evidence

USBPcap attempts produced zero packets and are not evidence of device behavior.
The successful capture used Windows' built-in USBXHCI/UCX ETW providers with
Microsoft's documented Default + PartialDataBusTrace keywords.

Local capture: `firmware/rebuild-re/evidence/wsl-audio/OmniUsb-20260908T174535Z_000001.etl`.
The XML export and selected Omni events are adjacent. Device selection uses
the USB device descriptor's VID/PID, then its ETW device identifier.

At 17:45:36 UTC, UCX recorded:

| Request | Result |
| --- | --- |
| `a1 02 00 02 00 05 08 00` playback volume RANGE | 8 bytes, USB and NT status success |
| `a1 02 00 01 00 0a 00 01` clock frequency RANGE | 14 bytes, USB and NT status success |
| `02 01 00 00 82 00 00 00` CLEAR_FEATURE ENDPOINT_HALT on 0x82 | USB status `0xc0000004` (STALL), NT status `0xc0000001` |

The failing request is standard USB endpoint management, not an AudioControl
class request. The existing firmware's AC-only request ring therefore did not
record it. This explains why the old trace appeared to stop at clock RANGE.

## Source defect and correction

NXP's pinned Chapter 9 handler delegates endpoint SET/CLEAR_FEATURE to class
events `SetEndpointHalt` / `ClearEndpointHalt`. Our source-owned audio adapter
did not handle either event, so its default InvalidRequest stalled EP0.

The correction handles both events for the configured notification interrupt
endpoint 0x82, calls the existing controller stall/unstall API, and propagates
its status. It validates the full 16-bit endpoint index. It does not accept
halt requests for the isochronous streams or take ownership of HID endpoint
0x81. No descriptors, boot code, MCU2, or DSP code changed.

`test_endpoint_halt_arm.py` executes compiled Chapter 9 and audio adapter code
with bounded dispatcher/controller mocks. The exact captured request fails
on the installed build without touching the controller. It succeeds on the
corrected candidate, which also passes invalid request, reset-state, and
controller error propagation checks. Hardware timing is not emulated.

Candidate `omni-a-2843ee785dfbbf52`, image SHA-256:
`83b4647a56f6e4ddb9024dff54c8d8c2d3251bfb206998cc023c02b66ab8d75b`.

Two build directories produced identical application images. Offline checks
passed: compiled audio adapter, endpoint halt regression, 124 boot-ack cases,
48 recovery cases, four startup regression cases, three host test suites,
image CRC/hash, vectors/RAM bounds, source snapshot, and restore references.

## Candidate hardware results

The candidate was flashed once after the defect was identified and reproduced
offline. Per-block acknowledgements, CRC, full staged image readback, running
build identity and 16656-byte code readback passed. Startup acknowledgement was
1 (written), ROM driver status 0. Evidence:
`releases/omni-a-2843ee785dfbbf52/hardware-20260908T175354Z.json`.

Windows now reports `CM_PROB_NONE` on the audio, HID, and composite device.
Its standard UAC2 driver creates playback and microphone endpoints. All five
Windows-originated volume/mute cases and all five simulated device-originated
cases passed. Notifications completed with no errors during those checks.
The advertised synthetic -60..0 dB / 1 dB range also matched Windows.

The following ten-second silent duplex test failed: only 52 callbacks arrived
(about 0.52 seconds at 480 frames/callback). Playback advanced 587 packets,
microphone 528, and the firmware error counter increased from 0 to 1. Captured
callback data was zero; PortAudio reported no callback status flags. This is
not a streaming pass. The error counter does not identify its cause.
Evidence: `releases/omni-a-2843ee785dfbbf52/uac2-windows-20260908T175436.368172Z.json`.

A subsequent attempt to attach this build to WSL failed with device-busy even
after forced binding. After unbinding back to Windows, HID enumeration returned
an uppercase cached serial and direct feature-report exchange failed. A later
ETW test therefore stopped at identity selection without exercising streams.
Windows still showed installed device entries. Host binding state and a device
fault are both unresolved possibilities; neither is established as the cause.
The user has been asked for full power removal before another test. No second
candidate was flashed, and cold boot/physical recovery of this build remain
unverified. No ETW or USBPcap capture process is intentionally left running.

### Follow-up after full power removal

The user completed the requested power cycle. At 18:00:43 UTC, full runtime
readback matched and acknowledgement was 2 (already valid), driver status 0.
This verifies one normal cold startup of the candidate; physical recovery of
this build is still untested.

The next Windows test again passed all ten volume/mute cases. Duplex ran for
952 callbacks, with 9475 playback and 9526 microphone device packets, but 18
firmware errors. UCX trace `OmniUsb-20260908T180104Z_000001.etl` shows successful
96-byte microphone packets interrupted by transaction errors (`0xc0000011`).
It also records interrupt endpoint babble statuses (`0xc0000012`) near shutdown.
Neither descriptor-cache cleanup nor the endpoint halt fix resolves this
separate transfer problem. The firmware's aggregate counter cannot classify it.

After both streams closed, forced WSL attachment succeeded. On the same build,
all ten native ALSA volume/mute cases passed, including completion of nine
device notifications. Ten-second duplex captured exactly 960000 zero bytes,
but added 22 firmware errors (18 to 40). Playback advanced 9967 packets and
microphone 10024. Thus both hosts reproduce errors; Linux command completion
alone would have hidden the problem. WSL's USB/IP transport remains an extra
variable compared with native Linux hardware. Evidence:
`firmware/rebuild-re/evidence/wsl-audio/linux-test-20260908T180516Z.json`.

The device is currently attached to WSL with streams stopped, volume -30 dB,
and mute enabled. The next investigation is bounded error classification and
clock/controller state. In particular the earlier manual review identifies an
unmeasured FRO trim-source prerequisite; it is a hypothesis, not a proven cause.

## Portability requirement

Windows, Linux, and macOS are intended standard-driver targets. Linux success
does not waive Windows failure; macOS behavior remains untested. Diagnostic
and flashing tools may have platform-specific backends without requiring a
host bridge for normal audio or volume operation.

## References

- [Microsoft USB ETW capture procedure](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/how-to-capture-a-usb-event-trace)
- [Microsoft USB trace status examples](https://techcommunity.microsoft.com/t5/microsoft-usb-blog/answering-the-question-quot-what-s-wrong-with-my-device-quot/ba-p/270697)
- [Microsoft native UAC2 support](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers)
- Pinned NXP USB middleware: `device/usb_device_ch9.c`, `device/class/usb_device_class.c`, and `device/class/usb_device_audio.c` in the locally retained vendor checkout.
