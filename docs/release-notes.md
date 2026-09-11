Download **open-omni.zip**, extract it, then open `flash.cmd` on Windows or run
`sh flash.sh` on Linux/macOS. Python 3.10 or newer is required.

This prerelease includes USB1 playback at 16/24-bit, 48/96 kHz, wireless playback,
native volume/mute, source selection and bias, line-out controls, expanded settings
menus, OLED meters and battery charging status. MCU2, DSP and bootloader remain stock.

Known gaps: USB1 microphone capture streams silence, independent USB2/USB3 mixer
faders are missing, and local settings aren't saved across power cycles. Secondary
USB audio and full lifecycle/endurance qualification remain incomplete.

The firmware is built twice with identical application bytes. Host tests, UI
sanitizer checks and simulated flasher tests on Windows, Linux and macOS pass before
publication. These checks don't constitute hardware qualification of this snapshot.

The flasher saves the installed MCU1 application before writing and verifies the
running build afterward. No stock or per-unit factory firmware is bundled.
