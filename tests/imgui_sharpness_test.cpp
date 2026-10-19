// trials_hd - font rasterization and texture lifecycle without a GPU or desktop.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include <imgui.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/ui/overlay/achievement_toast.h>
#include <rex/ui/overlay/achievements_overlay.h>

#include "performance_overlay.h"
#include "ui_theme.h"

namespace {
void Check(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}

class Context final : public rex::ui::WindowedAppContext {
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};
class Window final : public rex::ui::Window {
 public:
  explicit Window(Context& context) : rex::ui::Window(context, "test", 2560, 1440) {}
  uint32_t dpi = 96;
  void Resize(uint32_t width, uint32_t height) {
    WindowDestructionReceiver receiver(this);
    OnActualSizeUpdate(width, height, receiver);
  }
 private:
  uint32_t GetLatestDpiImpl() const override { return dpi; }
  bool OpenImpl() override {
    WindowDestructionReceiver receiver(this);
    OnActualSizeUpdate(2560, 1440, receiver);
    return true;
  }
  void RequestCloseImpl() override {}
  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(rex::ui::Surface::TypeFlags) override {
    return nullptr;
  }
  void RequestPaintImpl() override {}
};
class Texture final : public rex::ui::ImmediateTexture {
 public:
  explicit Texture(int& live) : ImmediateTexture(1, 1), live_(live) { ++live_; }
  ~Texture() override { --live_; }
 private:
  int& live_;
};
class Drawer final : public rex::ui::ImmediateDrawer {
 public:
  int uploads = 0;
  int draws = 0;
  int live = 0;
  bool fail_next_upload = false;
  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(
      uint32_t width, uint32_t height, rex::ui::ImmediateTextureFilter,
      bool, const uint8_t* data) override {
    Check(width > 0 && height > 0 && data, "Atlas upload must contain pixels");
    ++uploads;
    if (fail_next_upload) {
      fail_next_upload = false;
      return nullptr;
    }
    return std::make_unique<Texture>(live);
  }
  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch&) override {}
  void Draw(const rex::ui::ImmediateDraw& draw) override {
    ++draws;
    Check(draw.texture != nullptr, "Text must reference an uploaded texture");
  }
  void EndDrawBatch() override {}
};
class Dialog final : public rex::ui::ImGuiDialog {
 public:
  explicit Dialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  float size = 32;
  float density = 1;
  const char* text = "Settings Controls Graphics Throttle Brake";
 private:
  void OnDraw(ImGuiIO&) override {
    ImGui::PushFont(nullptr, size);
    ImGui::Begin("Sharpness");
    ImGui::TextUnformatted(text);
    const auto* baked = ImGui::GetFontBaked();
    Check(std::abs(baked->Size - size) < 0.01f,
          "Font must be rasterized at requested size, not stretch a 10px atlas");
    Check(std::abs(baked->RasterizerDensity - density) < 0.01f,
          "Font rasterization must match framebuffer DPI");
    ImGui::End();
    ImGui::PopFont();
  }
};

void CheckDimming(bool expected, float width, float height) {
  const auto* background = ImGui::GetBackgroundDrawList();
  Check(background->VtxBuffer.Size == (expected ? 4 : 0),
        "Menus need exactly one dim rectangle; passive overlays must not dim");
  if (expected) {
    Check(background->VtxBuffer[0].pos.x == 0 && background->VtxBuffer[0].pos.y == 0 &&
              background->VtxBuffer[2].pos.x == width &&
              background->VtxBuffer[2].pos.y == height,
          "Dim layer must cover the whole logical viewport");
    Check((background->VtxBuffer[0].col >> IM_COL32_A_SHIFT) >= 128,
          "Dim layer must visibly darken the game");
  }
}

