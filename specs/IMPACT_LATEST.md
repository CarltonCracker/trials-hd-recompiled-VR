# UI dimming and achievement scaling

type: impact
context: Follow-up to BUG-001, user-requested menu readability improvements.

## Target

Shared ImGui dialog/drawer composition, achievements layout, and Trials UI theme.

## Dependents

- Settings, achievements, SDK settings, console, debug, guest XAM dialogs and
  message boxes: opt into a single shared background dim layer.
- Performance overlay and persistent achievement notifications: explicitly opt out.
- Settings and achievements: use one configured menu scale (1.6 × max(1, height/720)).
- Vulkan and WebGPU immediate drawers: existing solid-rectangle path, no renderer changes.

## Affected Stories

No release-plan stories exist. Scope is the two requested UI changes.

## Test Coverage

Existing `imgui_sharpness` CPU harness covers real drawer texture lifetime and DPI.
Extend it for dim visibility, passive overlays, multiple menus and shared scale.
Headless screenshots will verify real achievements text/icons, wrapping, scroll,
header controls and unchanged background when only the performance overlay is up.

## Risk: Medium

Shared dialog policy must not dim for the always-registered achievement notifier.
Default SDK dimming remains disabled until the consumer theme opts in. Disable
ImGui's separate modal scrim in the Trials theme to avoid double dimming.

## Plan

1. Add independent per-dialog dim policy and one configurable background scrim.
2. Centralize menu scale in per-drawer style; keep current Settings sizing.
3. Scale achievements font, width, icons, spacing and controls; cap height to 92%
   of the viewport and retain list scrolling. Center like Settings.
4. Extend CPU tests, rebuild/stage, inspect 1440p and 720p screenshots, document.

## Result

Completed. All 7 ctests pass; screenshot dim/restore checks pass at both
resolutions. See `specs/verifications/UI-readability.md`.
