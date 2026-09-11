# USB offline audit — 2026-09-09

## Result and limits

The installed `omni-a-a25679854cf28603` image contains a reproducible concurrency hazard in the pinned NXP controller driver: initializing/deinitializing one endpoint can overwrite another endpoint's hardware buffer selector. A compiled-instruction test reproduces the lost selector. This is **not yet the proven cause** of the recorded notification failure or microphone errors, and does not justify another isolated flash.

The next feature image now includes the endpoint snapshot described below. Keep implementing inter-chip startup/audio in parallel. Do not treat a speculative USB fix as a prerequisite for offline feature development.

The initial audit used the installed application binary, primary device documentation, the pinned SDK, and existing evidence. Subsequently, the source-owned audio adapter and diagnostic dispatcher gained opcode 15 and bounded counters for the next feature batch. The vendor checkout and endpoint behavior were not changed. This agent performed no live device operations.

## Implemented diagnostic and integration checks

Opcode 15 accepts endpoint `0x03`, `0x82`, or `0x83` and page 0–3. Each response starts with four words: version 1, page, endpoint, flags. Flag 1 means the validated private controller state belongs to USB0; flag 2 means the descriptor list passed the SRAM/alignment bounds; flag 4 means repeated hardware selector/interrupt/descriptor reads matched. Hardware still runs while CPU interrupts are masked, so flag 4 is not an atomicity guarantee.

- Page 0: controller registers and both descriptor words, with before/after fields.
- Page 1: driver transfer/producer/consumer state, stored lengths, queue/callback/cancel counters.
- Page 2: notification busy/age/revisions/pending, last queue/completion result, configuration/alternates.
- Page 3: endpoint open/close/halt/clear-halt and transfer counters.

`../rebuild-re/read_usb_endpoints.py BUILD_ID` is the read-only Windows/Linux reader. Private driver, handle, and descriptor-list pointers are bounds/alignment checked before dereference. Invalid endpoint/page requests return status 2. Snapshot reads do not modify controller registers.

Final integrated candidate `omni-a-6f7d35b4a486f338` passed `test_usb_endpoints_arm.py`: 24 valid endpoint/page/PRIMASK combinations, five invalid request cases, 19 invalid pointer/register-base cases, hardware-change detection, and zero USB MMIO writes. Evidence: `releases/omni-a-6f7d35b4a486f338/usb-endpoints-regression.json`.

The independent compiled UART/probe test `test_mcu2_uart_arm.py` also passed against that candidate: FC7 vector/pins/divisors/8N2, sixteen-byte IRQ budget, ring ordering and overflow, TX backpressure, line/FIFO failures, shutdown, exact 1036-byte request/response, no automatic retry, 250 ms/wrap timeout, cancellation, and eight actual main-loop gating cases. Six HID guard cases and opcode 15 dispatch were exercised. The HID stream-active rejection branches are source/disassembly-reviewed only because this Unicorn version fails at their taken backward BNE.W; those branch cases are not counted as passing emulation. The independent compiled **main** stream/disconnect gates pass. Evidence: `releases/omni-a-6f7d35b4a486f338/mcu2-uart-regression.json`.

ISR timing and first-anomaly capture before microphone requeue remain proposed additions, not implemented claims. UART wire behavior and Windows behavior require separate hardware evidence.

## First hardware snapshot interpretation

Parent-agent captures `../rebuild-re/evidence/wsl-audio/usb-endpoints-20260908T225451Z.json` and `usb-endpoints-20260908T225547Z.json` came from the running 6f build before and after its single MCU2 query. Every page has valid pointers and matching hardware reads. Apart from INFO/frame values, endpoint state is unchanged across the query.

EP82 is internally consistent and idle: hardware selector zero, software producer/consumer zero, no outstanding transfer, no pending volume notification, no queued or completed notifications, both descriptors inactive, and no pending endpoint interrupt. One clear-halt event was recorded. These passive captures do not test notification delivery.

Playback/microphone each show two open/close/cancel cycles, with no current alternate or active transfer, max-packet size zero, and descriptor zero disabled. The remaining inactive second descriptor and remembered transfer lengths are retained driver state after deinit; they are not evidence of a live stream failure. The parent recorded zero audio errors.

