// trials_hd - ImGui theme matching the game's main menu (see ui_theme.h).

#include "ui_theme.h"

#include <cstdint>

namespace trials_hd {
namespace {

constexpr ImVec4 Rgb(uint32_t rgb, float alpha = 1.0f) {
  return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                (rgb & 0xFF) / 255.0f, alpha);
}

// Sampled from the main menu (tests/headless/main_menu_nav.txt captures).
constexpr uint32_t kInk = 0x0B0908;       // Shadowed panel edges, selection bar.
constexpr uint32_t kPanel = 0x160F09;     // Menu panel body.
constexpr uint32_t kPanelLit = 0x22160C;  // Between the body and the lit top.
constexpr uint32_t kBrown = 0x3B230E;     // Lit top of the menu panel.
constexpr uint32_t kRust = 0x563313;      // Rust texture on the panel.
constexpr uint32_t kRustLit = 0x7A4A1E;
constexpr uint32_t kRustHot = 0x9A5F26;
constexpr uint32_t kOrange = 0xC07A32;    // Rider's kit / logo splatter accent.
constexpr uint32_t kTan = 0xD1BD8E;       // Menu item text.
constexpr uint32_t kCream = 0xE4DAC1;     // Selected menu item text.
constexpr uint32_t kGold = 0xE0AE3A;      // Selected menu item text, at its pulse.
constexpr uint32_t kMuted = 0x8F8675;     // Faded tan.
constexpr uint32_t kGreen = 0x72A858;     // The A button glyph.

}  // namespace

void ApplyUiTheme(ImGuiStyle& style, rex::ui::Style& ui_style) {
  ImVec4* c = style.Colors;
  c[ImGuiCol_Text] = Rgb(kTan);
  c[ImGuiCol_TextDisabled] = Rgb(kMuted);
  c[ImGuiCol_WindowBg] = Rgb(kPanel, 0.96f);
  c[ImGuiCol_ChildBg] = Rgb(kPanel, 0.0f);
  c[ImGuiCol_PopupBg] = Rgb(kPanelLit, 0.98f);
  c[ImGuiCol_Border] = Rgb(kBrown);
  c[ImGuiCol_BorderShadow] = Rgb(kInk, 0.0f);
  // Inset fields read like the menu's dark selection bar.
  c[ImGuiCol_FrameBg] = Rgb(kInk, 0.85f);
  c[ImGuiCol_FrameBgHovered] = Rgb(kBrown);
  c[ImGuiCol_FrameBgActive] = Rgb(kRust);
  c[ImGuiCol_TitleBg] = Rgb(kPanelLit);
  c[ImGuiCol_TitleBgActive] = Rgb(kBrown);
  c[ImGuiCol_TitleBgCollapsed] = Rgb(kPanel);
  c[ImGuiCol_MenuBarBg] = Rgb(kPanelLit);
  c[ImGuiCol_ScrollbarBg] = Rgb(kInk, 0.6f);
  c[ImGuiCol_ScrollbarGrab] = Rgb(kRust);
  c[ImGuiCol_ScrollbarGrabHovered] = Rgb(kRustLit);
  c[ImGuiCol_ScrollbarGrabActive] = Rgb(kRustHot);
  c[ImGuiCol_CheckMark] = Rgb(kCream);
  c[ImGuiCol_SliderGrab] = Rgb(kTan);
  c[ImGuiCol_SliderGrabActive] = Rgb(kCream);
  c[ImGuiCol_Button] = Rgb(kBrown);
  c[ImGuiCol_ButtonHovered] = Rgb(kRustLit);
  c[ImGuiCol_ButtonActive] = Rgb(kRustHot);
  c[ImGuiCol_Header] = Rgb(kRust, 0.85f);
  c[ImGuiCol_HeaderHovered] = Rgb(kRustLit);
  c[ImGuiCol_HeaderActive] = Rgb(kRustHot);
  c[ImGuiCol_Separator] = Rgb(kBrown);
  c[ImGuiCol_SeparatorHovered] = Rgb(kRustLit);
  c[ImGuiCol_SeparatorActive] = Rgb(kRustHot);
  c[ImGuiCol_ResizeGrip] = Rgb(kRust, 0.4f);
  c[ImGuiCol_ResizeGripHovered] = Rgb(kRustLit, 0.7f);
  c[ImGuiCol_ResizeGripActive] = Rgb(kRustHot, 0.9f);
  c[ImGuiCol_InputTextCursor] = Rgb(kCream);
  c[ImGuiCol_Tab] = Rgb(kPanelLit);
  c[ImGuiCol_TabHovered] = Rgb(kRustLit);
  c[ImGuiCol_TabSelected] = Rgb(kRust);
  c[ImGuiCol_TabSelectedOverline] = Rgb(kGold);
  c[ImGuiCol_TabDimmed] = Rgb(kPanel);
  c[ImGuiCol_TabDimmedSelected] = Rgb(kBrown);
  c[ImGuiCol_TabDimmedSelectedOverline] = Rgb(kRustLit);
  c[ImGuiCol_PlotLines] = Rgb(kTan);
  c[ImGuiCol_PlotLinesHovered] = Rgb(kCream);
  c[ImGuiCol_PlotHistogram] = Rgb(kOrange);
  c[ImGuiCol_PlotHistogramHovered] = Rgb(kCream);
  c[ImGuiCol_TableHeaderBg] = Rgb(kBrown);
  c[ImGuiCol_TableBorderStrong] = Rgb(kBrown);
  c[ImGuiCol_TableBorderLight] = Rgb(kPanelLit);
  c[ImGuiCol_TableRowBg] = Rgb(kInk, 0.0f);
  c[ImGuiCol_TableRowBgAlt] = Rgb(kCream, 0.04f);
  c[ImGuiCol_TextLink] = Rgb(kOrange);
  c[ImGuiCol_TextSelectedBg] = Rgb(kRustLit, 0.6f);
  c[ImGuiCol_TreeLines] = Rgb(kMuted, 0.5f);
  c[ImGuiCol_DragDropTarget] = Rgb(kCream, 0.9f);
  c[ImGuiCol_DragDropTargetBg] = Rgb(kInk, 0.0f);
  c[ImGuiCol_UnsavedMarker] = Rgb(kCream);
  c[ImGuiCol_NavCursor] = Rgb(kGold);
  c[ImGuiCol_NavWindowingHighlight] = Rgb(kCream, 0.7f);
  c[ImGuiCol_NavWindowingDimBg] = Rgb(kInk, 0.2f);
  c[ImGuiCol_ModalWindowDimBg] = Rgb(kInk, 0.6f);

  // The achievements list and unlock toast follow the same palette; the
  // console, debug and settings badge colors are status colors, left as is.
  auto& a = ui_style.achievements;
  a.unlocked_title = Rgb(kCream);
  a.unlocked_desc = Rgb(kTan);
  a.locked_title = Rgb(kMuted);
  a.locked_desc = Rgb(0x6B6355);
  a.badge_gamerscore = Rgb(kGold);
  a.row_unlocked_bg = Rgb(kRust, 0.55f);
  a.header_text = Rgb(kCream);
  a.progress_bar = Rgb(kGreen);
  ui_style.toast.text = Rgb(kCream);
  ui_style.toast.title = Rgb(kGold);
}

}  // namespace trials_hd
