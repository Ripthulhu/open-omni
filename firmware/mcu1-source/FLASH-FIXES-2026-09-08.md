# Corrected MCU1 source firmware hardware trial

The user explicitly requested flashing the firmware with the fixes. Target was
the transmitter's **MCU1 LPC5528**, bootloader **1038:2291**, update target **1**.

Installed build: `omni-a-5c74e71366c8bb50`.
Application image: 285612 bytes at 0xC000, SHA-256
`b94d7978831596b131eab99aa3446af313d5e2125e6b1e957f47ccd837e440ed`.
Running code/data: 13360 bytes, SHA-256
`9af7de2831491cff1b9e46c06cbaef1268ff0b65fcff73cf021019496e8c2fc9`.

## Verified hardware results

- Preflash image/manifest/CRC/source/restore checks passed. Fresh application
  vectors and metadata matched the preserved preflash state; metadata flags were
  eligible/pending/ack 1/0/1, force zero. No SteelSeries/Omni-named process was
  running in the inspected process list.
- MCU1 received the image with each block acknowledged. Both bootloader CRC
  values matched, and all 285612 staged bytes read back exactly before commit.
- The application enumerated with its unique build serial. Runtime identity and
  every code/data byte matched. Startup acknowledgement was **1 (written and
  verified), driver 0**. A separate live diagnostic read reported fault field zero;
  that field is not persistent across resets.
- The user confirmed unplugging every USB/power cable for 30 seconds, then
  reconnecting USB1 normally without holding the dial. Read-only enumeration
  observed absence and return of the correct application. Full runtime code
  verification passed again; acknowledgement was **2 (already valid), driver 0**.
  This is **one verified normal cold-start trial**.

Evidence is in `releases/omni-a-5c74e71366c8bb50/`:

- `preflash-20260908T161608Z.json`
- `hardware-20260908T161905Z.json` — staged image, commit, warm runtime
- `health-20260908T161941.244829Z.json` — live fault and acknowledgement
- `usb-cycle-20260908T162034Z.json` — observed absence/return
- `hardware-20260908T162051Z.json` — post-power-cycle runtime

## Physical recovery

After the successful cold-start trial, the user confirmed a fresh 30-second
power removal and approximately four-second dial hold. The bootloader collection
was absent when queried; subsequent HID and Windows PnP enumeration instead
showed the correct source application at 1038:2290 with its matching build serial.
No return/commit command was needed or sent. This attempt did not establish
physical recovery, but the application remained available. One five-second hold
was then tested after another user-confirmed 30-second power removal.

**The five-second hold entered MCU1's real 1038:2291 bootloader.** Its application
vectors matched the installed build. Readback of the full 512-byte metadata page
showed flags 1/0/1, force zero and SHA-256
`e40ab2b054b52ca1df02ef4e3f98e146e7cc65a130218f5af60bb1f9005c7e34`.
The whole page exactly matched the preflash page after the completed boot/ack
cycle. Evidence: `recovery-20260908T162544Z.json`.

All 13360 installed code bytes were also verified through bootloader readback.
The reviewed standalone `01 01 00 01` command was issued once to return to that
existing application, without restaging or writing an application image. This
command performs the loader metadata transition and starts a new acknowledgement
cycle. Its host write returned short while the loader disconnected, so the first
return script correctly recorded an uncertain outcome and stopped. **It was not
retried.** The source application appeared, and an independent verify-only run
confirmed the exact build and full code again, with acknowledgement **1 (written
and verified), driver 0**. This resolves the return outcome as successful while
preserving the original short-write evidence.

Evidence: `return-from-recovery-20260908T162657Z.json` (original uncertain host
result and pre-return code readback) and `hardware-20260908T162731Z.json` (successful
independent runtime verification). **Final state: the fixed source firmware is
running.** This proves physical loader entry and return to the installed app;
it does not claim a fresh stock/diagnostic restore trial after this fixed build.

## Scope and remaining work

This is the diagnostic milestone application. It does not implement audio,
display/dial behavior or MCU2/DSP startup. MCU2, the DSP and headset firmware were
not replaced; the volume bridge remains paused. Host image writes stay within
the approved MCU1 application envelope. The loader and startup acknowledgement
also update their verified metadata page at 0x7F800.

The successful cold boot is not a completed reliability campaign. Software
recovery entry, persistent fault evidence, live analog trim/ROM-gate contract
checks, repeated cold boots and the later audio/control milestones remain open.
The release manifest retains its original packaging-time validation state;
these timestamped hardware records describe what has actually run.
