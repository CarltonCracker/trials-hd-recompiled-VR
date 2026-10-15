// trials_hd - Xbox Live Arcade splash screen control.

#include <iterator>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(show_arcade_splash, false, "Display",
                    "Show the Xbox Live Arcade splash screen on boot")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REX_EXTERN(__imp__sub_82186030);
REX_EXTERN(sub_82181268);
REX_EXTERN(sub_82183720);

extern "C" REX_FUNC(sub_82186030) {
  uint32_t r6_addr = ctx.r6.u32;
  __imp__sub_82186030(ctx, base);
  uint32_t scene_obj = r6_addr ? rex::memory::load_and_swap<uint32_t>(base + r6_addr) : 0;

  if (!REXCVAR_GET(show_arcade_splash) && scene_obj != 0) {
    // Look up "Live Arcade Logo" child in the startup scene.
    // 2 KB below SP is safely beyond any child stack frames.
    static const char16_t kLogoName[] = u"Live Arcade Logo";
    const uint32_t name_addr = (ctx.r1.u32 - 2048) & ~15u;
    for (size_t i = 0; i < std::size(kLogoName); ++i) {
      rex::memory::store_and_swap<uint16_t>(base + name_addr + i * 2, kLogoName[i]);
    }

    const uint32_t out_child_addr = name_addr + 64;
    rex::memory::store_and_swap<uint32_t>(base + out_child_addr, 0);

    PPCContext sub_ctx = ctx;
    sub_ctx.r3.u64 = scene_obj;
    sub_ctx.r4.u64 = name_addr;
    sub_ctx.r5.u64 = out_child_addr;
    sub_82181268(sub_ctx, base);

    uint32_t child_obj = rex::memory::load_and_swap<uint32_t>(base + out_child_addr);
    REXLOG_INFO("XuiElementGetChildById result: child=0x{:08X}", child_obj);
    if (child_obj != 0) {
      REXLOG_INFO("Hiding Xbox Live Arcade splash logo on boot");
      sub_ctx = ctx;
      sub_ctx.r3.u64 = child_obj;
      sub_ctx.f1.f64 = -5000.0;
      sub_ctx.f2.f64 = -5000.0;
      sub_82183720(sub_ctx, base);
    }
  }
}
