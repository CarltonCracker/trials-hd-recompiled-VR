// trials_hd - first-start setup screen (see installer_dialog.h).

#include "installer/installer_dialog.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>

// Fill in the setup screen from the command line and install without a click
// (read by TrialsHdApp::OnFinalizePaths).
REXCVAR_DEFINE_STRING(install_game, "", "Setup",
                      "Install the game from this package, or a folder with the game and DLC "
                      "packages, then start it");
REXCVAR_DEFINE_STRING(install_dlc, "", "Setup",
                      "Install DLC from this package or folder into the installed game, then "
                      "start it");

namespace trials_hd {

namespace fs = std::filesystem;

namespace {

constexpr ImVec4 kGood(0.55f, 0.85f, 0.45f, 1.0f);
constexpr ImVec4 kWarning(1.0f, 0.75f, 0.35f, 1.0f);
constexpr ImVec4 kError(1.0f, 0.45f, 0.40f, 1.0f);

std::string Megabytes(uint64_t bytes) {
  return fmt::format("{:.1f} MB", double(bytes) / (1024.0 * 1024.0));
}

fs::path HomeDir() {
  if (const char* home = std::getenv("HOME"); home && *home) return home;
  return "/";
}

// Starting points for the browser: home, the file system root, and where
// removable drives (an SD card on a Steam Deck, a USB stick) are mounted.
std::vector<std::pair<std::string, fs::path>> Places() {
  std::vector<std::pair<std::string, fs::path>> places = {{"Home", HomeDir()}, {"Computer", "/"}};
  std::error_code ec;
  std::vector<fs::path> media;
  if (const char* user = std::getenv("USER"); user && *user) {
    media.push_back(fs::path("/run/media") / user);
  }
  media.insert(media.end(), {"/media", "/mnt", "/Volumes"});
  for (const auto& dir : media) {
    if (fs::is_directory(dir, ec) && !fs::is_empty(dir, ec)) {
      places.emplace_back(dir.string(), dir);
    }
  }
  return places;
}

std::string Lower(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char c) { return std::tolower(c); });
  return text;
}

void Label(const char* text, float width) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(text);
  ImGui::SameLine(width);
}

}  // namespace

InstallerDialog::InstallerDialog(rex::ui::ImGuiDrawer* drawer, Options options,
                                 std::function<void(const fs::path&)> on_installed,
                                 std::function<void()> on_dismissed)
    : ImGuiDialog(drawer),
      options_(std::move(options)),
      on_installed_(std::move(on_installed)),
      on_dismissed_(std::move(on_dismissed)) {
  // The runtime's input system (which opens controllers for the game) does
  // not exist yet; SDL counts subsystem users, so it can init it again later.
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    REXLOG_WARN("Setup: no controller support: {}", SDL_GetError());
  }
  game_already_installed_ = installer::IsGameInstalled(options_.install_dir);
  nav_.IgnoreHeld();
  for (auto& path : options_.initial_paths) {
    pending_scans_.push_back(path);
  }
  browse_dir_ = HomeDir();
}

InstallerDialog::~InstallerDialog() {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
  for (SDL_Gamepad* pad : gamepads_) {
    SDL_CloseGamepad(pad);
  }
  SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void InstallerDialog::AddPath(fs::path path) {
  if (screen_ == Screen::kInstalling) return;
  pending_scans_.push_back(std::move(path));
  if (screen_ == Screen::kBrowse || screen_ == Screen::kFailed) {
    screen_ = Screen::kChoose;
    focus_next_ = true;
  }
}

// Quit (first start) or Cancel / OK (in game).
void InstallerDialog::Dismiss() {
  if (on_dismissed_) on_dismissed_();
  Close();  // Deleted after this frame; stops a scan in progress.
}

void InstallerDialog::OnClose() {
  ImGuiIO& io = GetIO();
  io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
  nav_.Release(io);
}

// Opens controllers as they connect (GamepadNav reads opened ones).
void InstallerDialog::OpenGamepads() {
  int count = 0;
  if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
    for (int i = 0; i < count; ++i) {
      if (!SDL_GetGamepadFromID(ids[i])) {
        if (SDL_Gamepad* pad = SDL_OpenGamepad(ids[i])) {
          gamepads_.push_back(pad);
        }
      }
    }
    SDL_free(ids);
  }
}

