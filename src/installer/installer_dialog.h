// trials_hd - first-start setup screen: asks for the player's Trials HD package
// (and optional DLC track packs), installs them into assets/ (game_installer.h)
// and hands the folder back so the game can boot. Shown before the runtime
// exists, so it brings its own controller handling (gamepad_nav.h). The same
// screen adds DLC and custom tracks to the running game (settings, Game tab).

#pragma once

#include <atomic>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <rex/ui/imgui_dialog.h>

#include "gamepad_nav.h"
#include "installer/game_installer.h"

struct SDL_Gamepad;

namespace trials_hd {

// Owns itself once shown: it deletes itself after the frame in which the first
// install finished (on_installed runs first) or the player left it
// (on_dismissed: Quit, or in game Cancel / OK).
class InstallerDialog : public rex::ui::ImGuiDialog {
 public:
  struct Options {
    std::filesystem::path install_dir;
    // Scanned at once (--install_game / --install_dlc).
    std::vector<std::filesystem::path> initial_paths;
    // Install as soon as the scans find what is needed, without a click.
    bool auto_install = false;
    // Take a base game package found while the game is already installed
    // (--install_game: repair); otherwise only DLC is added to an install.
    bool reinstall_game = false;
    // Adding DLC while the game runs: Cancel instead of Quit, and a done
    // screen (the game sees new DLC after a restart) instead of closing.
    bool in_game = false;
  };

  InstallerDialog(rex::ui::ImGuiDrawer* drawer, Options options,
                  std::function<void(const std::filesystem::path& assets_dir)> on_installed,
                  std::function<void()> on_dismissed);
  ~InstallerDialog() override;

  // A file or folder to look for packages in (dropped on the window). UI thread.
  void AddPath(std::filesystem::path path);

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  enum class Screen { kChoose, kBrowse, kInstalling, kFailed, kDone };

  struct BrowseEntry {
    std::string name;
    bool is_dir = false;
    uint64_t size = 0;
  };

  // Paths from the system file picker arrive on another thread.
  struct Inbox {
    std::mutex mutex;
    std::vector<std::filesystem::path> paths;
    std::vector<std::string> errors;
  };

  void OpenGamepads();
  void PumpWork();
  void StartScan(std::filesystem::path path);
  void MergeScan(installer::ScanResult result);
  void StartInstall();
  bool CanInstall() const;

  void FocusHere();
  void DrawChoose(float scale);
  void DrawBrowse(float scale);
  void DrawInstalling(float scale);
  void DrawFailed(float scale);
  void DrawDone();
  void Dismiss();

  void OpenBrowser(std::filesystem::path dir);
  void ListBrowseDir();
  void ShowSystemPicker(bool folder);

  Options options_;
  std::function<void(const std::filesystem::path&)> on_installed_;
  std::function<void()> on_dismissed_;
  Screen screen_ = Screen::kChoose;
  GamepadNav nav_;
  std::vector<SDL_Gamepad*> gamepads_;
  bool focus_next_ = true;  // focus the first control of a new screen

  // What will be installed. `dlc_selected_` and `dlc_installed_` parallel `dlc_`.
  std::optional<installer::Source> game_;
  std::vector<installer::Source> dlc_;
  std::vector<bool> dlc_selected_;
  std::vector<bool> dlc_installed_;  // already in install_dir: unticked at first
  // Custom tracks install all or none (a save can hold hundreds).
  std::vector<installer::Source> tracks_;
  bool tracks_selected_ = true;
  std::vector<std::string> problems_;
  bool game_already_installed_ = false;

  // One worker at a time: a scan (more may be queued) or the install. Its
  // results are read after joining it, which orders them after its writes.
  std::deque<std::filesystem::path> pending_scans_;
  std::atomic<bool> worker_done_{true};
  std::optional<installer::ScanResult> scan_result_;
  std::filesystem::path scanning_;
  bool installing_ = false;
  bool install_ok_ = false;
  std::string install_error_;
  installer::Progress progress_;

  std::shared_ptr<Inbox> inbox_ = std::make_shared<Inbox>();

  std::filesystem::path browse_dir_;
  std::vector<BrowseEntry> browse_entries_;
  std::string browse_error_;
  char path_input_[1024] = {};

  // Last: destroyed (stopped and joined) before everything it writes to.
  std::jthread worker_;
};

}  // namespace trials_hd
