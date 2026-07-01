#pragma once

struct AppState;

class SpaceshipControlSystem {
public:
  void reset();
  void update(AppState &state, float dt);
};
