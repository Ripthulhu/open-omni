# Bounded audio error diagnostics

Candidate `omni-a-753a79269629ee0a` adds observation to the Code 10 fix. It does
not change descriptors, packet sizes, clocks, or transfer scheduling. Application
SHA-256: `e31c5c966a172614f243f23dd672bfbb87cd446fcba837ec2bfc1d0159513a6e`.
Installed through the physically verified MCU1 bootloader. Per-block replies,
CRC, and full staged image readback passed. Runtime identity matched, but full
runtime code readback failed with a HID read error. Windows enumerates the exact
new build and reports the composite, audio, and HID devices OK (Code 10 remains
fixed). A subsequent diagnostic identity write blocked and failed; no clock or
error-ring data was obtained. A targeted Windows device restart returned 3010
(reboot required). A normal full power cycle has been requested. Do not mark
runtime verification, cold startup, or the diagnostic measurements passed.
Evidence: release `hardware-20260908T181956Z.json`.

The Windows parent chain is the transmitter, one Generic USB Hub
(05E3:0608), USB Root Hub, AMD USB 3.10 controller. This does not establish the
multiple intervening hubs required by NXP erratum USB.2; do not attribute the
failure to that erratum based on the current topology.

The reason for this diagnostic build is measured errors under Windows and
Linux, while the previous aggregate error counter cannot identify the cause.
The earlier FRO trim prerequisite remains unmeasured. Do not modify analog
trim or claim it is responsible until evidence supports that conclusion.

## HID additions

Feature reports remain 64 bytes, report ID 1. Identity capability bit 6 means
both of these read-only commands are supported. No arbitrary memory access is
added. The source-owned adapter uses a 512-byte ring and records only errors,
with no allocation or printing. USB INFO supplies the frame timestamp; it
wraps with the bus frame counter, so the monotonically incrementing sequence
orders records within a boot.

- Opcode 11, request byte 4 = slot 0..15. Response bytes 4..15 contain total
  errors, overwritten records, and capacity (three little-endian uint32s).
  Bytes 16..47 contain eight uint32s: sequence, USB INFO, kind, endpoint,
  transfer length, SDK status, USB DEVCMDSTAT, USB INTSTAT. Unused bytes are
  zero. Invalid slots return header plus zero record; the ring is not cleared.
- Opcode 12 returns fourteen uint32s beginning at response byte 4: format
  version 1, ANALOG_CTRL_CFG, FRO192M_CTRL, FRO192M_STATUS, MAINCLKSELA,
  MAINCLKSELB, AHBCLKDIV, USB0CLKSEL, USB0CLKDIV, USB INFO, DEVCMDSTAT, INTSTAT,
  EPLISTSTART, DATABUFSTART. This is a fixed register allowlist, read only.

Error kinds: 1 notification completion length; 2 playback length; 3 microphone
length; 4 stream requeue failure; 5 notification queue failure; 6 initial stream
queue failure. The recorder preserves the previous counter behavior so existing
host tests still reject any new errors. It does not silently classify or suppress
an error as harmless.

Read after streams stop using `firmware/rebuild-re/read_audio_diagnostics.py`
with the exact build ID. It supports Windows hidapi and Linux hidraw and reports
whether the total changed while reading the ring.

## Offline checks

`test_audio_error_trace_arm.py` executes the compiled adapter. Twenty injected
zero-length microphone completions leave the newest sixteen records and four
overwrites. It checks ordering, fields, invalid-slot zeroing, and all fixed clock
snapshot offsets against mocked registers. Existing compiled adapter/halt tests,
124 boot-ack cases, 48 recovery cases, four startup cases, source/CRC/vector/RAM
and restore checks pass. Two build directories produce identical applications.
None of these tests proves real controller timing or the cause of stream errors.

## Cold-start hardware measurements

After the requested full power cycle, exact runtime identity, all 16992 code
bytes, SHA-256, and boot acknowledgement status 2 / driver 0 passed.
Evidence: release `hardware-20260908T182503Z.json`.

