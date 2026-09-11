# Reverse engineering

The transmitter has two MCUs and an Airoha DSP. MCU1 owns the custom application;
MCU2 and the DSP retain their stock firmware. Findings below distinguish decoded
code, captured protocol traffic and observed hardware behaviour.

| Area | Findings |
| --- | --- |
| Flash layout and recovery | [Boot layout](../firmware/rebuild-re/BOOT_LAYOUT.md), [initial captures](../firmware/rebuild-re/RESULTS-2026-09-08.md) |
| Documentation | [Official manuals and SDK references](../firmware/rebuild-re/official-docs/INDEX.md) |
| MCU2 link | [Startup gates](../firmware/rebuild-re/MCU2-STARTUP-GATES-2026-09-10.md), [receive handshake](../firmware/rebuild-re/MCU2-RX-STARTUP-HANDSHAKE-2026-09-10.md) |
| MCU2 controls | [Control contracts](../firmware/rebuild-re/MCU2-CONTROLS-CONTRACTS-2026-09-10.md), [native runtime](../firmware/rebuild-re/NATIVE-MCU2-RUNTIME-2026-09-10.md) |
| DSP settings | [Native settings](../firmware/rebuild-re/NATIVE-DSP-SETTINGS-2026-09-10.md), [native gain](../firmware/rebuild-re/NATIVE-DSP-GAIN-RESULTS-2026-09-10.md) |
| USB audio | [CPU clock correction](../firmware/rebuild-re/AUDIO-CPU96-RESULTS-2026-09-10.md), [high-resolution playback](../firmware/rebuild-re/AUDIO-HIRES-LIVE-2026-09-11.md), [USB cursor](../firmware/rebuild-re/USB-OUT-CURSOR-2026-09-11.md) |
| Microphone | [RX DMA and live capture](../firmware/rebuild-re/MICROPHONE-2026-09-11.md) |
| Mixer | [Line input and source mixing](../firmware/rebuild-re/MIXER-2026-09-10.md) |
| Charging | [Charger](../firmware/rebuild-re/BATTERY-CHARGER-IMPLEMENTATION-2026-09-10.md), [percentage error](../firmware/rebuild-re/BATTERY-PERCENTAGE-BUG-2026-09-11.md), [removal status](../firmware/rebuild-re/BATTERY-REMOVAL-UI-2026-09-11.md) |
| Headset battery | [Telemetry and shared UART](../firmware/rebuild-re/HEADSET-BATTERY-IMPLEMENTATION-2026-09-10.md) |
| Display and controls | [Buttons](../firmware/rebuild-re/LOCAL-BUTTONS-LIVE-2026-09-10.md), [idle timeout](../firmware/rebuild-re/DISPLAY-IDLE-FIX-2026-09-10.md), [menus](../firmware/rebuild-re/EXPANDED-MENUS-2026-09-11.md) |

Research scripts and dated reports remain in `firmware/rebuild-re`. Older exploratory
scripts under `firmware` and `qr-patch` aren't supported flash entry points. Use the
[portable flasher](../tools/flash/README.md) to install current builds.