class Achievements final : public rex::ui::AchievementsOverlayDialog {
 public:
  Achievements(rex::ui::ImGuiDrawer* drawer, rex::system::AchievementManager* manager)
      : AchievementsOverlayDialog(drawer, nullptr, nullptr, manager) {}
  ImVec2 size;
  ImVec2 position;
  float glyph_height = 0;
  float scroll = 0;
 private:
  void OnDraw(ImGuiIO& io) override {
    AchievementsOverlayDialog::OnDraw(io);
    ImGui::Begin("Achievements##overlay");
    size = ImGui::GetWindowSize();
    position = ImGui::GetWindowPos();
    glyph_height = 0;
    const auto& vertices = ImGui::GetWindowDrawList()->VtxBuffer;
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
    for (int i = 0; i + 3 < vertices.Size; i += 4) {
      if (vertices[i].col == color) {
        glyph_height = std::max(glyph_height, vertices[i + 2].pos.y - vertices[i].pos.y);
      }
    }
    ImGui::BeginChild("##achlist");
    scroll = ImGui::GetScrollY();
    ImGui::EndChild();
    ImGui::End();
  }
};

class Counter final : public trials_hd::PerformanceOverlay {
 public:
  explicit Counter(rex::ui::ImGuiDrawer* drawer) : PerformanceOverlay(drawer) {}
  ImVec2 position, size;
 private:
  void OnDraw(ImGuiIO& io) override {
    ImGui::LogToClipboard();
    PerformanceOverlay::OnDraw(io);
    ImGui::LogFinish();
    ImGui::Begin("##performance_overlay");
    position = ImGui::GetWindowPos();
    size = ImGui::GetWindowSize();
    ImGui::End();
  }
};

void CheckMenuComposition(Window& window, Drawer& target) {
  std::string counter_text;
  rex::ui::ImGuiDrawer imgui(&window, 0, nullptr, trials_hd::ApplyUiTheme);
  auto& platform = ImGui::GetPlatformIO();
  platform.Platform_ClipboardUserData = &counter_text;
  platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
    *static_cast<std::string*>(ImGui::GetPlatformIO().Platform_ClipboardUserData) = text;
  };
  imgui.SetImmediateDrawer(&target);
  rex::ui::AppUIDrawContext frame(2560, 1440);
  Counter overlay(&imgui);
  rex::ui::AchievementToastDialog toast(&imgui, nullptr, nullptr);
  imgui.Draw(frame);
  imgui.Draw(frame);
  Check(counter_text.find_first_of("0123456789") != std::string::npos &&
            counter_text.find_first_not_of("0123456789 \r\n\t") == std::string::npos,
        "FPS counter must render one number without units, stats or a graph");
  Check(overlay.position.x == 20 && overlay.position.y == 20,
        "FPS counter must anchor at the top-left");
  Check(overlay.size.x < 200 && overlay.size.y < 80, "FPS counter must remain compact");
  CheckDimming(false, 2560, 1440);
  {
    Dialog menu(&imgui);
    imgui.Draw(frame);
    CheckDimming(true, 2560, 1440);
    {
      Dialog second_menu(&imgui);
      imgui.Draw(frame);
      CheckDimming(true, 2560, 1440);
    }
    imgui.Draw(frame);
    CheckDimming(true, 2560, 1440);
  }
  imgui.Draw(frame);
  CheckDimming(false, 2560, 1440);
  rex::system::AchievementEvent event;
  event.achievement.label = "Synthetic notification";
  toast.Push(event);
  imgui.Draw(frame);
  CheckDimming(false, 2560, 1440);

  rex::system::AchievementManager manager;
  for (uint32_t id = 1; id <= 12; ++id) {
    rex::system::AchievementInfo info;
    info.id = id;
    info.label = "Synthetic achievement";
    info.unachieved_description = "Long synthetic description to exercise wrapping in the list.";
    manager.RegisterAchievement(std::move(info));
  }
  Achievements achievements(&imgui, &manager);
  struct Viewport { uint32_t width, height, dpi; };
  for (const auto viewport : {Viewport{2560, 1440, 96}, Viewport{1280, 720, 96},
                              Viewport{2560, 1440, 192}, Viewport{2560, 1440, 120}}) {
    window.dpi = viewport.dpi;
    window.Resize(viewport.width, viewport.height);
    rex::ui::AppUIDrawContext resized_frame(viewport.width, viewport.height);
    imgui.Draw(resized_frame);
    imgui.Draw(resized_frame);
    const float density = float(viewport.dpi) / 96.0f;
    const float width = viewport.width / density;
    const float height = viewport.height / density;
    const float scale = std::max(1.0f, height / 720.0f) * 1.6f;
    Check(std::abs(achievements.size.x - 520.0f * scale) <= 1.0f,
          "Achievements width must match the Settings menu scale");
    Check(achievements.size.y <= height * 0.92f + 1.0f,
          "Achievements must fit the viewport and scroll instead");
    Check(std::abs(achievements.position.x + achievements.size.x / 2 - width / 2.0f) <= 1,
          "Achievements must be horizontally centered");
    Check(achievements.glyph_height >= 10.0f * scale * 0.6f,
          "Achievements text must match the Settings font scale");
    CheckDimming(true, width, height);
  }
  window.dpi = 96;
  window.Resize(2560, 1440);
  int page_request = 1;
  bool close_request = false;
  bool closed = false;
  achievements.SetOnClose([&] { closed = true; });
  achievements.SetScrollInputProvider([&] {
    rex::ui::AchievementsOverlayDialog::ScrollInput input;
    input.page = page_request;
    input.close = close_request;
    page_request = 0;
    return input;
  });
  imgui.Draw(frame);
  imgui.Draw(frame);
  Check(achievements.scroll > 0, "Controller page-down must scroll the achievements list");
  page_request = -1;
  imgui.Draw(frame);
  imgui.Draw(frame);
  Check(achievements.scroll <= 1, "Controller page-up must return to the top");
  close_request = true;
  imgui.Draw(frame);
  Check(closed, "Controller close action must invoke the owner's safe close callback");
  imgui.SetImmediateDrawer(nullptr);
}
}

