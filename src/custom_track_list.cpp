// trials_hd - list every custom track, not only the first 301.
//
// The custom track list (the object built by sub_825BEFA8) finds tracks with one
// XamContentCreateEnumerator call for saved games (content type 1, 301 items per
// enumerate) and one overlapped XamEnumerate into a buffer inside the object
// (+428, 301 x 308-byte XCONTENT_DATA = 92708 bytes, `li r7,301` in sub_825BED48).
// The state machine sub_825BF1E0 then closes the enumerator and opens the items one
// by one: state 2 opens item [index], states 3-6 read the track, and sub_825BCB78
// moves to the next item (index + 1 < count: state 2, else state 0 and the "done"
// event). Tracks go into a linked list of 96-byte nodes, so nothing after the
// buffer has a limit; with 493 tracks installed the game opened 301 and stopped.
//
// When sub_825BCB78 is about to finish a full batch, this refills the buffer with
// the next 301 items of the same listing (what the SDK's enumerator would return on
// the next XamEnumerate) and sets the index to -1, so the original moves to item 0
// of the new batch and the state machine keeps going.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xam/user_profile.h>

REX_EXTERN(__imp__sub_825BED48);
REX_EXTERN(__imp__sub_825BCB78);

namespace {

using rex::system::xam::XCONTENT_DATA;

// Fields of the custom track list object.
constexpr uint32_t kState = 384;           // state machine step, 1 = enumerating
constexpr uint32_t kItems = 428;           // XCONTENT_DATA[kBatch]
constexpr uint32_t kItemCount = 0x16BD0;   // items in kItems (from XamEnumerate)
constexpr uint32_t kItemIndex = 0x16BE0;   // item being opened
constexpr uint32_t kBatch = 301;           // the buffer's capacity

// Items of the current listing already handed to the game; 0 until the first
// batch is used up. Only the state machine touches the list, under its lock.
size_t g_delivered = 0;

// What XamContentCreateEnumerator(device 0, saved games) lists, in its order: the
// player's content, then content shared by all players.
std::vector<rex::system::xam::XCONTENT_AGGREGATE_DATA> ListSavedGames() {
  using rex::system::XContentType;
  using rex::system::xam::DummyDeviceId;
  auto* kernel = REX_KERNEL_STATE();
  const uint64_t xuid = kernel->user_profile()->xuid();
  auto* content = kernel->content_manager();
  auto items = content->ListContent(uint32_t(DummyDeviceId::HDD), xuid, XContentType::kSavedGame);
  if (xuid != 0) {
    auto common = content->ListContent(uint32_t(DummyDeviceId::HDD), 0, XContentType::kSavedGame);
    items.insert(items.end(), common.begin(), common.end());
  }
  return items;
}

}  // namespace

// Starts a new enumeration (or flags one for later when the list is busy).
extern "C" REX_FUNC(sub_825BED48) {
  const uint32_t list = ctx.r3.u32;
  __imp__sub_825BED48(ctx, base);
  if (rex::memory::load_and_swap<uint32_t>(base + list + kState) == 1) {
    g_delivered = 0;
  }
}

// Moves to the next item; at the end of a full batch, loads the next one first.
extern "C" REX_FUNC(sub_825BCB78) {
  const uint32_t list = ctx.r3.u32;
  const uint32_t index = rex::memory::load_and_swap<uint32_t>(base + list + kItemIndex);
  const uint32_t count = rex::memory::load_and_swap<uint32_t>(base + list + kItemCount);
  if (count == kBatch && index + 1 >= count) {
    if (g_delivered == 0) {
      g_delivered = count;
    }
    const auto items = ListSavedGames();
    if (items.size() > g_delivered) {
      const size_t n = std::min<size_t>(kBatch, items.size() - g_delivered);
      for (size_t i = 0; i < n; ++i) {
        // The enumerator copies the XCONTENT_DATA part of each item (already
        // big-endian), as XStaticEnumerator<XCONTENT_DATA> does.
        std::memcpy(base + list + kItems + i * sizeof(XCONTENT_DATA), &items[g_delivered + i],
                    sizeof(XCONTENT_DATA));
      }
      REXLOG_INFO("Custom tracks: listing items {}-{} of {}", g_delivered + 1, g_delivered + n,
                  items.size());
      g_delivered += n;
      rex::memory::store_and_swap<uint32_t>(base + list + kItemCount, uint32_t(n));
      rex::memory::store_and_swap<uint32_t>(base + list + kItemIndex, UINT32_MAX);
    } else {
      g_delivered = 0;
    }
  }
  __imp__sub_825BCB78(ctx, base);
}
