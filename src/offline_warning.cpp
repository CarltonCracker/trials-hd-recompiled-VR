// trials_hd - no "Offline warning" before each track.
//
// When a profile is signed in without Xbox LIVE, the game's sign-in poll
// (sub_8261ECE0) sets that player's bit in a "not warned yet" mask, and the
// pre-track setup run on picking a bike (sub_8263B570) then shows the
// PRETRACK_OFFLINE_WARNING message box ("Any scores achieved while offline
// cannot be saved to Xbox LIVE leaderboards...") and clears the bit. Xbox LIVE
// never exists here, so clear the mask first: the game takes its own
// "already warned" path. The message box code is reachable only through that
// bit, and the mask is read nowhere else.

#include <cstdint>

#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_8261ECE0);
REX_EXTERN(__imp__sub_8263B570);

namespace {
// Global mask at 0x82768498 (-31592 from 0x82770000) of players pending the
// PRETRACK_OFFLINE_WARNING popup.
constexpr uint32_t kOfflineWarningPending = 0x82768498;
}  // namespace

// Sign-in status poll: clears the warning mask so offline warnings are never flagged.
extern "C" REX_FUNC(sub_8261ECE0) {
  __imp__sub_8261ECE0(ctx, base);
  rex::memory::store_and_swap<uint32_t>(base + kOfflineWarningPending, 0);
}

// Pre-track entry: ensure mask is cleared so the popup is bypassed.
extern "C" REX_FUNC(sub_8263B570) {
  rex::memory::store_and_swap<uint32_t>(base + kOfflineWarningPending, 0);
  __imp__sub_8263B570(ctx, base);
}