void InstallerDialog::PumpWork() {
  if (!worker_done_.load(std::memory_order_acquire)) return;
  if (worker_.joinable()) worker_.join();

  if (installing_) {
    installing_ = false;
    if (install_ok_) {
      REXLOG_INFO("Setup: installed to {}", options_.install_dir.string());
      if (on_installed_) on_installed_(options_.install_dir);
      if (options_.in_game) {
        screen_ = Screen::kDone;
        focus_next_ = true;
        return;
      }
      Close();
      return;
    }
    REXLOG_WARN("Setup: install failed: {}", install_error_);
    screen_ = install_error_ == "Cancelled" ? Screen::kChoose : Screen::kFailed;
    focus_next_ = true;
  }
  if (scan_result_) {
    MergeScan(std::move(*scan_result_));
    scan_result_.reset();
    scanning_.clear();
  }
  if (!pending_scans_.empty()) {
    StartScan(std::move(pending_scans_.front()));
    pending_scans_.pop_front();
    return;
  }
  // --install_game: once its scans are done, install if there is something to.
  if (options_.auto_install) {
    options_.auto_install = false;
    if (CanInstall() && screen_ == Screen::kChoose) StartInstall();
  }
}

void InstallerDialog::StartScan(fs::path path) {
  REXLOG_INFO("Setup: looking for packages in {}", path.string());
  scanning_ = path;
  worker_done_ = false;
  worker_ = std::jthread([this, path = std::move(path)](std::stop_token stop) {
    scan_result_ = installer::Scan(path, stop);
    worker_done_.store(true, std::memory_order_release);
  });
}

void InstallerDialog::MergeScan(installer::ScanResult result) {
  bool found = false;
  for (auto& source : result.sources) {
    if (source.kind == installer::SourceKind::kBaseGame) {
      if (game_already_installed_ && !options_.reinstall_game) {
        REXLOG_INFO("Setup: game already installed; not reinstalling {}",
                    source.path.string());
        continue;
      }
      REXLOG_INFO("Setup: game package {}", source.path.string());
      game_ = std::move(source);
      found = true;
      continue;
    }
    const bool known = std::ranges::any_of(
        dlc_, [&](const auto& dlc) { return dlc.package_id == source.package_id; });
    if (!known) {
      REXLOG_INFO("Setup: DLC '{}' at {}", source.display_name, source.path.string());
      const bool installed = installer::IsDlcInstalled(options_.install_dir, source.package_id);
      dlc_.push_back(std::move(source));
      dlc_selected_.push_back(!installed);
      dlc_installed_.push_back(installed);
      found = true;
    }
  }
  for (auto& problem : result.problems) {
    REXLOG_WARN("Setup: {}", problem);
    problems_.push_back(std::move(problem));
  }
  if (found) {
    focus_next_ = true;  // Onto Install, now that there may be something to install.
  }
  if (!found && result.problems.empty()) {
    problems_.push_back(fmt::format("Nothing new to install in {}", scanning_.string()));
  }
  // Only the latest few; older ones scroll away.
  if (problems_.size() > 4) {
    problems_.erase(problems_.begin(), problems_.end() - 4);
  }
}

bool InstallerDialog::CanInstall() const {
  const bool any_dlc = std::ranges::find(dlc_selected_, true) != dlc_selected_.end();
  return game_ || (game_already_installed_ && any_dlc);
}

void InstallerDialog::StartInstall() {
  installer::Plan plan;
  plan.base_game = game_;
  for (size_t i = 0; i < dlc_.size(); ++i) {
    if (dlc_selected_[i]) plan.dlc.push_back(dlc_[i]);
  }
  REXLOG_INFO("Setup: installing to {} ({} DLC)", options_.install_dir.string(), plan.dlc.size());
  screen_ = Screen::kInstalling;
  focus_next_ = true;
  installing_ = true;
  install_error_.clear();
  progress_.SetStage("Starting");
  progress_.bytes_done = 0;
  worker_done_ = false;
  worker_ = std::jthread([this, plan = std::move(plan)](std::stop_token stop) {
    std::string error;
    install_ok_ = installer::Install(plan, options_.install_dir, progress_, stop, &error);
    install_error_ = std::move(error);
    worker_done_.store(true, std::memory_order_release);
  });
}

