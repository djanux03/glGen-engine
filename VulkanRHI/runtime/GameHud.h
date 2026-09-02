#pragma once

#include <string>

// =============================================================================
// GameHud — gameplay HUD state, written by Lua, drawn by the editor overlay
// =============================================================================
// The engine had no way for a script to put anything on screen: Lua could
// spawn, move and raycast, but the only feedback channel was log.info() into
// the console. That is fine for a tech demo and useless for a game, where the
// player needs to see score and time without reading a log.
//
// This is deliberately a plain state block rather than an immediate-mode
// drawing API. Lua declares what the HUD should say each frame
// (game.hud{...}); VkEditor decides how it looks, using the editor's own
// theme and fonts. Scripts therefore cannot draw arbitrary shapes over the
// viewport -- which keeps the game's look consistent and keeps the renderer
// out of Lua's reach.
//
// `active` is set by the first game.hud{} call and cleared when play mode
// ends, so a scene with no game running shows the normal play-mode banner.
// =============================================================================

namespace GameHud {

struct State {
  bool active = false;

  std::string title;  // small label above the timer, e.g. "EMBER RUN"
  int score = 0;      // collected
  int total = 0;      // needed to win

  float time = 0.0f;    // seconds remaining
  float timeMax = 0.0f; // full bar value; 0 hides the timer entirely

  // Large centered message. Redeclared by the script every frame it should
  // be visible, so there is no expiry bookkeeping on this side.
  std::string banner;
  std::string bannerTone; // "good" | "bad" | "neutral"

  std::string hint; // small line along the bottom

  void reset() { *this = State{}; }
};

// Single shared block. The engine runs one world and one HUD; threading a
// pointer through Lua bindings, VkAppState and the editor would buy nothing.
inline State &get() {
  static State s;
  return s;
}

} // namespace GameHud
