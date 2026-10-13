// trials_hd - in-game achievements menu hook.
//
// Intercepts the game's menu call (sub_82124618 and __imp__XamShowAchievementsUI)
// and displays the ReXGlue ImGui achievements overlay dialog.

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "trials_hd_app.h"

extern "C" REX_FUNC(sub_82124618) {
  if (auto* app = TrialsHdApp::Instance()) {
    app->ShowAchievements();
  }
}

extern "C" REX_FUNC(__imp__XamShowAchievementsUI) {
  ctx.r3.u64 = 0;  // X_ERROR_SUCCESS
  if (auto* app = TrialsHdApp::Instance()) {
    app->ShowAchievements();
  }
}