void InstallerDialog::OnDraw(ImGuiIO& io) {
  OpenGamepads();
  {
    std::vector<fs::path> picked;
    {
      std::lock_guard lock(inbox_->mutex);
      picked.swap(inbox_->paths);
      for (auto& error : inbox_->errors) problems_.push_back(std::move(error));
      inbox_->errors.clear();
    }
    for (auto& path : picked) AddPath(std::move(path));
  }
  PumpWork();

  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  nav_.Feed(io);

  const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  // Sized for reading from a couch, like the settings menu.
  const float scale = std::max(1.0f, io.DisplaySize.y / 720.0f) * 1.4f;
  ImGui::SetNextWindowSize(ImVec2(std::min(620 * scale, io.DisplaySize.x * 0.96f), 0),
                           ImGuiCond_Always);
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, io.DisplaySize.y * 0.94f));
  if (focus_next_) {
    ImGui::SetNextWindowFocus();
  }
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);
  if (ImGui::Begin(options_.in_game ? "Add DLC###setup" : "Trials HD Setup###setup", nullptr,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
    switch (screen_) {
      case Screen::kChoose:
        DrawChoose(scale);
        break;
      case Screen::kBrowse:
        DrawBrowse(scale);
        break;
      case Screen::kInstalling:
        DrawInstalling(scale);
        break;
      case Screen::kFailed:
        DrawFailed(scale);
        break;
      case Screen::kDone:
        DrawDone();
        break;
    }
  }
  ImGui::End();
  ImGui::PopFont();
}

// Gives the next control the keyboard / controller focus on a new screen,
// highlighted: ImGui ignores A until the nav cursor is visible, and a player
// with only a controller should see where they are from the start.
void InstallerDialog::FocusHere() {
  if (focus_next_) {
    ImGui::SetKeyboardFocusHere();
    ImGui::SetNavCursorVisible(true);
    focus_next_ = false;
  }
}

void InstallerDialog::DrawChoose(float scale) {
  const bool busy = !worker_done_ || !pending_scans_.empty();
  const float label_width = 90.0f * scale;

  if (options_.in_game) {
    ImGui::TextWrapped(
        "Select Big Pack or Big Thrills DLC packages (the files in 5841095A/00000002/), "
        "extracted DLC folders, or a folder that contains them. You can also drop them onto "
        "this window.");
  } else {
    ImGui::TextWrapped(
        "Trials HD Recompiled plays your own copy of Trials HD (Xbox Live Arcade). Select the "
        "game package (the file in 5841095A/000D0000/) or a folder that contains it. Big Pack "
        "and Big Thrills DLC found there are added too. You can also drop files or folders onto "
        "this window.");
  }
  ImGui::Separator();

  // In game the game is running; only DLC goes on this screen.
  if (!options_.in_game) {
    Label("Game", label_width);
    if (game_) {
      ImGui::TextColored(kGood, "%s", game_->display_name.c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("(%s, %s)", game_->path.filename().string().c_str(),
                          Megabytes(game_->bytes).c_str());
    } else if (game_already_installed_) {
      ImGui::TextColored(kGood, "Installed");
    } else {
      ImGui::TextDisabled("Not selected");
    }
  }

  Label("DLC", label_width);
  if (dlc_.empty()) {
    ImGui::TextDisabled(options_.in_game ? "None selected" : "None (optional)");
  } else {
    ImGui::BeginGroup();
    for (size_t i = 0; i < dlc_.size(); ++i) {
      bool selected = dlc_selected_[i];
      const auto label =
          fmt::format("{}  ({}{})##dlc{}", dlc_[i].display_name, Megabytes(dlc_[i].bytes),
                      dlc_installed_[i] ? ", installed" : "", i);
      if (ImGui::Checkbox(label.c_str(), &selected)) {
        dlc_selected_[i] = selected;
      }
    }
    ImGui::EndGroup();
  }

  if (busy) {
    ImGui::TextDisabled("Searching %s...",
                        scanning_.empty() ? "" : scanning_.filename().string().c_str());
  }
  for (const auto& problem : problems_) {
    ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
    ImGui::TextWrapped("%s", problem.c_str());
    ImGui::PopStyleColor();
  }

  ImGui::Separator();
  const bool can_install = !busy && CanInstall();
  if (!can_install) FocusHere();
  const bool want_dlc = game_ || options_.in_game;
  if (ImGui::Button(want_dlc ? "Browse for DLC..." : "Browse for the game...")) {
    OpenBrowser(browse_dir_);
  }
  ImGui::SameLine();
  if (ImGui::Button("System file picker...")) {
    ShowSystemPicker(/*folder=*/false);
  }
  ImGui::SameLine();
  if (ImGui::Button("Folder...")) {
    ShowSystemPicker(/*folder=*/true);
  }

  ImGui::Spacing();
  ImGui::BeginDisabled(!can_install);
  if (can_install) FocusHere();
  const bool dlc_only = !game_ && game_already_installed_;
  const char* install_label = options_.in_game ? "Install DLC"
                              : dlc_only       ? "Install DLC and play"
                                               : "Install and play";
  if (ImGui::Button(install_label)) {
    StartInstall();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(options_.in_game ? "Cancel" : "Quit")) {
    Dismiss();
    return;
  }
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("Installs to %s", options_.install_dir.string().c_str());
  ImGui::PopStyleColor();
  // In game, B leaves like it does the settings menu.
  if (options_.in_game && nav_.Pressed(GamepadNav::kBack)) {
    Dismiss();
  }
}

void InstallerDialog::OpenBrowser(fs::path dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) dir = HomeDir();
  browse_dir_ = std::move(dir);
  const auto text = browse_dir_.string();
  std::snprintf(path_input_, sizeof(path_input_), "%s", text.c_str());
  ListBrowseDir();
  screen_ = Screen::kBrowse;
  focus_next_ = true;
}

