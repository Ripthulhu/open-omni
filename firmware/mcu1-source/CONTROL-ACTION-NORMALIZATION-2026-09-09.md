# Control-action normalization (local + remote → one navigation intent)

The next concrete step on the controls decoder: a pure layer that turns the
decoder's event stream into a device-independent **navigation intent**, so one
consumer handles the transmitter switches and the headset's DSP frames
identically. This is the "normalize local and remote controls before a single
serialized menu/volume state machine" step from `CONTROLS-ROUTING-2026-09-09.md`
(§ *Remote headset events*, line ~137). It owns no menu state and invents no
menu-tree semantics — both remain unresolved and are left to a later, evidence-
backed state machine.

## What it does
`include/control_action.h` / `src/control_action.c`:
- `omni_control_intent(event, out)` maps each `omni_controls` event kind to an
  `omni_nav_intent_t {action, source, direction}`. The mapping mirrors the stock
  dispatcher, which enqueues the **same** event IDs for the local switch table
  and the remote `0x27EA6` branch:
  - local short push / remote `04` → `OMNI_NAV_SELECT` (stock `0x2106`)
  - local push hold → `OMNI_NAV_MENU` (stock `0x2107`)
  - local Back short / remote `07` → `OMNI_NAV_BACK` (stock `0x2108`)
  - local Back hold → `OMNI_NAV_BACK_HOLD_UNKNOWN` (stock `0x2109`, unnamed)
  - remote `05`/`06` → `OMNI_NAV_DIAL` direction `0`/`1` (stock `0x2104`/`0x2105`)
  - remote `08`/`0A` → `OMNI_NAV_REMOTE_MENU_EXIT`/`ENTER` (stock `0x27F6A`)
  `source` records LOCAL vs REMOTE; everything else collapses to one stream.
- `omni_control_dial_target(menu_active)` is the one evidenced routing decision:
  outside a menu the dial is the **volume** control (the behavior the installed
  firmware already ships); inside a menu it moves the selection. It owns no menu
  state — the caller supplies `menu_active`.

## Deliberately NOT done (unresolved evidence)
- **No menu state machine.** `0x201F4`/`0x2109` are context-specific and the menu
  tree/depth is not RE'd. `OMNI_NAV_BACK` is surfaced but never auto-clears any
  menu flag here.
- **No local-rotary ↔ remote-dial direction equivalence.** `rotary.c` returns a
  signed step whose physical sign is "a board-level choice"; remote frames carry
  the stock `0/1` distinction. Folding them would assert an unproven mapping, so
  local rotary unification is deferred (needs the board-polarity observation and
  a headset-`91` wire capture the routing doc already flags).
- **No hardware.** Pure C11; no GPIO/UART/USB, no replies, no stock RAM reuse.

## Tests
`tests/test_control_action.c` (CMake `control_action_normalization`), strict
`-Wall -Wextra -Werror -Wconversion -UNDEBUG`: full kind×origin mapping table;
local/remote unification of Select and Back; `NULL`/out-of-range-kind rejection
leaving `OMNI_NAV_NONE`; the dial-routing gate; and an **integration** test that
drains a mixed local+remote FIFO from the real `omni_controls` decoder straight
into `omni_control_intent`, asserting one ordered normalized stream. Passes
alongside the unchanged `shared_controls` suite.

## Next
Still inactive (no UI/runtime caller). The following step — a serialized
menu/volume state machine consuming this intent — is gated on live DSP `91`
reception, the passive wire-length/direction capture, and stock reply semantics
per the routing doc; it should not be guessed from static evidence alone.
