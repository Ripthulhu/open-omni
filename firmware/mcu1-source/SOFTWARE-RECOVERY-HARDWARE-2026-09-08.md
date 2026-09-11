# Software recovery hardware result — 2026-09-08



Installed build: `omni-a-003daae989892621`.

Image SHA256: `fa0b02ed258921b8279b87e5b18f691ea1986f9431b6a29a48e41c9ca120c088`.

Code: 14104 bytes, SHA256 `2767cd28cda7b4f70a50b5ba616ccb094e8117f73c10fa2c498b99f2b1674be9`.



1. User entered physical recovery; confirmed unique MCU1 1038:2291 FFC0.

2. Candidate/source/restores preflight passed. Flash accepted per-block ACKs and

   CRC checks; all 285612 staged bytes matched before commit. Runtime identity

   and all 14104 code bytes matched; startup acknowledgement status 1, driver 0.

3. Source software recovery preparation verified, followed by one reset request.

   MCU1 enumerated as 1038:2291 without user action. All installed code matched

   through bootloader readback; complete metadata page captured.

4. Sent loader return command 01 01 00 01 once, without restaging. Write returned

   -1 during disconnect. Did not retry. Independent runtime verification then

   matched identity and full code, with acknowledgement status 1 / driver 0.



Left this build running. One software recovery/return cycle is hardware-verified.

Normal cold boot of this new candidate, its physical recovery/restore trial and

repeated lifecycle trials remain pending. Audio/dial/display remain unimplemented.



Evidence in `releases/omni-a-003daae989892621/` (local, ignored by Git):

- `hardware-20260908T164617Z.json`

- `software-recovery-20260908T164633.881618Z.json`

- `software-recovery-return-20260908T164652.933003Z.json`

- `hardware-20260908T164654Z.json`



Do not interpret the interrupted loader return write alone as success. The

separate runtime identity, readback and acknowledgement checks establish return.


## Normal cold boot — 16:50 UTC

User confirmed all transmitter power was disconnected for 30 seconds and USB1
reconnected normally without holding the dial. After that confirmation, runtime
verification found `omni-a-003daae989892621` and matched all 14104 code bytes.
Startup acknowledgement was status 2 (already valid), driver status 0: no new
acknowledgement write was needed. Evidence: `hardware-20260908T165020Z.json`.
This is one user-performed cold-start trial; this check did not independently
monitor USB absence or measure the unpowered interval. Device left running.