void InstallerDialog::ListBrowseDir() {
  browse_entries_.clear();
  browse_error_.clear();
  std::error_code ec;
  for (auto it = fs::directory_iterator(browse_dir_, fs::directory_options::skip_permission_denied,
                                        ec);
       !ec && it != fs::end(it); it.increment(ec)) {
    BrowseEntry entry;
    entry.name = it->path().filename().string();
    if (entry.name.starts_with('.')) continue;
    std::error_code entry_ec;
    entry.is_dir = it->is_directory(entry_ec);
    if (!entry.is_dir) {
      if (!it->is_regular_file(entry_ec)) continue;
      entry.size = it->file_size(entry_ec);
    }
    browse_entries_.push_back(std::move(entry));
  }
  if (ec) browse_error_ = ec.message();
  std::ranges::sort(browse_entries_, [](const BrowseEntry& a, const BrowseEntry& b) {
    if (a.is_dir != b.is_dir) return a.is_dir;
    return Lower(a.name) < Lower(b.name);
  });
}

void InstallerDialog::DrawBrowse(float scale) {
  FocusHere();
  if (ImGui::Button("Use this folder")) {
    AddPath(browse_dir_);
    return;
  }
  ImGui::SameLine();
  if (ImGui::Button("Up") && browse_dir_.has_parent_path()) {
    OpenBrowser(browse_dir_.parent_path());
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    screen_ = Screen::kChoose;
    focus_next_ = true;
    return;
  }
  for (const auto& [name, dir] : Places()) {
    ImGui::SameLine();
    if (ImGui::Button(name.c_str())) {
      OpenBrowser(dir);
    }
  }

  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::InputText("##path", path_input_, sizeof(path_input_),
                       ImGuiInputTextFlags_EnterReturnsTrue)) {
    fs::path typed(path_input_);
    std::error_code ec;
    if (fs::is_directory(typed, ec)) {
      OpenBrowser(typed);
    } else if (fs::exists(typed, ec)) {
      AddPath(typed);
      return;
    } else {
      browse_error_ = "No such file or folder";
    }
  }
  if (!browse_error_.empty()) {
    ImGui::TextColored(kWarning, "%s", browse_error_.c_str());
  }
  ImGui::TextDisabled("A: open folder or choose package   B: up   Use this folder: search all of it");

  const float list_height = std::max(160.0f * scale, ImGui::GetIO().DisplaySize.y * 0.5f);
  // Flattened: the d-pad moves from the buttons into the list and back.
  if (ImGui::BeginChild("##entries", ImVec2(0, list_height),
                        ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened)) {
    std::optional<fs::path> open_dir;
    std::optional<fs::path> choose;
    for (size_t i = 0; i < browse_entries_.size(); ++i) {
      const auto& entry = browse_entries_[i];
      const auto label = entry.is_dir
                             ? fmt::format("[{}]##e{}", entry.name, i)
                             : fmt::format("{}    {}##e{}", entry.name, Megabytes(entry.size), i);
      if (ImGui::Selectable(label.c_str())) {
        (entry.is_dir ? open_dir : choose) = browse_dir_ / entry.name;
      }
    }
    if (browse_entries_.empty()) {
      ImGui::TextDisabled("(empty)");
    }
    ImGui::EndChild();
    if (open_dir) {
      OpenBrowser(*open_dir);
    } else if (choose) {
      AddPath(*choose);
      return;
    }
  } else {
    ImGui::EndChild();
  }

  // B: up one folder (ImGui leaves text input on its own B).
  if (nav_.Pressed(GamepadNav::kBack) && !ImGui::GetIO().WantTextInput &&
      browse_dir_.has_parent_path() && browse_dir_ != browse_dir_.root_path()) {
    OpenBrowser(browse_dir_.parent_path());
  }
}

