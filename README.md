# Open Omni

Custom firmware for the SteelSeries Arctis Nova Pro Omni transmitter.
Native USB audio, hardware volume control, a mixer and an OLED interface without GG.

The firmware replaces MCU1 (NXP LPC5528). MCU2, the wireless DSP and the bootloader
stay stock.

## Support

- USB1 stereo playback: 16/24-bit, 48/96 kHz.
- Headset microphone capture over USB1: mono 16-bit, 48 kHz, including mic-only use.
- Native Windows volume and mute, synchronised with the transmitter and headset dials.
- Wireless headset playback, USB2/USB3 source selection and USB1/secondary-source bias.
- USB1 and line-in faders for analogue line-out, with independent or master-linked volume.
- Headset, microphone, Bluetooth and display settings through the device menus.
- OLED meters, headset battery status and spare-battery charging status.

Independent USB2/USB3 mixer faders and saving custom settings across power cycles
are not implemented.
Secondary-port audio and long-term reliability still need full validation. The spare battery shows charging
or full status, not an estimated percentage while charging.

[Hardware support and limitations](docs/status.md)

## Install

Download **open-omni.zip** from [Releases](https://github.com/Ripthulhu/open-omni/releases)
and extract it. Prereleases are development snapshots.

For newer untagged builds, download the `open-omni-firmware` artifact from
[Actions](https://github.com/Ripthulhu/open-omni/actions/workflows/firmware.yml).

Install Python 3.10 or newer. Connect the transmitter through USB1, then:

- **Windows:** open `flash.cmd`.
- **Linux/macOS:** run `sh flash.sh` from the extracted folder.

The flasher saves the installed application, checks every transfer and verifies the
running firmware. These are development builds. The cross-platform launcher has
simulated transport tests; physical flashing has been tested through its shared
protocol code on Windows.

[Flashing and recovery](tools/flash/README.md)

## Build

Requires GNU Arm Embedded 13.2.1, CMake, Ninja, Git and Python 3.
Dependencies are downloaded from pinned upstream revisions.

```sh
python3 firmware/mcu1-source/tools/fetch_dependencies.py vendor
python3 firmware/mcu1-source/tools/build.py --vendor "$PWD/vendor" --build "$PWD/build-arm" --public
```

GitHub builds the firmware twice to check reproducibility and runs the host tests,
UI sanitizer checks and flasher tests on Windows, Linux and macOS.

## Source and research

- [Firmware](firmware/mcu1-source/): application source, drivers and tests.
- [Tools](tools/): portable flasher and packaging.
- [Research](docs/research.md): board, protocols, audio and charging findings.
- [Repository layout](docs/repository.md).
