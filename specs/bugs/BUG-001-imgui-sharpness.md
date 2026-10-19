---
bug_id: BUG-001
status: fixed
severity: medium
scope: ui
title: Blurry ImGui text at 1440p
---

# Blurry ImGui text at 1440p

Security impact: NONE; no security exploit path identified.

## Reproduce

Open Settings (F1) at 2560x1440. Text is soft, including the larger FPS overlay.
Capture the host framebuffer, not the guest frame (which excludes ImGui).
Run the `ui_sharpness` headless script at 2560x1440. On Xvfb use software Vulkan
(device 1 on the test machine); the hardware driver cannot present to Xvfb.

## Isolate

Settings requests a 32px font at 1440p, but the UI renderer uploads only the
initial 10px font atlas. Before this fix, no test covered scaled fonts or atlas updates.
This is a missing renderer capability, not a game render-resolution problem.

## Hypothesize

1. Fixed font atlas is stretched: test that requesting a 32px font actually
   rasterizes 32px glyphs, then compare native-resolution screenshots.
2. DPI scaling stretches glyphs: test rasterizer density at 125%, 150%, 200%.
3. Game rendering is blurred: falsified if only host UI changes after updating
   atlas handling without changing game resolution or postprocessing.

## Verify

The regression test fails on the unmodified drawer with:
`Font must be rasterized at requested size, not stretch a 10px atlas`.
Software Vulkan screenshots reproduce soft text. This confirms hypothesis 1;
DPI density is a separate missing input to the same rasterization path.

## TDD Fix Plan

1. RED: exercise the actual UI drawer through an in-memory window and texture
   target. A 32px font must have 32px baked glyphs, without repeated uploads
   for unchanged frames. GREEN: implement dynamic ImGui texture requests.
2. Check physical-pixel density after DPI changes, plus detach/reattach cleanup.
3. Compare settings, overlay and achievements screenshots at 1440p. Retain the
   existing UI size and theme. Run all ctests and stage updated runtime libraries.

## Acceptance Criteria

- [x] Text is rasterized at requested size, not stretched from 10px.
- [x] DPI changes produce matching rasterizer density.
- [x] Atlas updates and teardown are safe; stable frames do not reupload.
- [x] Before/after 1440p screenshots inspected.
- [x] Existing tests pass.

## Resolution

Fixed 2026-10-18. Enabled dynamic atlas texture requests and framebuffer density
in the SDK drawer. Full texture replacement uses the existing immediate-renderer
API only when the atlas changes, retaining backend deferred resource destruction.
Upload failures skip the frame and retry, rather than draw invalid atlas data.
No font, UI size, game rendering, or saved setting changes are required.

Evidence:
- RED: original drawer fails the requested glyph-size check.
- GREEN: all 7 ctests pass, including 16–120px glyph sizes, 100/125/150/200% DPI,
  stable frames, failed-upload recovery, detach/reattach and destruction.
- Game rebuilt and runtime/plugins staged. Controls, Graphics, FPS and
  achievements captured and inspected at 2560x1440 (Xvfb/software Vulkan).
- Native-resolution comparisons: `out/ui-sharpness/settings-comparison.png` and
  `out/ui-sharpness/fps-comparison.png`. Full captures under `before/` and `after/`.
- SDK reverse-apply check, SDK whitespace check, project whitespace check
  (excluding patch context lines), shell syntax and Python compilation pass.

Self-review: correctness, resource lifetime, steady-state upload cost, scope,
and security checked. No new dependencies or security input surfaces. Defect-class
sweep found three scaled-font calls (settings and two overlay sizes), all fixed
through the shared drawer; no remaining fixed-atlas upload in the UI source.
No independent agent review or browser/macOS runtime check performed.

Implementation and verification evidence are maintained on `dev`.
