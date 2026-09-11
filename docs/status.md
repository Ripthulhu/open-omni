# Hardware support

The application runs on the transmitter's LPC5528 (MCU1), linked at `0xC000`.
Updates target MCU1 only. MCU2, the Airoha DSP, bootloader code and factory settings
are retained. The stock loader owns the acknowledgement metadata page at `0x7F800`.

| Feature | Status |
| --- | --- |
| USB1 playback | Stereo PCM16/PCM24 at 48/96 kHz |
| Wireless headset playback | Working with stock MCU2 and DSP |
| Native volume/mute | Windows, transmitter and headset controls integrated |
| Source bias | USB1 versus the selected MCU2 source |
| USB2/USB3 | Source selection implemented; shares one MCU2 path; audio validation incomplete |
| Line-out mixer | USB1 and line-in levels, mute and master-link controls implemented |
| USB2/USB3 mixer faders | Not implemented |
| Microphone capture | Endpoint streams silence; physical PCM path incomplete |
| Headset settings | Limiter, ANC, transparency, microphone controls and EQ commands |
| Bluetooth | Settings available; connection status depends on passive reports |
| OLED | Meters, volume, source bias, battery status and configurable timeout |
| Spare battery | Charging works; charge/full status, no live percentage estimate |
| Saving custom settings | Local UI/mixer state resets on power cycle; persistence not implemented |
| Endurance and lifecycle | Full qualification incomplete |

## Controls

Clockwise raises master volume and scrolls down menus. A short dial click switches
between master volume and source bias. Hold opens Settings. Click edits an item,
turn changes its proposed value, click confirms, and Back cancels or returns one level.
The headset dial and buttons also control volume, bias and menus.

The menu groups are Line out, Headset, Microphone, Bluetooth, Display and Inputs.
Shared Bluetooth startup/call and microphone LED settings require a complete cached
state before writing. UNAVAILABLE means that state isn't known yet. SENT confirms a
local DSP acknowledgement, not acoustic verification or persistence.

## Known limits

USB1 can mix with either USB2 or USB3. Three physical connectors don't provide three
simultaneous USB streams. The DSP meter labels aren't proven USB port mappings.

Bluetooth status is unknown after MCU1 resets until a report arrives. An absent icon
doesn't prove Bluetooth is off. Charging raises battery terminal voltage, so the
spare-battery display uses CHG, confirmed full or unknown instead of converting the
live voltage to a misleading percentage.

Build success proves compilation and offline checks. Listening tests, reconnects,
physical recovery and endurance still need validation on each hardware candidate.
