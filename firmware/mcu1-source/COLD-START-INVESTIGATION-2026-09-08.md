# MCU1 USB disappearance: static investigation

> **Implementation/recovery update:** Both established defects below are corrected
> in unflashed candidate `omni-a-10c7e4e03dfb3749`. The transmitter subsequently
> returned to MCU1 bootloader USB. A fresh full application capture matches the
> failed build exactly; metadata is now eligible/pending/ack 1/0/1, force 0.
> The recovery trigger remains uncertain. See `STARTUP-FIX-2026-09-08.md`.

## Updated conclusion after independent review and CPU emulation

Two concrete defects were missed by the initial review below:

1. **Missing successful-startup acknowledgement.** The update commit writes
   flags 0/1/0 at metadata offsets +12/+13/+14 and directly launches the application.
   Stock application routine 0x22680 acknowledges that launch by setting +14=1.
   Our source does not. The next cold boot therefore changes the flags to 0/0/0
   and selects the loader rather than the source application (assuming intact
   captured loader and expected metadata). Passing the vector presence check is
   necessary but not sufficient. This corrects the emphasis of the initial audit.
2. **USB functional clock enabled too late.** The frozen ELF reads USBFSH PORTMODE
   (0x400A205C) at PC 0xC248, before its clock setup call at 0xC258 clears
   USB0CLKDIV.HALT at 0xC7D2. HALT resets to one. The pinned NXP example starts
   that clock before PORTMODE; the bootloader USB branch does too. A warm launch
   can inherit the working clock, hiding the error. A cold direct application
   launch can stall at the earlier register access. The earlier check that the
   clock routine exists was insufficient: its position is the defect.

The first issue predicts loader USB, not total USB disappearance. Later dial holds
can make the application eligible again, exposing the second issue. We therefore
have specific defects and a plausible mechanism for later failed application boots,
but **not a proven full explanation of the first missing physical-recovery USB**.
No current PC, clock state, or flash/metadata readback is available.

### Evidence and limits

- Independent reports: `../rebuild-re/evidence/boot-update-review.md` and
  `../rebuild-re/evidence/source-startup-review.md`, with exact instructions,
  pinned SDK references and NXP documentation links.
- `../rebuild-re/emulate_boot_metadata.py` executes captured Thumb fragments
  using Unicorn 2.1.4. Commit plus five cold-selection cases passed, covering
  unacknowledged, acknowledged, previously failed, forced dial recovery and
  force-cleared long-hold states. Output:
  `../rebuild-re/evidence/cold-start-audit-20260908/metadata-emulation.json`.
  Flash wrapper/flush operations are modeled; USB and physical flash are not.
- Actual updater page footprint is [0xC000,0x51C00), plus the loader metadata
  page [0x7F800,0x7FA00). The final 84 bytes beyond the application payload are
  preserved by read/modify/write. No reviewed bounded update path overlaps boot
  or named factory pages, but there is no post-commit metadata readback.
- A fresh read-only host check found no transmitter in usbipd or Windows PnP;
  WSL lsusb listed only root hubs. USB forwarding is not hiding it in that check.

### Revised first SWD observations

Preserve PC/fault/reset state, boot flash and metadata before any restore. If the
application is selected, inspect USB0CLKDIV (0x40000398) and USB0CLKSEL
(0x400002A8), looking for a stop near PC 0xC248. **Do not read PORTMODE first if
the functional clock is stopped**, since that transaction is the suspected hang.
Reset-assisted attachment may be needed for a stalled bus. If metadata selects
the loader and PC is there instead, investigate loader initialization separately.

Both defects need correction before a future firmware trial: order clocks before
register access, implement and verify the startup-acknowledgement contract,
retain useful startup/fault evidence, and implement software recovery. No firmware
source or hardware was changed during this review; the failed release is preserved.

## Initial review (superseded where noted above)

The failure followed the first independent application trial, but the exact cause is
not established. A cold-start defect in the application is plausible; by itself it
does not explain why a correctly entered, intact physical bootloader also fails to
enumerate. Do not call a particular clock, watchdog, USB RAM, or flash defect proven.
No hardware writes, resets, or SWD operations were performed in this investigation.

## Evidence rechecked

`../rebuild-re/audit_cold_start.py` audits the frozen release rather than assuming
the current working source matches it. All ten checks passed. Results and selected
instruction-level disassembly are in `../rebuild-re/evidence/cold-start-audit-20260908/`.

- Release `omni-a-29585df231c9362f` and its source snapshot match the manifest hashes.
- Application CRC and code hash match; all logged blocks are contiguous and cover
  exactly [0xC000, 0x51BAC). These are host-side and historical checks, not fresh
  evidence that boot/factory flash is unchanged today.
- Initial SP is 0x20030000 and reset vector is 0xC131. All handler vectors point to
  Thumb code inside the executable extent.
