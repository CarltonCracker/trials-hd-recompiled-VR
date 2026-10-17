#include <rex/graphics/trials_fast_path.h>

#include <cassert>
#include <string>
#include <thread>
#include <vector>

using namespace rex::graphics::trials;

int main() {
  assert(Enabled());
  rex::cvar::SetFlagByName("vulkan_trials_fast_path", "false");
  assert(!Enabled());
  rex::cvar::SetFlagByName("vulkan_trials_fast_path", "true");
  assert(Enabled());
  assert(TakeReport(200).empty());

  int evaluations = 0;
  assert(TRIALS_EXPECT(++evaluations == 1, Fallback::kUploadGpuWritten));
  assert(evaluations == 1 && TakeReport(200).empty());
  assert(!TRIALS_EXPECT(++evaluations == 0, Fallback::kUploadGpuWritten));
  assert(evaluations == 2);
  // Rare contract violations must not round to 0.0 per frame.
  assert(TakeReport(200) == " fallbacks: upload_gpu_written=1");
  assert(TakeReport(200).empty());

  CountStat(Stat::kUploadPagesHoisted, 400);
  CountFallback(Fallback::kRenderTargetFormatListUnsupported, 3);
  assert(TakeReport(200) ==
         " fast path/frame: upload_pages_hoisted=2.0 fallbacks: g8_format_list_unsupported=3");
  assert(TakeReport(200).empty());

  std::vector<std::jthread> producers;
  for (int i = 0; i < 4; ++i) {
    producers.emplace_back([] {
      for (int n = 0; n < 1000; ++n) CountFallback(Fallback::kUploadReadChanged);
    });
  }
  producers.clear();
  assert(TakeReport(200) == " fallbacks: upload_read_changed=4000");
  assert(TakeReport(200).empty());

  CountStat(Stat::kOcclusionQueries);
  CountFallback(Fallback::kUploadPageInOrder);
  assert(TakeReport(0) ==
         " fast path/frame: occlusion_queries=1.0 fallbacks: upload_page_in_order=1");
}
