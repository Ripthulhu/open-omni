# Source inter-chip and audio primitives — 2026-09-09

## Installed DSP inquiry and hardware result

`omni-a-1f282d1790403701`, SHA256 `8d1299f5d17b622493171353bd253e68caac100d02c40a607b90cda32373e430`, is installed with the independent DSP USART3 query and shared bounded UART backend. One software recovery from6f and one flash passed full staged/running readback, ACK1/driver0, code25024 bytes (`hardware-20260908T231921Z.json`). The earlier6f MCU2 timeout below remains historical evidence.

The single DSP inquiry established communication:4 bytes TX,13 bytes RX, one well-formed frame, no UART FIFO/line/ring errors, no parser malformed/expired bytes. The captured prefix is `DB0DE10300360000`. The installed response matcher incorrectly required exactly7 bytes, so it rejected the longer reply and timed out. The raw failed result remains unchanged in `releases/omni-a-1f282d1790403701/dsp-status-20260908T232042.369666Z.json`. Only the first8 bytes were retained; the remaining5 are unknown. The three version/status fields `003600` agree with captured stock RAM. The seven-byte emulation input used earlier was a minimal synthetic fixture, not a wire capture or a maximum-length rule. A source correction is being prepared without another parser-only flash.

PIO1_7 was already HIGH/output and PIO0_17 already input before preparation. Both prior IOCON words were0x100. This trial does not establish that GPIO preparation changed the peer's state. A separate once-only MCU2 trial on this build still produced1036 TX/0 RX, no UART errors, then stopped. Saved port7 configuration confirms USART mode,8N2 (CFG0x44 after disabling), OSR12/BRG0, FRO12 clock selector2/FRG255, TXIOCON0x121 and RXIOCON0x127. Port3 had the same baud basis with8N1 and IOCON0x121 on both pins. MCU2 silence remains unexplained; do not assume peer power or startup as the cause. Evidence: `mcu2-status-20260908T232131.633921Z.json`.

After DSP testing the passive USB endpoint read at232131Z remained configured with both audio alternates0 and notification endpoint82 idle: pending0, busy0, matching selectors0. This read does not prove notification delivery or clear the previous USB/audio faults. No automatic retry, peer firmware update, audio activation or additional volume stress occurred.

Stock instructions prove `BD04E102` is the periodic DSP status inquiry. The corrected instruction-level oracle establishes that E1 handling at0x2716C reads **all nine bytes4..12**, even when its synthetic packet declared only7 bytes. The original fixture silently supplied zero-filled memory beyond the packet; it was not a valid complete reply. Source now requires the observed `DB0DE103` thirteen-byte form and retains all nine opaque data bytes. Exact13 is a conservative source policy; stock does not itself validate E1 length. Missing/truncated7/12-byte records and unobserved longer variants are rejected. The corrected host tests pass; the next combined ARM build will validate the updated compiled probe. UART3 uses the recovered TXPIO0_2/RXPIO0_3 and8N1, while UART7 remains8N2. The two ports share one bounded receive ring and cannot run simultaneous probes.

Before this explicit DSP query, source reproduces the unconditional stock startup preceding both UART initializers: PIO1_7 output HIGH (latch before direction), PIO0_17 input. Stock raw IOCON0x4100 includes reserved bit14; source expresses documented digitalGPIO settings0x100. Prior pins, directions and IOCON values are retained. The physical role of these pins remains unproven; HIGH may release a held peer reset. There is **no LOW pulse**, arbitrary GPIO command, mode change, gain change or persistent write. Two seconds of cooperative settling precede the single250ms query. This settling time is source policy. No automatic retry follows failure.

HID18 uses nonzero LE32 token and `DSP1` guard;19 has the same15-word status layout as17, plus stage4 for settling. HID20 takes port3/7 and page0/1 and returns version/port/page plus ten fixed configuration words. HID21 returns version, first unexpected length, eight raw header bytes, five parser counters, then six pre-start GPIO words. `probe_dsp_status.py RELEASE --execute` sends one request and saves these diagnostics even when the response check fails. All probe paths continue to cancel on audio activation, recovery or USB deconfiguration.

The recovered48/96k audio transition is now a separate inactive `audio_mode.c` state machine. It sends the exact stop/gate/resume/rate frames, preserves20ms inter-frame spacing,50ms initial and75ms quiet/settle phases, and reports only local completion. Configuration failure, timeout or cancellation isolates pins; there is no claimed DSP acknowledgement. The source has not activated PLL/I2S/DMA or routed wireless samples.

Ten host suites pass. Final compiled UART/DSP/MCU2 tests cover both ports, selection-lock rejection, GPIO ordering/no LOW writes, settling/timeouts, partial frames, peer exclusion, USB/audio cancellation and fixed read-only snapshots. Recovery/startup/USB guard/UI checks and reproducible builds pass. See the release JSON evidence and exact stock emulators. A decompiler omission of consecutive PLL writes was detected and corrected using a53-write instruction trace; no inherited-PLL-power defect is claimed.

## First MCU2 inquiry candidate and result

Candidate `omni-a-6f7d35b4a486f338`, application SHA256 `41df38fca0777ad020177b838787f4d78b15b57d771ce4d695e97c061d80fb99`, adds an explicit MCU2 status transaction and fixed USB endpoint diagnostics. DSP framing and audio conversion/DMA planning compile independently and pass host tests; they are not yet connected to live audio. No stock MCU1 application functions are called.

## Implemented

