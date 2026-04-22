#pragma once

struct AppState;

class PlayerInteractionSystem {
public:
  void reset();
  void update(AppState &state, float dt);

private:
  float mCraterCooldown = 0.0f;
};
