# FPS counter and achievements verification — 2026-10-19

type: verification
context: Numeric FPS counter, achievements Close crash, and controller accessibility.

## Implementation

- FPS overlay renders only a number at the top-left, without units, stats, graph
  or panel. Settings calls it `FPS Counter` (capitalized in the subsequent
  PC SETTINGS task). Existing cvar/binding IDs remain
  unchanged, including F2 and saved `show_performance_overlay` preferences.
- Both app and SDK default achievements owners defer destruction out of Draw.
  The dialog and icon textures remain alive through the current presentation.
- Achievements accepts scroll input from the app: D-pad/either stick scroll,
  LB/RB page, A closes, and existing B/Back closing remains. Hints are visible.
  The first controller sample consumes held A to avoid closing on entry.
  Keyboard arrows/Page Up/Down work too. Scrolling, paging and A are focus-gated.

## Evidence

- All 7 ctests pass after rebuilding/staging the game and runtime/plugins.
- CPU UI regression captures the counter's text through an in-memory clipboard
  and checks numeric-only contents, top-left position and compact dimensions.
- CPU UI regression verifies achievements page-down/page-up and close actions.
- Actual 1440p virtual-controller captures pass:
  - RB changes list content; LB restores the top.
  - D-pad and left stick move through the list, reaching the last achievement.
  - Right stick returns to the top; A and B both close safely.
- Actual Close-button, title-bar X, reopen and Escape checks pass on the final
  build. Each close restores background brightness to 2.505–2.506 times the
  dimmed baseline; reopening returns to the dimmed baseline.
- Inspected the number-only top-left counter and the Graphics settings toggle.
  The initial lowercase label was subsequently capitalized to `FPS Counter`.
- Before-fix close capture checker fails on the frozen menu. Synthetic fixture
  checks confirm both screenshot checkers reject missing transitions.
- SDK reverse-apply, SDK/project whitespace (excluding patch context lines),
  Python compilation and Bash syntax checks pass.

## Local artifacts

All images/logs remain gitignored and are not game content to publish:

- `out/fps-counter/ui_overlay_only.png`, `ui_graphics.png`
- `out/achievements-controller/controller_*.png`
- `out/achievements-close/before/` (freeze/backtrace), `after/`, `final/`

The runs used Xvfb/software Vulkan and silent audio, not the user's desktop.
Browser/macOS runtime checks and an independent agent review were not run.
Implementation and evidence are maintained on `dev`; see git history for publication status.
