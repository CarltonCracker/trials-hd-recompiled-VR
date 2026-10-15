// trials_hd - "Exit Game" main menu button and clean quit handler.

#include <cstdint>
#include <iterator>
#include <string_view>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "trials_hd_app.h"

REX_EXTERN(__imp__sub_8265BA38);
REX_EXTERN(__imp__sub_8265C648);
REX_EXTERN(__imp__sub_82124A38);

// MainMenu::OnInit - change "Return to Game Library" button to "Exit Game".
extern "C" REX_FUNC(sub_8265BA38) {
  uint32_t menu = ctx.r3.u32;
  __imp__sub_8265BA38(ctx, base);

  uint32_t return_button = rex::memory::load_and_swap<uint32_t>(base + menu + 72);
  if (return_button != 0) {

    // Replace all occurrences of L"RETURN TO GAME LIBRARY" in guest memory with L"Exit Game"
    static const char16_t kTarget[] = u"RETURN TO GAME LIBRARY";
    uint8_t target_bytes[(std::size(kTarget) - 1) * 2];
    for (size_t i = 0; i < std::size(kTarget) - 1; ++i) {
      target_bytes[i * 2] = (kTarget[i] >> 8) & 0xFF;
      target_bytes[i * 2 + 1] = kTarget[i] & 0xFF;
    }
    std::string_view target_sv(reinterpret_cast<const char*>(target_bytes), sizeof(target_bytes));

    static const char16_t kRepl[] = u"Exit Game";
    uint8_t repl_bytes[sizeof(kRepl)];
    for (size_t i = 0; i < std::size(kRepl); ++i) {
      repl_bytes[i * 2] = (kRepl[i] >> 8) & 0xFF;
      repl_bytes[i * 2 + 1] = kRepl[i] & 0xFF;
    }

    // Search 0x40000000 - 0x50000000 (guest heap range)
    for (uint32_t search_base = 0x40000000; search_base < 0x50000000; search_base += 0x100000) {
      std::string_view mem_sv(reinterpret_cast<const char*>(base + search_base), 0x100000);
      size_t pos = 0;
      while ((pos = mem_sv.find(target_sv, pos)) != std::string_view::npos) {
        uint32_t match_addr = search_base + static_cast<uint32_t>(pos);
        REXLOG_INFO("Replacing RETURN TO GAME LIBRARY with Exit Game at guest 0x{:08X}", match_addr);
        std::memcpy(base + match_addr, repl_bytes, sizeof(repl_bytes));
        pos += target_sv.size();
      }
    }
  }
}

// MainMenu button click handler - quit if ReturnButton is pressed.
extern "C" REX_FUNC(sub_8265C648) {
  uint32_t menu = ctx.r3.u32;
  uint32_t clicked_button = ctx.r4.u32;
  uint32_t return_button = rex::memory::load_and_swap<uint32_t>(base + menu + 72);
  if (clicked_button != 0 && clicked_button == return_button) {
    REXLOG_INFO("Exit Game button pressed; requesting application quit");
    if (auto* app = TrialsHdApp::Instance()) {
      app->Quit();
    }
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8265C648(ctx, base);
}

// Intercept return-to-dashboard launcher to quit cleanly instead of asserting.
extern "C" REX_FUNC(sub_82124A38) {
  REXLOG_INFO("sub_82124A38 (launch dashboard) called; requesting application quit");
  if (auto* app = TrialsHdApp::Instance()) {
    app->Quit();
  }
  ctx.r3.u64 = 0;
}