The new baseline does not reproduce a256's earlier EPINUSE bit 5 equal to one. The old capture lacks the descriptors and software selectors needed to explain that value. No selector repair is justified by the new captures. The MCU2 query itself timed out with 1036 bytes submitted and zero received; it did not establish working inter-chip communication, and produced no visible USB endpoint-state change.

## Inputs

- Application SHA256: `15f8d96a000eb3faaf1a63dab7e772e6c1103f76001f2d98d5f6e6cf72e4fdd2`.
- NXP USB revision: `b330ba1e98bec0faa1b97fdf772bf81803a05868`; device support `fdf4429f3dda7f427bf4ef438a0e85fca083e178`.
- Local official `../rebuild-re/official-docs/UM11126-rev2.8.pdf`, chapter 40: sections 40.4.7, 40.7.7–40.7.9, 40.8.1, 40.8.4. Use section names; the extracted table-of-contents numbering is inconsistent with body numbering.
- `releases/omni-a-a25679854cf28603/uac2-windows-20260908T222811.733952Z.json`.
- `../rebuild-re/evidence/wsl-audio/audio-diagnostics-20260908T222731Z.json` and `usb-guard-20260908T222730Z.json`.
- Executable audit: `../rebuild-re/audit_usb_dci_offline.py`.
- Result: `releases/omni-a-a25679854cf28603/usb-dci-offline-audit.json`.

## 1. Demonstrated unrelated-endpoint selector overwrite

The LPC5528 driver enables double buffering. `EPINUSE` selects the next hardware buffer separately for each physical endpoint. UM11126 says hardware toggles the corresponding bit when it clears a completed buffer's ACTIVE bit; software can also force the selector by writing the register.

In pinned `usb_device_lpcip3511.c`, `USB_DeviceLpc3511IpEndpointInit` (around line 532) and `USB_DeviceLpc3511IpEndpointDeinit` (around line 693) reset one selector using a whole-register read/modify/write:

```c
registerBase->EPINUSE &= ~(1UL << endpointIndex);
```

Disabling CPU interrupts does not prevent USB hardware from updating another selector during those instructions. The test calls the **compiled** endpoint-deinit function for microphone endpoint `0x83` with an idle transfer. It schedules an unrelated endpoint `0x82` hardware completion immediately after the actual load of `EPINUSE` at application address `0xF6E6`.

| Scheduled event | EP82 bit 5 after deinit | Result |
|---|---:|---|
| EP82 completion before the CPU reads EPINUSE | `0x20` | Preserved |
| EP82 completion after the CPU reads EPINUSE, before it writes | `0x00` | Hardware selector overwritten |

The raced completion should leave bit 5 equal to one. The compiled function writes the stale zero back while clearing only its intended bit 7. An affected endpoint can then have hardware waiting on an inactive buffer while the driver queues its other buffer. The driver also modifies EPINUSE while canceling active double buffers; those paths deserve the same review.

**Scope:** the harness models this register interleaving, not complete USB hardware timing. It neither proves the event occurs frequently nor proves it occurred in our capture. No host traffic was sent by the harness.

**Why this does not settle the recorded notification failure:** EPINUSE was already `0x20` in the guard's first snapshot during the initial Windows-only burst, before the deliberate duplex test. The application had no completed notification count. That earlier state cannot be attributed to the later microphone close. We need both endpoint descriptors and driver producer/consumer selectors to determine whether it is inconsistent.

**Potential correction after confirmation:** avoid whole-register writes to live endpoints' hardware selectors. One approach is to initialize the affected endpoint's software producer/consumer selectors from its current hardware selector, keeping that value through deinit/reinit, instead of forcing zero. Canceling two buffers and initialization/reset paths must be covered before accepting this approach. Another approach is single buffering with a complete matching driver configuration; simply changing EPBUFCFG underneath a driver that still alternates buffers is invalid. Neither correction was applied here.

## 2. What microphone length zero actually establishes

On LPC5528, `FSL_FEATURE_USB_VERSION=200`. Consequently the pinned `USB_DeviceLpc3511IpTokenUpdate` computes completion length for **IN as well as OUT** from the endpoint descriptor's remaining byte count. It does not blindly report the submitted IN length.

The compiled ISR test supplies a 96-byte microphone transaction, an inactive completed descriptor, one queued buffer, and a microphone endpoint interrupt:

| Descriptor bytes remaining | Actual callback length |
|---:|---:|
| 0 | 96 |
| 96 | 0 |

