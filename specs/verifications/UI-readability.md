# UI readability verification — 2026-10-18

type: verification
context: User requested dimming behind ImGui menus and Settings-scale achievements.

## Changes

- One configurable background scrim behind all UI windows when any interactive
  dialog is open. Performance and achievement notifications opt out.
- Trials opts into 60%-black dimming; native ImGui modal scrim is transparent
  to avoid stacking dim layers.
- Settings and achievements share display-relative scale. Achievements scales
  text, icons, padding and spacing, centers in the viewport and scrolls within
  92% of its height. The close button uses measured text width.

## Evidence

- RED: extended CPU test failed on missing menu background dim rectangle.
- GREEN: all 7 ctests pass. The UI test checks single/multiple menus, idle/visible
  notifications, performance overlay, last-menu removal, actual achievement
  window and glyph dimensions at 720p/1440p and 100/125/200% display density.
  Original font-size, atlas lifetime, upload retry and DPI regressions still pass.
- Game rebuilt; runtime and renderer plugins staged.
- Inspected actual 2560x1440 and 1280x720 screenshots, including performance-only,
  Settings, Graphics, achievements, two menus, and all menus closed.
- Screenshot pixel check: the same static background patch is 0.399 times its
  undimmed brightness for each menu combination, and 1.000 after closing menus,
  at both resolutions. Confirms no double-dimming or stuck backdrop.
- 1440p achievements: 32px text, 1664px width; 720p: 16px text, 832px width.
- Captures: gitignored `out/ui-readability/{1440p,720p}/ui_*.png`.
- SDK reverse-apply, SDK/project whitespace (excluding patch context), Bash syntax
  and Python compilation checks pass.

## Self-review

Correctness, resource ownership, performance and security checked. Dimming is a
single solid rectangle; stable frames do not upload new font textures. No new
runtime dependency, user-data surface, game assets or generated code tracked.
SDK edits are exported through the project patch. No independent agent review.
Browser/macOS runtime checks were not run. Implementation is maintained on `dev`.