int main() {
  Context context;
  Window window(context);
  Check(window.Open(), "Open fake window");
  Drawer target;
  rex::ui::ImGuiDrawer imgui(&window, 0);
  imgui.SetImmediateDrawer(&target);
  rex::ui::AppUIDrawContext frame(2560, 1440);
  {
    Dialog dialog(&imgui);
    imgui.Draw(frame);
    Check(target.uploads > 0, "Upload initial atlas");
    const int first_uploads = target.uploads;
    imgui.Draw(frame);
    imgui.Draw(frame);
    const int stable_uploads = target.uploads;
    imgui.Draw(frame);
    Check(target.uploads == stable_uploads, "Unchanged UI must not upload every frame");
    dialog.size = 40;
    dialog.text = "New glyphs: 0123456789 !?";
    imgui.Draw(frame);
    Check(target.uploads > first_uploads, "New size/glyphs must update atlas");
    for (const auto dpi : {120u, 144u, 192u, 96u}) {
      window.dpi = dpi;
      dialog.density = float(dpi) / 96.0f;
      imgui.Draw(frame);
      Check(std::abs(imgui.GetIO().DisplaySize.y - 1440.0f / dialog.density) < 0.01f,
            "Keep logical UI coordinates consistent with physical framebuffer");
    }
    // Failed uploads must skip rendering stale atlas coordinates and retry.
    target.fail_next_upload = true;
    target.draws = 0;
    dialog.size = 48;
    imgui.Draw(frame);
    Check(target.draws == 0, "Failed upload must not render invalid atlas data");
    imgui.Draw(frame);
    Check(target.draws > 0, "Retry must recover without recreating the drawer");
    for (float size : {16.0f, 24.0f, 32.0f, 80.0f, 120.0f}) {
      dialog.size = size;
      imgui.Draw(frame);
    }
    imgui.SetImmediateDrawer(nullptr);
    Check(target.live == 0, "Detach must release atlas textures");
    imgui.SetImmediateDrawer(&target);
    imgui.Draw(frame);
    Check(target.live > 0, "Reattach must recreate atlas textures");
  }
  const int idle_uploads = target.uploads;
  imgui.Draw(frame);
  Check(target.uploads == idle_uploads, "No dialogs must not trigger uploads");
  imgui.SetImmediateDrawer(nullptr);
  Check(target.live == 0, "No atlas texture leaks");
  {
    rex::ui::ImGuiDrawer temporary(&window, 0);
    temporary.SetImmediateDrawer(&target);
    Dialog dialog(&temporary);
    temporary.Draw(frame);
  }
  Check(target.live == 0, "Drawer destructor must release attached atlas textures");
  CheckMenuComposition(window, target);
  Check(target.live == 0, "Menu composition must not leak textures");
}
