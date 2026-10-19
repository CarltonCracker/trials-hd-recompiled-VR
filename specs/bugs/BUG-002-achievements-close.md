---
bug_id: BUG-002
status: fixed
severity: high
scope: ui
title: Achievements Close button crashes the game
---

# Achievements Close button crashes the game

Security impact: LOW; UI-triggered lifetime error, no external exploit path identified.

## Reproduce

At 2560x1440 open achievements with F7 and click Close (Esc). Use
`tests/headless/achievements_close.txt` to reproduce through real mouse input.
Also test the title-bar X, keyboard Escape and reopening after each close.

## Isolate

The achievements dialog invokes its close callback from its drawing method.
The app callback immediately resets the unique pointer that owns that dialog.
The callback storage is destroyed while being invoked, and the base dialog Draw
method accesses the deleted object after the derived drawing method returns.

## Hypothesize

Synchronous destruction during drawing causes use-after-free. Queue the owner
reset on the UI context instead, after the dialog draw stack has returned.
Do not call the base self-deleting Close API on an externally owned dialog:
that would leave a dangling unique pointer and permit a second deletion.

## Verify

The real Close-button click freezes the software-Vulkan run: all later close,
reopen and Escape captures remain stuck on the same dialog. GDB shows the UI
thread blocked during Vulkan presentation. The owner reset runs before the
current draw list can consume the dialog's icon texture pointers, in addition
to deleting the base Draw receiver. This verifies the unsafe close lifetime.
Baseline captures and backtrace are local in `out/achievements-close/before/`.
After deferring the reset, all Close-button, title-bar X, Escape and reopening
checks pass. Closed frames restore the game background to 2.505–2.506 times its
dimmed brightness; reopened frames return to 1.000 times the dimmed baseline.
All 7 ctests pass. Captures are local in `out/achievements-close/after/`.

The SDK's default achievements owner had the same synchronous reset pattern;
its callback is deferred too, and `SetOnClose` now documents that owners must
not destroy the dialog during Draw. Both ownership sites were checked. Final 2026-10-19 captures with controller
scrolling enabled also pass Close/X/Escape and reopening checks:
`out/achievements-close/final/`. See `specs/verifications/FPS-controller-close.md`.

## Fix plan

1. Reproduce by clicking the real Close button under the headless harness.
2. Defer the app-owned dialog reset until after drawing, retaining the existing
   input-release gate and same UI-thread ownership.
3. Re-run Close button, reopen, X button, reopen and Escape with screenshots.
4. Run all ctests and confirm no game content or screenshots are tracked.
