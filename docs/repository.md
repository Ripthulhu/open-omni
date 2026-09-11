# Repository layout

| Path | Contents |
| --- | --- |
| `firmware/mcu1-source/src`, `include` | LPC5528 application and drivers |
| `firmware/mcu1-source/tests`, `cmake` | Host tests and firmware build configuration |
| `firmware/mcu1-source/tools` | Build, dependency fetch and diagnostic tools |
| `tools/flash` | Cross-platform firmware installer |
| `tools/package_firmware.py` | Portable firmware bundle packaging |
| `firmware/rebuild-re` | Protocol analysis, capture tools and dated technical findings |
| `firmware/rebuild-re/evidence` | Selected protocol captures and verification records |
| `qr-patch` | Historical stock-firmware patch experiments |
| `tools/legacy/bridge` | Old Windows volume bridge for stock firmware |
| `docs` | Hardware support and research index |

Start with the [README](../README.md) for installation and the
[research index](research.md) for hardware and protocol details.

Stock firmware dumps, per-unit factory backups, SDK checkouts, manuals and generated
releases stay local. Some research tests require those inputs. Current public builds
fetch pinned SDK sources and don't require a stock dump.

Keep firmware paths stable: build, analysis and restore tools resolve inputs relative
to them. Git preserves line endings because the build identity hashes exact source
bytes. Historical reports describe their dated experiments, not the current device state.
