# Current manual review and prefetch correction

> Later hardware trial: this candidate has now been flashed and verified,
> including one normal cold start and physical loader entry/return. See
> `FLASH-FIXES-2026-09-08.md`. Unflashed statements below describe the review-time state.

The user supplied `C:/Users/dylan/Downloads/UM11126.pdf`. Its cover identifies
UM11126 revision 2.8, dated 27 May 2025, covering LPC55S6x/LPC55S2x/LPC552x.
NXP's catalog date of 23 June 2025 is the publication date. The PDF has 1,238
physical pages and 18,860,329 bytes. Its SHA-256 is
`60f33e5d2f360d6a1a826a7f1e10df5bf32f28b8c22ec838b49ae8f19b335355`.

A byte-identical copy is archived at
`../rebuild-re/official-docs/UM11126-rev2.8.pdf`; the original was unchanged.
The documentation index now distinguishes this verified current edition from
the older Farnell copy and historical download failures. Only relevant startup
and flash sections were reviewed, not the whole manual.

## Implemented correction

UM11126 Table 113 note [2] (PDF page 81) and section 5.6.1.1 (page 116) require
CPU0 flash prefetch to be disabled before flash-controller commands. The previous
unflashed acknowledgement candidate did not establish this precondition.

`src/boot_ack_lpc5528.c` now saves FMCCR.PREFEN, clears it with barriers before
the first ROM call, and restores it on every completion/failure path before
restoring interrupts. Only PREFEN is restored; any flash timing changes made by
ROM initialization remain intact. This matches the prefetch handling pattern
already used by the pinned NXP clock driver's flash timing routine.

Candidate `releases/omni-a-5c74e71366c8bb50` supersedes the unflashed
`omni-a-10c7e4e03dfb3749` candidate. The new application is 285612 bytes with
13360 bytes of code/data. Image SHA-256:
`b94d7978831596b131eab99aa3446af313d5e2125e6b1e957f47ccd837e440ed`.

No USB, SWD, erase, program, reset, or commit command was sent during this
documentation review. The candidate remains **unflashed and hardware-unvalidated**.

## Remaining prerequisites

The manual confirms USB0CLKDIV.HALT resets to one, supporting the corrected
clock-before-PORTMODE ordering. It also documents FRO192M_TRIM_SRC=1 as a
prerequisite for USBCLKADJ (section 11.5.4, PDF page 257). Our selected SDK startup
does not explicitly establish that setting, and the analog chapter assigns its
management to the SDK power library. No suitable initializer was found in the
currently selected path. Record the live trim-source and calibration state before
accepting the USB startup as independent of the stock ROM/loader configuration.
No speculative analog-register write was added.

ROM clock-gate state is likewise inherited from the preserved bootloader, which
uses ROM flash APIs before launching this application; a fresh register capture
has not established that contract. Current source does not disable those gates.

These are explicit remaining startup checks, not evidence that either was the
cause of the earlier USB disappearance. The manual does not describe SteelSeries'
metadata contract or board/inter-chip protocols. Those remain supported by the
captured firmware and subsequent hardware tests.

The exact current binary's offline evidence is packaged alongside its manifest.
All 124 acknowledgement cases passed (62 existing cases, each with prefetch
initially enabled and disabled). Every modeled ROM entry checks prefetch is
disabled, and every return verifies restoration while retaining ROM-updated
flash timing. All four compiled USB startup cases passed, with the cold model
updated to the manual's mux=7 and FRO192M_CTRL reset values. A separate rebuild
produced the identical application hash. Local file verification passed for
CRC/hash, vectors, memory bounds, all 19 source files, restore images and evidence.
See `startup-regression.json`, `boot-ack-regression.json`, and
`file-verification.json` in the new release directory. Physical flash timing,
power interruption, USB enumeration and recovery still require hardware tests.

Focused source references: `../rebuild-re/official-docs/UM11126-rev2.8-flash-review.md`
and `UM11126-rev2.8-usb-review.md` in that directory. Build logs are
`../rebuild-re/evidence/manual-2.8-prefetch-build.log` and
`manual-2.8-prefetch-repro.log` in that directory.
