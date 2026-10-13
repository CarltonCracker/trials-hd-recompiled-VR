// trials_hd - main menu changes for the PC port.
//
// Removes the Leaderboards, Achievements and Download Content entries: they
// front Xbox LIVE / marketplace services that do not exist on PC. The menu
// scene comes from the player's game data and is left untouched; after the
// game's own menu setup runs, the three buttons are hidden and the buttons
// below them move up into the freed slots. XUI's navigation already skips
// hidden buttons, so the D-pad order follows without further changes.
//
// Addresses are for default.xex v0.0.1.2 (title 5841095A).

#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

// Main menu scene setup: resolves each button of MainMenuButtonGroup by ID
// (XUI get-child-by-ID) and stores the handles in the scene object (r3).
REX_EXTERN(__imp__sub_8265BA38);
// Game helper: sends XUI message 0x12 (set show) with a BOOL to an element.
REX_EXTERN(sub_8265D5A0);
// XuiElementGetPosition / XuiElementSetPosition (handle, D3DXVECTOR3*).
REX_EXTERN(sub_82180F38);
REX_EXTERN(sub_82180F88);

namespace {

// Handles of the lower button group in the scene object, top to bottom.
constexpr uint32_t kLeaderboardButton = 0x34;
constexpr uint32_t kGarageButton = 0x38;
constexpr uint32_t kAchievementsButton = 0x3C;
constexpr uint32_t kHelpButton = 0x40;
constexpr uint32_t kUnlockDownloadButton = 0x44;
constexpr uint32_t kReturnButton = 0x48;
constexpr uint32_t kLowerGroup[] = {kLeaderboardButton, kGarageButton,         kAchievementsButton,
                                    kHelpButton,        kUnlockDownloadButton, kReturnButton};

bool IsRemoved(uint32_t slot) {
  return slot == kLeaderboardButton || slot == kAchievementsButton ||
         slot == kUnlockDownloadButton;
}

struct Vec3 {
  float v[3];
};

class GuestCalls {
 public:
  // Reserves a small guest stack frame for the vector argument; the XUI calls
  // build their own frames below it.
  GuestCalls(PPCContext& ctx, uint8_t* base) : ctx_(ctx), base_(base), saved_r1_(ctx.r1.u64) {
    ctx_.r1.u64 -= kFrameSize;
    scratch_ = ctx_.r1.u32 + 0x50;
  }
  ~GuestCalls() { ctx_.r1.u64 = saved_r1_; }

  uint32_t Load32(uint32_t address) const { return std::byteswap(Raw32(address)); }

  void SetShow(uint32_t element, bool show) {
    ctx_.r3.u64 = element;
    ctx_.r4.u64 = show ? 1 : 0;
    sub_8265D5A0(ctx_, base_);
  }

  Vec3 GetPosition(uint32_t element) {
    ctx_.r3.u64 = element;
    ctx_.r4.u64 = scratch_;
    sub_82180F38(ctx_, base_);
    Vec3 out;
    for (int i = 0; i < 3; ++i) {
      out.v[i] = std::bit_cast<float>(Load32(scratch_ + 4 * i));
    }
    return out;
  }

  void SetPosition(uint32_t element, const Vec3& pos) {
    for (int i = 0; i < 3; ++i) {
      Raw32(scratch_ + 4 * i) = std::byteswap(std::bit_cast<uint32_t>(pos.v[i]));
    }
    ctx_.r3.u64 = element;
    ctx_.r4.u64 = scratch_;
    sub_82180F88(ctx_, base_);
  }

 private:
  static constexpr uint32_t kFrameSize = 0x80;

  uint32_t& Raw32(uint32_t address) const {
    return *reinterpret_cast<uint32_t*>(base_ + address);
  }

  PPCContext& ctx_;
  uint8_t* base_;
  uint64_t saved_r1_;
  uint32_t scratch_;
};

void RemoveLiveButtons(PPCContext& ctx, uint8_t* base, uint32_t scene) {
  GuestCalls guest(ctx, base);
  std::vector<Vec3> slots;
  std::vector<uint32_t> kept;
  for (uint32_t offset : kLowerGroup) {
    uint32_t element = guest.Load32(scene + offset);
    if (!element) {
      return;  // Unexpected scene layout: leave the menu as the game built it.
    }
    slots.push_back(guest.GetPosition(element));
    if (IsRemoved(offset)) {
      guest.SetShow(element, false);
    } else {
      kept.push_back(element);
    }
  }
  // Remaining buttons take the topmost slots, keeping their order.
  for (size_t i = 0; i < kept.size(); ++i) {
    guest.SetPosition(kept[i], slots[i]);
  }
}

}  // namespace

extern "C" REX_FUNC(sub_8265BA38) {
  const uint32_t scene = ctx.r3.u32;
  const uint64_t return_address = ctx.lr;
  __imp__sub_8265BA38(ctx, base);

  const uint64_t result = ctx.r3.u64;
  if (int32_t(ctx.r3.u32) >= 0) {
    RemoveLiveButtons(ctx, base, scene);
  }
  // The caller only relies on r3, r1 and the non-volatile registers.
  ctx.r3.u64 = result;
  ctx.lr = return_address;
}
