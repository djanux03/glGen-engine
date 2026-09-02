#pragma once
// EditorGrid.h — the ground grid and origin axes.
//
// Builds a line list for VulkanRenderer::setDebugLines(). Depth-tested by that
// pipeline, so the grid is correctly hidden behind terrain and geometry rather
// than floating over them.
//
// The grid FOLLOWS THE CAMERA in whole-cell steps rather than being a fixed
// slab centred on the origin. A fixed grid is either small enough to run out
// from under you or large enough to cost tens of thousands of lines; snapping
// a modest patch to the camera gives an apparently infinite grid for a fixed
// budget, and snapping in whole cells is what stops the lines visibly crawling
// as you move.

#include "VulkanRenderer.h"

#include <cmath>
#include <glm/glm.hpp>
#include <vector>

struct EditorGridSettings {
  bool enabled = true;
  // Metres per cell. 1 m matches the engine's working units.
  float cellSize = 1.0f;
  // Every Nth line is drawn brighter, so distance and scale stay readable
  // without counting cells.
  int majorEvery = 10;
  // Half-width in cells. The patch must be comfortably LARGER than the fade
  // distance, or the distance fade never reaches zero and the grid ends on a
  // visible straight edge instead of dissolving.
  int halfExtent = 100;
  // Height of the grid plane. Terrain rarely sits at y=0, so this is
  // adjustable rather than assumed.
  float height = 0.0f;

  glm::vec4 minorColor{0.32f, 0.34f, 0.38f, 0.28f};
  glm::vec4 majorColor{0.44f, 0.47f, 0.52f, 0.5f};
  // The X and Z axes through the origin, coloured like every other editor's
  // so orientation is readable at a glance.
  bool showAxes = true;
  glm::vec4 axisXColor{0.80f, 0.25f, 0.30f, 0.9f};
  glm::vec4 axisZColor{0.25f, 0.45f, 0.85f, 0.9f};

  // Distance at which lines have faded out entirely. Kept below the patch's
  // own half-width so the fade, not the patch boundary, is what you see.
  float fadeDistance() const {
    return static_cast<float>(halfExtent) * cellSize * 0.85f;
  }
};

// Appends the grid (and axes) to `out`. `cameraPos` is used only to snap the
// patch; the caller sets the fade distance on the renderer.
inline void buildEditorGrid(const EditorGridSettings &settings,
                            const glm::vec3 &cameraPos,
                            std::vector<vkrhi::VulkanRenderer::DebugLineVertex> &out) {
  if (!settings.enabled || settings.cellSize <= 0.0f || settings.halfExtent <= 0)
    return;

  const float cell = settings.cellSize;
  const int half = settings.halfExtent;
  const float y = settings.height;

  // Snap the patch origin to the cell lattice so lines stay put in world
  // space as the camera moves.
  const float originX = std::floor(cameraPos.x / cell) * cell;
  const float originZ = std::floor(cameraPos.z / cell) * cell;
  const float span = static_cast<float>(half) * cell;

  auto push = [&out](glm::vec3 a, glm::vec3 b, glm::vec4 color) {
    out.push_back({a, color});
    out.push_back({b, color});
  };

  for (int i = -half; i <= half; ++i) {
    const float offset = static_cast<float>(i) * cell;
    const float x = originX + offset;
    const float z = originZ + offset;

    // "Major" is decided in ABSOLUTE cell coordinates, not relative to the
    // camera -- otherwise the bright lines would slide around as you move.
    const int cellX = static_cast<int>(std::lround(x / cell));
    const int cellZ = static_cast<int>(std::lround(z / cell));
    const bool majorX = settings.majorEvery > 0 && (cellX % settings.majorEvery) == 0;
    const bool majorZ = settings.majorEvery > 0 && (cellZ % settings.majorEvery) == 0;

    // The origin axes are drawn separately and brighter; skip the grid line
    // that would z-fight with them.
    if (!(settings.showAxes && cellZ == 0))
      push({originX - span, y, z}, {originX + span, y, z},
           majorZ ? settings.majorColor : settings.minorColor);
    if (!(settings.showAxes && cellX == 0))
      push({x, y, originZ - span}, {x, y, originZ + span},
           majorX ? settings.majorColor : settings.minorColor);
  }

  if (settings.showAxes) {
    push({originX - span, y, 0.0f}, {originX + span, y, 0.0f},
         settings.axisXColor);
    push({0.0f, y, originZ - span}, {0.0f, y, originZ + span},
         settings.axisZColor);
  }
}
