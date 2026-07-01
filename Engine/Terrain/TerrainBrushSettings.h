#pragma once

struct TerrainBrushSettings {
  bool enabled = false;
  int mode = 0;   // 0=Raise,1=Lower,2=Add,3=Remove
  int target = 0; // 0=Tree,1=Rock,2=Grass
  float radius = 6.0f;
  float strength = 2.0f; // units per second
  int scatterCount = 6;
};
