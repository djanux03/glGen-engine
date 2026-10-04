#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gameplay {
// The simulation owns ammunition and cadence; presentation never grants a shot.
// Seconds until the next shot carry across frames, including a partial interval,
// so automatic fire has the same rate at 30, 60 and 144 Hz.
struct RifleState {
  static constexpr int capacity = 30;
  static constexpr float shotInterval = .1f;
  bool enabled = true;
  bool automatic = true;
  int magazine = capacity;
  int reserve = 120;
  uint32_t shotsFired = 0;
  float cooldown = 0;
  float reloadRemaining = 0;
  float reloadDuration = 2.55f;
  bool emptyReload = false;
  bool triggerWasDown = false;

  bool reloading() const { return reloadRemaining > 0; }
  float reloadProgress() const {
    return reloading() ? 1 - reloadRemaining / reloadDuration : 0;
  }
  bool reload() {
    if (!enabled || reloading() || magazine >= capacity || reserve <= 0)
      return false;
    emptyReload = magazine == 0;
    reloadDuration = emptyReload ? 3.2f : 2.55f;
    reloadRemaining = reloadDuration;
    return true;
  }
  // Returns shot count for this tick. No catch-up after a pause, and no firing
  // during reload: a long tick crossing its end cannot fire in the past.
  int tick(float dt, bool trigger, bool active = true) {
    if (!active || !enabled || !std::isfinite(dt) || dt <= 0) {
      triggerWasDown = trigger;
      return 0;
    }
    dt = std::min(dt, .5f);
    if (reloading()) {
      reloadRemaining = std::max(0.f, reloadRemaining - dt);
      if (!reloading()) {
        const int transfer = std::min(capacity - magazine, reserve);
        magazine += transfer;
        reserve -= transfer;
        cooldown = shotInterval;
      }
      triggerWasDown = trigger;
      return 0;
    }
    if (trigger && !triggerWasDown && cooldown <= 0) cooldown = dt;
    cooldown -= dt;
    const bool fire = trigger && (automatic || !triggerWasDown);
    int count = 0;
    if (fire) {
      while (cooldown <= 1e-6f && magazine > 0) {
        --magazine;
        ++shotsFired;
        ++count;
        cooldown += shotInterval;
        if (!automatic) break;
      }
    }
    // An idle weapon is ready immediately, never banking a burst of shots.
    if (!trigger || magazine == 0) cooldown = std::max(0.f, cooldown);
    triggerWasDown = trigger;
    return count;
  }
};
} // namespace gameplay
