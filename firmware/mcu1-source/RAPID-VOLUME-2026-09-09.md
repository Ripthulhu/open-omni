# Rapid Windows volume USB failure

User reported moving only Windows slider can wedge USB while dial/display remain responsive. NativePnP nodes stillOK. Read-only HID identity failed. Initial passive traceOmniUsb-20260908T221103Z showed repeated SET_CUR volume and clockGET_CUR cancellations, USBstatus0xc0010000/NTSTATUS0xc0000120, not an observedSTALL handshake. Targeted PnP restart returned3010(rebootrequired). User fullpowercycle restored operation.

Traced reproduction on unchanged98e1652bb982bd11: stress_volume_windows.py count1000 pace0, triangular0..100% Windows scalar writes, no active audio streams created by test. Updates0..696 completed(~3ms/call), update697=97% timed out after2s. User confirmed display remained96%. Subsequent GET_CUR and restoreSET_CUR also failed. Workerexit1, noimageflashed. Evidence rapid-volume-20260908T221529Z.jsonl and OmniUsb-20260908T221521Z_000001.etl plus parsed XML/JSON. Trace stopped normally. Failure is in USB control-request progress before97% reachesapplication volume; exact cause not established.

Research candidatea25679854cf28603 adds main-loop SETUP guard. If attachedUSB has SETUP received but EP0OUT interrupt absent and enabled continuously2ms, it saves the first register/EP0descriptor snapshot and re-signals only EP0OUT through documentedINTSETSTAT. At least10ms between attempts, watchdog/mainloopkeepworking. NoUSBreset/flash/volumechanges in guard. Opcode14 exposes version1 evidence. This is an invariant-based diagnostic/recovery candidate, not a proven cause/fix until its counter and stress result are observed. It will not repair a different control-pipe failure with no pendingSETUP flag.

UM11126 rev2.8 tables USB0 DEVCMDSTAT/INTSTAT/INTSETSTAT and pinned PERI_USB.h document the separate flags and software interrupt set register. The pinned NXP USB ISR dispatches SETUP only from a captured EP0OUT interrupt bit. No vendor files modified. Two builds match SHA256 `15f8d96a000eb3faaf1a63dab7e772e6c1103f76001f2d98d5f6e6cf72e4fdd2`. Compiled guard tests cover normal states, 2ms qualification, 10ms backoff, wraparound, retained first snapshot and only the EP0OUT write; existing startup/recovery/audio/UI/percentage tests pass.

## Hardware result

After the user entered physical MCU1 recovery, exactly one flash installed `omni-a-a25679854cf28603`. Full staged and running code readback passed, with boot acknowledgement 1 / driver status 0. Evidence: release `hardware-20260908T222455Z.json`. MCU2, DSP and bootloader code were not flashed.

| Trial | Result | Evidence |
|---|---|---|
| 5,000 rapid Windows volume updates, no test audio | All complete in 15.047 seconds; HID readback and state restoration pass; zero audio errors; guard repairs increase 0 to 11 | `rapid-volume-20260908T222536Z.jsonl` |
| Another 5,000 updates with silent duplex audio | All volume updates complete and HID remains responsive; **overall failure: 36 new microphone errors** | `rapid-volume-20260908T222642Z.jsonl` |
| Ordinary native volume/mute test afterward | Five Windows-to-firmware cases pass; first device-to-Windows case fails with pending 3, completed notifications 0 | release `uac2-windows-20260908T222811.733952Z.json` |

The first guard snapshot proves the targeted condition occurred: DEVCMDSTAT `0x10010183` (SETUP pending and device attached), INTSTAT `0x40000000` (EP0OUT absent), INTEN `0x800003ff` (EP0OUT enabled). EP0 OUT/IN descriptors were `0x6b`/`0x6d`. Resignalling restored progress without USB reset. This identifies a recovery mechanism for the reproduced missing-interrupt condition; it does not yet identify why the original interrupt was lost.

The successful trial's bounded ETW capture `OmniUsb-20260908T222538Z_000001.etl` selected 8,843 events with no errors in the parser's error classification. Capture started after the burst began, so it is not a complete trace of all 5,000 requests. All raw and parsed evidence remains under `../rebuild-re/evidence/wsl-audio/`.

Audio diagnostic `audio-diagnostics-20260908T222731Z.json` retains 16 of the 36 zero-length microphone IN completions (kind 3, endpoint `0x83`); 20 older records were overwritten. No callback statistics were retained for the failed simultaneous trial because the original test raised before writing them. The stress tool now records final audio statistics even on failure. Read-only guard snapshot `usb-guard-20260908T222730Z.json` still shows 11 repairs.

**Open:** microphone errors under control traffic, device-originated notification reliability, cold/reconnect lifecycle, and wireless audio. No additional flash or hardware stress is justified merely to repeat these observations. All tests/captures stopped; user released from hands-off request. USB remained accessible at the last diagnostic read.
