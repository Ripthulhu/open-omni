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
| Microphone capture | USB1 mono PCM16 at 48 kHz; mic-only, reopen and 96k playback coexistence tested |
| Headset settings | Limiter, ANC, transparency, microphone controls and EQ commands |
| Custom EQ menus | Ten bands per channel; wireless frequency/gain/Q/filter, microphone and Bluetooth gain |
| Bluetooth | Settings available; connection status depends on passive reports |
| OLED | 30 Hz refresh, smoothed meters, volume, source bias, battery status and configurable timeout |
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

## Custom EQ

Open Wireless EQ, Mic EQ or BT EQ in Settings. Preset selects a built-in or
the last custom curve applied during this boot. Edit Curve opens a draft;
click a band to edit it, then select Apply Curve after band 10. Back cancels
the current field edit and leaves the remaining draft available. New Flat
replaces only the draft until Apply Curve is selected.

Wireless bands expose frequency, gain, Q and filter type. Microphone and
Bluetooth expose ten band gains. Clockwise increases a selected value.
Gain detents are 0.5 dB within -12 to +12 dB.

Drafts and the last accepted custom curves are held in MCU1 RAM. Switching
presets preserves these copies; restarting MCU1 does not. If a startup
snapshot reports Custom without its coefficients, Edit Curve does not invent
them: use New Flat to explicitly create a replacement. No custom-coefficient
readback from the headset or nonvolatile save is implemented.

The EQ editor uses a wireframe band graph with hollow markers and a selected
band cursor. Wireless positions use a logarithmic 20 Hz..20 kHz axis; graphic
EQ uses band numbers. Unconfirmed gain/frequency edits preview on the graph
and Back restores the draft point. Wireless EQ now shows a calculated, summed filter response with Q-dependent
width, resonance and slopes. The model uses RBJ biquads at an assumed 48 kHz
EQ processing rate; the DSP coefficients/rate have not been independently
verified. Mic/Bluetooth retain band-gain plots because their filter parameters
are not established. Q and filter values stay visible below the graph.