Both cases execute the real compiled ISR and reach the normal notification callback. The latter follows directly from an inactive descriptor whose bytes were not consumed. A skipped transfer, a stale/incorrect buffer selection, or another controller state problem must be distinguished using the missing descriptor/state evidence. The test does not assert that hardware naturally generates this particular state without an external event.

The 16 retained hardware error records all have `INFO.ERR_CODE=9` (last sent/received NAK). This is a shared controller field, not a per-microphone error result. It does not establish an overrun, underrun, or clock failure. The application records `status=1` itself when callback length differs from 96; that value is its synthetic error status, not an additional error supplied by the USB controller.

Keep the stress result failed because the microphone callbacks were wrong. Describe it as 36 zero-length microphone completions; these records alone do not quantify lost USB frames or prove CPU overload.

## 3. CPU timing is a measurement task

UM11126 section 40.4.7 specifies a minimum 12 MHz CPU clock while USB0 receives or transmits. The current 12 MHz main clock meets that requirement. This does not establish enough processing margin for future DSP drivers or bursts of control requests.

Current traces contain neither ISR execution duration nor time from microphone completion to re-prime. Raising the clock solely because errors occur under load would therefore be a hypothesis. A clock change must also update SystemCoreClock, SysTick, delay routines, flash wait states/power requirements, and the retained ROM flash operation's clock contract. Keep the existing conservative flash path until those dependencies are handled deliberately.

For the next feature image, collect bounded maximum USB ISR duration and maximum active-stream re-prime gap with a monotonic hardware cycle counter. Report the clock frequency and counter-wrap handling alongside the values. Count durations above the 1 ms full-speed frame interval. Instrumentation must not print, allocate, write flash, or wait in the IRQ. The audit's instruction counts are **not** cycle timing and cannot substitute for this measurement.

## 4. Small diagnostic addition for the next planned image

The implemented fixed snapshot covers the following fields. It runs in one existing USB callback so software state cannot be observed halfway through another USB ISR. USB hardware still runs during the snapshot; changed selector/interrupt/descriptor reads leave the matching-reads flag clear.

Include:

- Application `notify_busy`, queued selector, sent revision, current revision, pending bits, completion count, queue result/count, and age of the outstanding notification.
- Both endpoint command words, EPINUSE, EPBUFCFG, EPSKIP, INTSTAT, INTEN, INFO, and EPTOGGLE.
- Driver state word (includes producerOdd, consumerOdd, doubleBufferBusy, transferring, stalled), both stored transaction lengths, transferLength, transferDone, and transferPrimedLength.
- Counters for init/deinit, halt/clear-halt, cancellation, send queue, and completion; a small first-anomaly snapshot is preferable to continuous logging.

These values distinguish the next action without another speculative flash:

| Observed state | Interpretation / next evidence |
|---|---|
| `notify_busy=1`, selected descriptor inactive, driver still transferring, no endpoint interrupt | Possible lost completion; inspect the retained first anomaly and interrupt history. |
| Hardware selector points to inactive buffer while other buffer is ACTIVE and software expects it | Buffer selector mismatch; compare lifecycle counters and the RMW hazard. |
| Selected notification descriptor ACTIVE, driver state agrees | Transfer is still queued; inspect host interrupt-IN polling/toggle evidence before changing device state. |
| `notify_busy=0`, pending bits nonzero | Check poll/queue counts and last return value; does not require assuming a dead host pipe. |
| Microphone completion length zero | Preserve descriptors and software state **before requeue**, since requeue overwrites the evidence. |

Opcode 2 in the previously installed a256 build permits application-flash readback only. Its existing HID diagnostics therefore cannot retrieve these RAM/controller fields by choosing another address. Opcode 15 was added to the integrated feature/diagnostic batch for this purpose.

## Reproduce offline

From the repository with the existing analysis Python dependencies:

```text
python firmware/rebuild-re/audit_usb_dci_offline.py firmware/mcu1-source/releases/omni-a-a25679854cf28603 --output firmware/mcu1-source/releases/omni-a-a25679854cf28603/usb-dci-offline-audit.json
```

Four compiled-code checks pass: non-raced selector preservation, raced selector loss, consumed 96-byte microphone completion, and unconsumed zero-byte completion. The test's `passed` means the audit reproduced those contracts, **not that the firmware is free of defects**.