- Captured boot function 0x3A50 reads the first eight application bytes and checks
  that neither word is 0xFFFFFFFF. Instruction disassembly confirms this. Our image
  passes this particular check; it is not rejected there for lacking a stock vector
  checksum or having a different reset-handler offset.
- Historical hardware log records matching running identity and all 11784 code/data
  bytes. No successful cold boot was recorded. That distinction remains essential.

## Startup dependencies inspected

1. Main RAM gates: boot reset at 0x1BC writes 0x78 to SYSCON AHBCLKCTRLSET0
   (0x40000220), enabling the four additional SRAM banks. Stock application's
   SystemInit repeats this. Our startup omits it, but it is already done on the
   captured bootloader reset path. Missing RAM gate setup is therefore not supported
   as the cause under a normally executing, unchanged bootloader.
2. USB RAM gate: the pinned SDK's CLOCK_EnableUsbfs0DeviceClock ends by enabling
   both kCLOCK_Usbd0 and kCLOCK_UsbRam1. Our board_init calls it before clearing
   [0x40100000,0x40104000). A missing USB RAM clock was investigated and not found.
3. USB pin setup: boot startup writes 0x107 to IOCON PIO0_22 (0x40001058) before
   the later application/USB-loader choice. Our board_init has no IOCON setup of
   its own, but the inherited setting is not exclusive to the USB update branch.
4. Clocks: our source explicitly enables FRO12/FRO96, selects FRO12 for CPU/AHB,
   and uses the SDK USBFS clock routine for a 48 MHz USB clock. The routine also
   enables USB clock adjustment. No simple missing USB clock gate was identified.
   Actual oscillator state, calibration and timing remain unmeasured.
5. Watchdog: source feeds an inherited watchdog but never establishes or records
   its configuration. Fault_Handler stops feeding and loops. Its only fault value
   is in BSS and disappears when the application restarts. This is a diagnostic
   deficiency and a candidate dependency, not evidence of an observed reset loop.
6. Handoff: boot routine 0x3CB4 disables/clears external IRQs, loads MSP/PSP from
   the application, and branches to its reset vector. It also writes the reset
   handler pointer to VTOR rather than the vector base. Our reset immediately
   masks interrupts and fixes VTOR to 0xC000. This loader behavior deserves attention
   when debugging exception timing, but does not explain failure of recovery before
   application handoff and is not established as the fault.

SDK inspection used the pinned local dependency tree under
`/srv/ai-agent/work/steelseries-arctis-nova-pro-omni-firmware/mcu1-source/vendor`:
`devices/LPC5500/LPC55S69/drivers/fsl_clock.c` (CLOCK_EnableUsbfs0DeviceClock),
`drivers/fsl_power.h`, `devices/LPC5500/LPC5528/system_LPC5528.c`, and
`devices/LPC5500/periph/PERI_SYSCON.h`.

## Why recovery is the distinguishing observation

Captured boot startup samples GPIO0_0 and writes boot-control metadata before the
application handoff. Physical recovery still uses substantial stock initialization,
stored settings and the bootloader's USB stack; it is not simply the NXP ROM ISP.
The normal-application and USB-update paths also leave different peripheral state.
Success after an update consequently does not establish cold-start independence.

If boot flash is intact, reset reaches that code, and the dial is sampled correctly,
the replacement application's lack of USB/audio/display initialization alone should
not prevent the forced loader path. The reported failure of both paths means at
least one of those assumptions, or later loader initialization/USB operation, must
be checked directly. The headset connection sound does not prove MCU1 is executing.

The earlier statement that a cold-start firmware bug caused the whole persistent
failure was too definite. The chronology is established; that specific mechanism is not.

## Next discriminating measurements with the 1.8 V SWD probe

First acquire evidence without erase/unlock/program actions:

- Establish MCU1 identity, then capture PC/SP, xPSR, CONTROL, VTOR, CFSR/HFSR,
  fault address registers, reset-cause status and watchdog state. Peripheral reads
  with clear-on-read behavior must be identified before collecting them.
- Read current [0,0xC000), application [0xC000,0x51BAC), reserved settings and
  metadata at 0x7F800. Compare against the captured baseline and frozen release.
  Metadata differences require decoding; they are not automatically corruption.
- If PC is in our Fault_Handler, inspect the stacked fault frame before reset.
  If repeatedly resetting, catch reset and locate the first failing operation.
- If PC is in the bootloader, identify the exact branch/wait and inspect GPIO0_0,
  its pin configuration, boot flags and the USB peripheral state.
- If normal attach fails, use reset-assisted attachment as a separate documented
  step. Establish reset reaching the bootloader and sample the physical dial signal.
- A controlled stock/diagnostic application restore, once flash state is preserved,
  can distinguish an application-dependent failure from persistent loader/hardware
  trouble. Replacing the app is not proof of cause unless cold boot is retested.

No replacement fix is justified for flashing yet. Preserve the failed build and
current target evidence; do not cover up the fault with speculative changes.