- `interchip.c`: bounded DSP byte parser and encoders, recovered from stock Thumb execution. RX accepts DB, consumes/discards DD, and validates05/15 RACE headers and lengths. Unlike stock dispatch, the parser preserves the raw15 marker for evidence; consumers must normalize when dispatching. Rejects malformed lengths instead of copying stock's unsafe clamping. No inferred command semantics or automatic retries.
- `mcu2_link.c`: cooperative, single-owner transaction. Exactly `BC04E102` followed by zero padding to1036 bytes. Success requires an entire1036-byte reply beginning `CB07E103`, three opaque response bytes, and zero padding. At most16 TX and32 RX bytes per poll;250ms absolute timeout. The timeout is our policy, not a recovered peer deadline.
- `mcu2_uart_lpc5528.c`: source-owned USART7, stock pins PIO0_20 RX / PIO1_30 TX, **8N2**. Independent12MHz input/OSR12/BRG0 computes923076.9 baud (+0.1603% against requested921600); stock baud-helper emulation selects the same divisors at12MHz. No global USB/CPU clock change or peer reset. A2048-byte IRQ receive ring captures a full response; overflow, FIFO or line error ends the transaction. IRQ work is bounded to16 bytes.
- `mcu2_probe.c`: UART stays inactive until a guarded HID request; **one query per MCU1 boot**, with same-token requests idempotent. Stops on completion/error, USB deconfiguration, audio stream activation, or recovery entry. A partial transmit can leave the peer waiting for more bytes; there is no blind retry or claim that MCU1 reboot resynchronizes it.
- `audio_bus.c`: exact little-endian PCM16/24 expansion into left-aligned I2S32; validated48/96kHz ten-block DMA plans for stereo32 playback and mono16 microphone. No peripheral accesses. Allocation, DSP mode transition and USB clock reconciliation remain the caller's responsibility.
- USB opcode15: fixed allowlisted endpoint snapshots and counters, to investigate notification/microphone faults within the next feature build. No endpoint behavior patch or vendor source changes.

## Diagnostic contract

The existing64-byte feature report format remains. Opcode16 enqueues the fixed MCU2 query: nonzero token LE32 at bytes4..7, ASCII `MCU2` at8..11. Requires successful boot acknowledgement, no recovery request and both audio alternate settings zero. Opcode17 reads15 LE32 words at bytes4..63:

| Word | Meaning |
|---:|---|
|0|version1|
|1|request token|
|2|stage:0 idle,1 queued,2 active,3 terminal|
|3|transaction:0 idle,1 running,2 success,3 timeout,4 IO error,5 cancelled|
|4..7|TX bytes, RX bytes, full frames received, rejected frames|
|8|three opaque reply bytes packed little endian|
|9..13|UART RX bytes, UART TX bytes, FIFO errors, line errors, dropped bytes|
|14|bit0 UART failure; upper16 bits count bytes in current/last frame|

`../rebuild-re/probe_mcu2_status.py RELEASE` reads status. Add `--execute` for the single request. It checks exact MCU1 build identity, saves success/failure evidence, and refuses a second transaction. The recovered MCU2 handler can clear a volatile flag/post a local event; it is not described as side-effect-free. No flash/reset/volume setter was found in that branch.

The protocol has no transaction identifier or checksum at this layer. A matching raw response proves the observed exchange, not full MCU2 health, DSP startup or audio readiness. Fresh packet alignment and physical baud tolerance require the hardware trial.

## Verification and next gate

Eight host suites pass, including exhaustive supported DSP lengths/types, malformed/truncated frames, backpressure/timeouts, MCU2 reply matching, signed PCM edges and DMA golden values from executed stock code. MCU1 stock21-case inter-chip emulation and audio configuration/descriptor emulation establish the reference bytes. Compiled ARM recovery/startup/USB guard/UI/volume checks pass, and two independently built candidate images match. UART/endpoint diagnostic compiled tests are recorded in the release.

Next hardware trial: full MCU1 flash/readback/boot acknowledgement, read passive USB endpoints, issue the single MCU2 query with no streams, preserve raw response and counters, then verify USB remains available. No volume stress or speculative audio activation belongs in that trial.

See [inter-chip protocol](../rebuild-re/INTERCHIP-PROTOCOL-2026-09-09.md), [audio path](../rebuild-re/WIRELESS-AUDIO-PATH-2026-09-09.md), and [USB audit](USB-OFFLINE-AUDIT-2026-09-09.md). Hardware results below will distinguish a prepared candidate from an installed build.

## Hardware result

The build is now **installed**. Software recovery from a256 verified the installed code, then exactly one MCU1 flash passed all block acknowledgements, CRCs, full staged image and22648-byte running code readback, ACK1/driver0. Evidence: `releases/omni-a-6f7d35b4a486f338/hardware-20260908T225429Z.json`.

The single MCU2 query **timed out**: all1036 bytes were accepted by the UART transmitter, zero RX bytes, zero FIFO/line errors or ring drops. The driver stopped at250ms and did not retry. The TX counter is not an external wire capture. Evidence: `mcu2-status-20260908T225452.389829Z.json`. Stock emulation and a captured stock RAM reply establish the protocol, but current peer startup/electrical framing remains unresolved. Do not infer successful inter-chip communication from this trial.

USB remained responsive. Passive endpoint snapshots at225451Z and225547Z were consistent before/after the query: notification endpoint82 idle, hardware/software selectors0, no queued transfer or volume notification pending. UI225549Z remained initialized/READY with12% muted, no audio streams/errors. Those reads do not exercise notification delivery or prove broader USB reliability. No additional volume stress, flash, peer reset or physical action was performed.