void InstallerDialog::DrawInstalling(float scale) {
  (void)scale;
  ImGui::TextUnformatted("Installing Trials HD...");
  const uint64_t total = progress_.bytes_total.load();
  const uint64_t done = std::min(progress_.bytes_done.load(), total);
  const float fraction = total ? float(double(done) / double(total)) : 0.0f;
  const auto overlay = fmt::format("{} / {}", Megabytes(done), Megabytes(total));
  ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), overlay.c_str());
  const auto stage = progress_.stage();
  ImGui::TextDisabled("%s", stage.c_str());
  ImGui::Spacing();
  FocusHere();
  if (ImGui::Button("Cancel")) {
    worker_.request_stop();
  }
}

void InstallerDialog::DrawDone() {
  ImGui::TextColored(kGood, "DLC installed.");
  ImGui::TextWrapped("Restart the game to play the new tracks.");
  ImGui::Spacing();
  FocusHere();
  if (ImGui::Button("OK") || nav_.Pressed(GamepadNav::kBack)) {
    Dismiss();
  }
}

void InstallerDialog::DrawFailed(float scale) {
  (void)scale;
  ImGui::TextColored(kError, "The install did not finish.");
  ImGui::TextWrapped("%s", install_error_.c_str());
  ImGui::Spacing();
  FocusHere();
  if (ImGui::Button("Back")) {
    screen_ = Screen::kChoose;
    focus_next_ = true;
  }
}

// The desktop's own file dialog (portal / GTK on Linux, Cocoa on macOS). It
// answers on another thread, possibly after this dialog is gone, so the
// callback only owns a reference to the inbox.
void InstallerDialog::ShowSystemPicker(bool folder) {
  auto* inbox = new std::shared_ptr<Inbox>(inbox_);
  auto callback = [](void* userdata, const char* const* files, int) {
    auto* holder = static_cast<std::shared_ptr<Inbox>*>(userdata);
    {
      std::lock_guard lock((*holder)->mutex);
      if (!files) {
        (*holder)->errors.push_back(
            fmt::format("The system file picker is not available ({}); use Browse instead.",
                        SDL_GetError()));
      } else {
        for (; *files; ++files) (*holder)->paths.emplace_back(*files);
      }
    }
    delete holder;
  };
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* window = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  const auto start = browse_dir_.string();
  if (folder) {
    SDL_ShowOpenFolderDialog(callback, inbox, window, start.c_str(), false);
  } else {
    SDL_ShowOpenFileDialog(callback, inbox, window, nullptr, 0, start.c_str(), true);
  }
}

}  // namespace trials_hd
