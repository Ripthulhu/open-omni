# Repository layout

| Path | Contents |
| --- | --- |
| `firmware/mcu1-source/src`, `include` | LPC5528 application and drivers |
| `firmware/mcu1-source/tests`, `cmake` | Host tests and firmware build configuration |
| `firmware/mcu1-source/tools` | Build, dependency fetch and diagnostic tools |
| `tools/flash` | Cross-platform firmware installer |
| `tools/package_firmware.py` | Portable firmware bundle packaging |
| `firmware/rebuild-re` | Build and host-test helper scripts |
| `tools/legacy/bridge` | Old Windows volume bridge for stock firmware |
| `docs` | Hardware support and device documentation |

Start with the [README](../README.md) for installation and
[hardware support](status.md) for the device details.

Stock firmware dumps, per-unit factory backups, SDK checkouts, manuals and generated
releases stay local. Some research tests require those inputs. Current public builds
fetch pinned SDK sources and don't require a stock dump.

Keep firmware paths stable: build, analysis and restore tools resolve inputs relative
to them. Git preserves line endings because the build identity hashes exact source
bytes. Historical reports describe their dated experiments, not the current device state.
