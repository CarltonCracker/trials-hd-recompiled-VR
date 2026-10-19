# Main-menu PC SETTINGS entry

type: impact
context: Replace Download Content with PC SETTINGS and capitalize FPS Counter.

## Scope

- Extend main-menu hooks in `exit_button.cpp` with the caption-refresh hook;
  do not add duplicate hooks or edit generated code.
- Download Content's button handle is stored at menu + 68, next to Exit at +72.
  The recompiled initialization and click handler establish these fields; the
  +68 branch leads to the Xbox LIVE/download flow.
- After caption refresh (`sub_8265BC90`), set only this control's source text
  through `sub_8265BB90` (message 2016 → `sub_821A0498` → `sub_82188B18`), using
  temporary big-endian UTF-16 guest stack storage. The message handler copies
  text. Runtime checks showed that generic XuiText/control-text setters do not
  update this button's visible caption.
  Avoid a new heap scan or editing shared localization strings/assets.
- Selecting it queues an idempotent host Settings open on the UI thread and
  bypasses the old online/download action. Preserve all other menu actions.
- Display `FPS Counter` in Graphics settings; keep internal cvar/binding names.

## Risk: Medium

Guest ABI/text lifetime and button identity must be correct. Preserve the original
initialization return context, and do not close Settings if duplicate menu events
arrive while A is held.

## Verification

- CPU test big-endian label encoding, guest stack alignment/backchain, context
  preservation, setter errors and invalid-control/stack handling.
- Build and run all ctests.
- 1440p screenshot: PC SETTINGS label replaces Download Content.
- Controller navigation selects it and opens Settings without an online popup;
  close and reopen it, then verify the Graphics `FPS Counter` label.
- Keyboard entry should open the same menu. No game files/screenshots in git.
- Isolate virtual-pad tests from physical controllers using the process-local
  SDL gamepad filter; otherwise a connected controller can take player one.

Verification completed on 2026-10-19: all 8 ctests pass, and the real 1440p
controller/keyboard captures pass. See `specs/verifications/PC-settings.md`.

## Separate metadata publication

The exact supplied `.recomp.json` was published alone on both remote branches:
`main` 104717c, `dev` e3c72ce. The spec requires the default branch. Those commits contain no UI changes; recomp.fyi refresh timing is external to
this repository.
