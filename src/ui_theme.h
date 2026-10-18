// trials_hd - ImGui theme matching the game's main menu: dark brown grunge
// panels, rust-orange accents and tan text (sampled from the menu itself).

#pragma once

#include <imgui.h>

#include <rex/ui/style.h>

namespace trials_hd {

// Applied from TrialsHdApp::OnConfigureStyle, after the SDK defaults.
void ApplyUiTheme(ImGuiStyle& style, rex::ui::Style& ui_style);

}  // namespace trials_hd
