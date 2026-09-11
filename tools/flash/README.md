# Open Omni flasher

Install Python 3.10 or newer. Download the `open-omni-firmware` artifact from a
successful [Firmware build](https://github.com/Ripthulhu/open-omni/actions/workflows/firmware.yml)
and extract both ZIP layers. The inner `open-omni.zip` contains this flasher and its
matching firmware. These are development builds; a green build is not a hardware test.

Connect only the transmitter's USB1 to this computer. Close GG and other Omni
diagnostic tools. Keep the headset disconnected from USB during flashing.

- Windows: double-click `flash.cmd`.
- macOS or Linux: open a terminal in the extracted folder and run `sh flash.sh`.
- Any OS: `python launch.py` (`python3` on systems where that is its name).

The launcher installs the pinned HID package into `~/.open-omni/flasher-venv`.
It needs internet on first use. It never installs drivers or changes audio endpoints.
Windows uses its standard HID driver; do not replace it with Zadig/WinUSB.

On Linux, install `libhidapi-hidraw0` and `libusb-1.0-0`. Install the included
`70-open-omni.rules` in `/etc/udev/rules.d/`, run
`sudo udevadm control --reload-rules`, then reconnect USB1. Flash as your normal
desktop user, not root. A headless session may need an administrator's access rule.

An existing Open Omni application enters recovery automatically. From stock, follow
the prompt: remove all power, hold the dial, reconnect USB1, keep holding five
seconds, then release. The tool waits up to 90 seconds for MCU1 `1038:2291`.
A short hold showing FIRMWARE UPDATE AVAILABLE while audio works is a different mode.

Before writing, the tool validates the new image and saves the installed application
under `~/OpenOmni-backups/<timestamp>/`, validating its CRC and backup file. It stops
if the backup is invalid. This saves the application, not the bootloader or factory
settings. Those regions are not flashed. Keep these private backups for recovery;
the public package contains no stock firmware.

The fast transfer checks every block ACK and the loader's whole-image CRC pair, then
verifies the running build identity, full code readback and boot acknowledgement.
A timeout or negative reply stops the transfer. A finalisation command alone is
never reported as success. A detailed log is saved beside the backup.

If runtime enumeration fails, power-cycle USB1 and run `python launch.py --verify`.
Do not repeat flashing just because Windows has not finished reconnecting.
Use `python launch.py --check` to validate the package without touching USB.

From a source checkout, pass a built release directory:
`python tools/flash/launch.py firmware/mcu1-source/releases/BUILD_ID`.
Hardware flashing has been exercised on Windows through the shared protocol code;
the new launcher and macOS/Linux physical USB access need their own hardware validation.
