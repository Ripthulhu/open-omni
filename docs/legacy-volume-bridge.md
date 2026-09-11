# Legacy Windows volume bridge

The [bridge scripts](../tools/legacy/bridge/) controlled the stock transmitter over
vendor HID. They were used before native volume support existed in the custom firmware.

Open Omni doesn't use this bridge. Don't run it alongside the custom firmware.
The scripts are kept as historical protocol examples and require Windows, Python,
`hidapi`, `pycaw` and `comtypes`.
