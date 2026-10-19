// trials_hd - scriptable virtual gamepad for headless testing.

#include "virtual_pad.h"

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace trials_hd {

namespace {

constexpr std::pair<std::string_view, SDL_GamepadButton> kButtons[] = {
    {"a", SDL_GAMEPAD_BUTTON_SOUTH},
    {"b", SDL_GAMEPAD_BUTTON_EAST},
    {"x", SDL_GAMEPAD_BUTTON_WEST},
    {"y", SDL_GAMEPAD_BUTTON_NORTH},
    {"back", SDL_GAMEPAD_BUTTON_BACK},
    {"guide", SDL_GAMEPAD_BUTTON_GUIDE},
    {"start", SDL_GAMEPAD_BUTTON_START},
    {"ls", SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"rs", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"up", SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};

constexpr std::pair<std::string_view, SDL_GamepadAxis> kAxes[] = {
    {"lx", SDL_GAMEPAD_AXIS_LEFTX},
    {"ly", SDL_GAMEPAD_AXIS_LEFTY},
    {"rx", SDL_GAMEPAD_AXIS_RIGHTX},
    {"ry", SDL_GAMEPAD_AXIS_RIGHTY},
    {"lt", SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
    {"rt", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
};

void Apply(SDL_Joystick* joystick, const std::string& line,
           const std::function<void(const std::string&)>& on_capture) {
  std::istringstream in(line);
  std::string cmd, name;
  int value = 0;
  if (line.rfind("set ", 0) == 0) {
    // Diagnostics / benchmarks: change a cvar at runtime ("set vsync false").
    std::istringstream args(line.substr(4));
    std::string cvar, cvar_value;
    if (args >> cvar >> cvar_value) {
      rex::cvar::SetFlagByName(cvar, cvar_value);
      REXLOG_INFO("Virtual pad: {} = {}", cvar, rex::cvar::GetFlagByName(cvar));
    }
    return;
  }
  if (line.rfind("capture ", 0) == 0) {
    if (on_capture) {
      on_capture(line.substr(8));
    }
    return;
  }
  if (line == "quit") {
    REXLOG_INFO("Virtual pad: quit requested");
    std::exit(0);
  }
  if (!(in >> cmd >> name >> value)) {
    return;
  }
  if (cmd == "btn") {
    for (const auto& [n, button] : kButtons) {
      if (n == name) {
        SDL_SetJoystickVirtualButton(joystick, button, value != 0);
        SDL_UpdateJoysticks();
        SDL_Event ev{};
        ev.type = SDL_EVENT_USER;
        SDL_PushEvent(&ev);
        return;
      }
    }
  } else if (cmd == "axis") {
    for (const auto& [n, axis] : kAxes) {
      if (n == name) {
        SDL_SetJoystickVirtualAxis(joystick, axis, Sint16(SDL_clamp(value, -32768, 32767)));
        SDL_UpdateJoysticks();
        SDL_Event ev{};
        ev.type = SDL_EVENT_USER;
        SDL_PushEvent(&ev);
        return;
      }
    }
  }
  REXLOG_WARN("virtual pad: unknown command '{}'", line);
}

}  // namespace

void StartVirtualPadFromEnvironment(std::function<void(const std::string&)> on_capture) {
  const char* fifo = std::getenv("TRIALS_HD_VIRTUAL_PAD");
  static bool started = false;  // UI thread only
  if (!fifo || !*fifo || started) {
    return;
  }
  started = true;
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_InitSubSystem(SDL_INIT_GAMEPAD);

  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
  desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
  desc.button_mask = (1u << desc.nbuttons) - 1;
  desc.name = "trials_hd virtual pad";
  SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
  SDL_Joystick* joystick = id ? SDL_OpenJoystick(id) : nullptr;
  if (!joystick) {
    REXLOG_ERROR("virtual pad: could not attach: {}", SDL_GetError());
    return;
  }
  char guid_str[64];
  SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guid_str, sizeof(guid_str));
  std::string mapping = std::string(guid_str) +
      ",trials_hd virtual pad,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,"
      "leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,"
      "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
      "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,";
  bool mapped = SDL_SetGamepadMapping(id, mapping.c_str());
  REXLOG_INFO("virtual pad attached, mapped={}, reading commands from {}", mapped, fifo);

  std::thread([joystick, path = std::string(fifo), on_capture = std::move(on_capture)] {
    for (;;) {
      std::ifstream in(path);  // Blocks until a writer opens the FIFO.
      std::string line;
      while (std::getline(in, line)) {
        Apply(joystick, line, on_capture);
      }
    }
  }).detach();
}

}  // namespace trials_hd