Read-only snapshot `audio-diagnostics-20260908T182454Z.json` reports
ANALOG_CTRL_CFG=1, FRO192M_CTRL=0x4177d3a0, FRO192M_STATUS=3,
MAINCLKSELA/B=0, AHBCLKDIV=0, USB0CLKSEL=3, USB0CLKDIV=1.
Thus the documented trim-source prerequisite is enabled; USB automatic
adjustment and 96 MHz output are enabled, with the USB divider set to two.
This is register evidence, not a frequency measurement. Error count was zero.
Windows had already opened playback automatically (alternate 1), even before
our explicit test. It is inaccurate to call this an entirely idle USB snapshot.

All ten Windows volume/mute cases passed again. Ten-second duplex delivered
1000 callbacks / 480000 captured frames, all zero, with no PortAudio status
flags. Firmware counters gained 9969 playback and 9999 microphone packets and
23 errors. The newest sixteen error records are all kind 3, endpoint 0x83,
length zero. Seven older records were overwritten, so their kinds are unknown.
This narrows investigation to zero-byte microphone completions; do not suppress
the errors or claim the underlying USB transaction fault is fixed.
Evidence: release `uac2-windows-20260908T182523.321022Z.json` and
`evidence/wsl-audio/audio-diagnostics-20260908T182539Z.json`.

Next hardware comparison: user asked to bypass the observed Generic USB Hub
and connect USB1 directly to a motherboard USB port. No further flash is
needed for that comparison. Installed diagnostic remains the research build.

## Direct motherboard port comparison

The user bypassed the hub. Windows now shows transmitter -> USB Root Hub ->
AMD controller (a different root/controller branch). Before testing, error count
was zero; FRO192M_CTRL=0x417ad3a0. Ten-second isolated duplex produced 999
callbacks, 479520 frames, no nonzero samples or PortAudio flags, 9999 microphone
packets and two new errors. Both records are zero-length microphone completions.
This is an observed reduction from the earlier 23 errors, not proof that the
hub alone caused them: controller and reconnection state also changed.

The full control test failed before audio because the first device-originated
volume/mute change did not reach Windows within three seconds. Firmware state
changed but notification completion count remained zero and pending=3. Later
reads showed two completed notifications, with another change still pending.
A targeted audio-interface restart returned 3010 (Windows restart required),
while PnP continued reporting the devices OK. No host reboot was issued.

Evidence: `audio-diagnostics-20260908T182803Z.json`, release
`uac2-windows-20260908T182814.238673Z.json`,
`stream-comparison-20260908T182905Z.json`, and
`audio-diagnostics-20260908T182915Z.json`.

A longer direct-port comparison produced 2999 callbacks / 1439520 frames in
30 seconds, all zero with no PortAudio flags. Firmware errors rose 2 -> 5 and
microphone packets 9999 -> 40005. Notifications had eventually completed
(pending zero, completed four), but their earlier multi-second delay remains
unacceptable. Evidence: `stream-comparison-20260908T183111Z.json`.
Windows restart requested to clear the OS-reported pending device restart.
After restart: verify exact installed identity/code, run the full volume/mute
and short duplex test, retrieve error records. Preserve failing evidence.
No additional firmware change or flash is justified solely by these results.

## Post-Windows-reboot result

User restarted Windows, retaining the same diagnostic application and direct
motherboard port. Full code readback and acknowledgement 2 / driver 0 passed
(`hardware-20260908T205716Z.json`). Firmware counters retained the earlier five
errors and 40005 microphone packets, so this was not a new MCU cold boot.

All ten native Windows volume/mute cases and the ten-second duplex test passed
(`uac2-windows-20260908T205735.940937Z.json`): 1000 callbacks, no nonzero captured
samples or PortAudio flags, error count unchanged at five. A subsequent
five-minute duplex test also passed (`stream-comparison-20260908T210310Z.json`):
29994 callbacks, 14397120 captured frames, 300003 new microphone packets,
300076 new playback packets, no host flags, and no new firmware errors.
No firmware was rebuilt or flashed for these results.

The clean-host/direct-port combination is now a passing baseline, not proof
that all failures were host-only. Hub/controller differences and reconnect
behavior still need separate validation. Five minutes of synthetic silent
streams does not satisfy the two-hour wireless-audio release gate.
