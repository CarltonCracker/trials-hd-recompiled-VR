# PC SETTINGS verification — 2026-10-19

type: verification
context: Replace Download Content with native Settings and capitalize FPS Counter.

## Implementation

- Keep the original button at `menu + 68`; Exit remains at `menu + 72`.
- Replace the caption after `sub_8265BC90` refreshes it. Send source-text message
  2016 through `sub_8265BB90`; `sub_821A0498` copies it with `sub_82188B18`.
  Generic XuiText/control-text setters did not change the visible caption.
- Use a copied PPC context and an aligned temporary guest frame with big-endian
  UTF-16. Only the target control changes; shared localization is not patched.
- Queue an idempotent Settings open on the UI thread. Skip the Xbox LIVE action
  for this button, retaining the other menu actions.
- Keep saved `show_performance_overlay` and `bind_performance_overlay` IDs.

## Results

- Native build and all 8 ctests pass, including the guest text ABI test.
- Inspected real 2560x1440 Xvfb captures in `out/pc-settings/`:
  - `pc_main_menu` and `pc_selected`: PC SETTINGS replaces Download Content;
    seven D-pad presses select it.
  - `pc_settings_controller`: held A opens Settings without toggling it closed.
  - `pc_settings_graphics`: the Graphics tab shows FPS Counter.
  - `pc_settings_keyboard`: Return reopens Settings after F1 closes it.
- Both activations appear in the logs. No caption-setter failure or online popup.
- The capture checker passes: open background ratios 0.393–0.397, closed ratios
  0.982–0.985, relative to the original game background.
- Synthetic checks accept activations split across rotated logs and reject a
  missing activation or missing menu dimming. Preserve all `trials_hd*.log` files.
- Rechecked achievements Close/X/Escape and controller screenshot checkers: pass.
- Python compilation, Bash syntax, SDK reverse-apply and source whitespace pass.
  The outer whitespace check excludes the patch's unchanged context lines.

Physical pads are excluded only in the headless game process so the virtual pad
gets player one. Logs flush every second before termination. The run used silent
audio and software Vulkan; it did not open a window on the user's desktop.

## Self-review

- ✓ Correctness/safety: guest ABI, copied string lifetime, UI-thread ownership,
  deferred achievements destruction and held-button behavior checked.
- ✓ Security/supply chain: no dependencies, credentials, external API calls or
  new untrusted-input paths. Caption storage is compile-time bounded.
- ✓ Performance/scope: caption work is confined to the menu refresh; no new heap
  scan. Numeric FPS rendering removes the graph/history work.
- ✓ Clarity/style/provenance: follows AGENTS.md and surrounding C++ conventions;
  SDK changes remain in the exported patch, with no submodule commit changes.
- ✓ Tests: CPU ABI/UI regressions plus real headless navigation/close checks.

No CONVENTIONS.md exists; unrelated language/package rules do not apply. Existing
large app/SDK files retain their structure rather than adding an unrelated
refactor. Independent agent review and browser/macOS runtime tests were not run.
All screenshots, dumps and logs stay gitignored. UI work remains on `dev` only.
