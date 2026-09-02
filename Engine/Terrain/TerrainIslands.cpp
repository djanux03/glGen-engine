#include "TerrainIslands.h"

#include "TerrainNoise.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// One cell per ~28 m at the default 4 km radius. Fine enough that a labelled
// island follows its own coastline, coarse enough that the flood fill is a
// few hundred thousand cells and runs in milliseconds.
constexpr uint32_t kLabelResolution = 320;

uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

float hashUnit(uint32_t h) {
  return static_cast<float>(h & 0x00FFFFFFu) / 16777215.0f;
}

} // namespace

const char *islandArchetypeName(IslandArchetype a) {
  switch (a) {
  case IslandArchetype::Meadows: return "Meadows";
  case IslandArchetype::Forest: return "Forest";
  case IslandArchetype::Highland: return "Highland";
  case IslandArchetype::Marsh: return "Marsh";
  default: return "?";
  }
}

void IslandMap::build(const TerrainNoiseSet &noiseSet, const TerrainSettings &settings) {
  mResolution = 0;
  mWorldSize = 0.0f;
  mLabels.clear();
  mIslands.clear();
  mNeutral = IslandInfo{};

  if (!settings.worldBounded)
    return; // endless world: no map to label, and callers get the neutral info

  mResolution = kLabelResolution;
  mWorldSize = settings.worldRadius * 2.2f;
  mOrigin = glm::vec2(-mWorldSize * 0.5f);
  const float cell = mWorldSize / static_cast<float>(mResolution - 1);

  // 1) Rasterise land/sea from the continent mask.
  std::vector<uint8_t> land(static_cast<size_t>(mResolution) * mResolution, 0);
  for (uint32_t j = 0; j < mResolution; ++j) {
    for (uint32_t i = 0; i < mResolution; ++i) {
      const glm::vec2 xz = mOrigin + glm::vec2(static_cast<float>(i),
                                               static_cast<float>(j)) * cell;
      // 0.5 IS the shoreline by construction (see continentAt), so this is the
      // land test with no extra threshold to keep in sync.
      land[static_cast<size_t>(j) * mResolution + i] =
          continentAt(noiseSet, settings, xz) > 0.5f ? 1u : 0u;
    }
  }

  // 2) Connected components, 4-connected. An explicit stack rather than
  // recursion: a big landmass is tens of thousands of cells deep and would
  // overflow the stack on the recursive version.
  mLabels.assign(static_cast<size_t>(mResolution) * mResolution, kNoIsland);
  std::vector<uint32_t> stack;
  for (uint32_t j = 0; j < mResolution; ++j) {
    for (uint32_t i = 0; i < mResolution; ++i) {
      const uint32_t start = j * mResolution + i;
      if (!land[start] || mLabels[start] != kNoIsland)
        continue;
      if (mIslands.size() >= kNoIsland - 1)
        break; // more landmasses than the label type can name; stop cleanly
      const uint16_t id = static_cast<uint16_t>(mIslands.size());

      IslandInfo info;
      info.id = id;
      glm::dvec2 sum(0.0);
      uint32_t count = 0;

      stack.clear();
      stack.push_back(start);
      mLabels[start] = id;
      while (!stack.empty()) {
        const uint32_t c = stack.back();
        stack.pop_back();
        const uint32_t cy = c / mResolution, cx = c % mResolution;
        sum += glm::dvec2(cx, cy);
        ++count;
        const int dx[4] = {1, -1, 0, 0};
        const int dy[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; ++k) {
          const int nx = static_cast<int>(cx) + dx[k];
          const int ny = static_cast<int>(cy) + dy[k];
          if (nx < 0 || ny < 0 || nx >= static_cast<int>(mResolution) ||
              ny >= static_cast<int>(mResolution))
            continue;
          const uint32_t nc = static_cast<uint32_t>(ny) * mResolution +
                              static_cast<uint32_t>(nx);
          if (land[nc] && mLabels[nc] == kNoIsland) {
            mLabels[nc] = id;
            stack.push_back(nc);
          }
        }
      }

      info.areaSqM = static_cast<float>(count) * cell * cell;
      const glm::dvec2 mean = sum / static_cast<double>(std::max(count, 1u));
      info.centroid = mOrigin + glm::vec2(static_cast<float>(mean.x),
                                          static_cast<float>(mean.y)) * cell;
      info.distanceFromSpawn = glm::length(info.centroid);
      mIslands.push_back(info);
    }
  }

  // 3) Radius, from the labelled cells back to each centroid.
  std::vector<float> maxDistSq(mIslands.size(), 0.0f);
  for (uint32_t j = 0; j < mResolution; ++j) {
    for (uint32_t i = 0; i < mResolution; ++i) {
      const uint16_t id = mLabels[static_cast<size_t>(j) * mResolution + i];
      if (id == kNoIsland)
        continue;
      const glm::vec2 xz = mOrigin + glm::vec2(static_cast<float>(i),
                                               static_cast<float>(j)) * cell;
      const glm::vec2 d = xz - mIslands[id].centroid;
      maxDistSq[id] = std::max(maxDistSq[id], glm::dot(d, d));
    }
  }
  for (size_t k = 0; k < mIslands.size(); ++k) {
    // Floored at half a cell: a one-cell islet has its centroid ON its only
    // cell, so the measured max distance is exactly 0 -- which would report a
    // landmass with real area as having no extent, and divide badly in any
    // consumer that normalises by radius.
    mIslands[k].radius = std::max(std::sqrt(maxDistSq[k]), cell * 0.5f);
  }

  // 4) Archetypes. Graded OUTWARD from spawn, which is the whole point: it is
  // what turns distance into progression instead of into more of the same. The
  // island containing the origin is pinned to Meadows so a new world always
  // starts somewhere gentle, and everything else picks from a tier-weighted
  // draw so the grading is a tendency rather than a set of concentric rings.
  const float far = std::max(settings.worldRadius - settings.worldEdgeFalloff, 1.0f);
  const uint16_t spawnId = idAt(glm::vec2(0.0f));
  for (IslandInfo &info : mIslands) {
    const float t = std::clamp(info.distanceFromSpawn / far, 0.0f, 1.0f);
    const uint32_t h = hash32(static_cast<uint32_t>(info.id) * 2654435761u ^
                              (settings.seed * 40503u));
    const float roll = hashUnit(h);

    if (info.id == spawnId) {
      info.archetype = IslandArchetype::Meadows;
    } else if (roll < 0.18f) {
      // A minority of islands ignore the gradient entirely. Without this the
      // map is readable but predictable, and there is no reason to sail to a
      // near island once you know what tier it is.
      info.archetype = static_cast<IslandArchetype>(
          hash32(h) % static_cast<uint32_t>(IslandArchetype::Count));
    } else if (t < 0.33f) {
      info.archetype = roll < 0.62f ? IslandArchetype::Meadows
                                    : IslandArchetype::Forest;
    } else if (t < 0.66f) {
      info.archetype = roll < 0.55f ? IslandArchetype::Forest
                                    : IslandArchetype::Marsh;
    } else {
      info.archetype = roll < 0.70f ? IslandArchetype::Highland
                                    : IslandArchetype::Forest;
    }

    const float vary = hashUnit(hash32(h ^ 0x9E37u));
    switch (info.archetype) {
    case IslandArchetype::Meadows:
      info.reliefScale = 0.55f + vary * 0.25f;
      info.treelineOffset = 12.0f;
      info.weightBias = glm::vec3(1.0f, 0.15f, 0.0f);
      break;
    case IslandArchetype::Forest:
      info.reliefScale = 0.85f + vary * 0.35f;
      info.treelineOffset = 4.0f;
      info.weightBias = glm::vec3(0.2f, 1.0f, 0.1f);
      break;
    case IslandArchetype::Highland:
      info.reliefScale = 1.7f + vary * 0.9f;
      info.treelineOffset = -14.0f;
      info.weightBias = glm::vec3(0.05f, 0.25f, 1.0f);
      break;
    case IslandArchetype::Marsh:
      info.reliefScale = 0.30f + vary * 0.15f;
      info.treelineOffset = 6.0f;
      info.weightBias = glm::vec3(0.5f, 0.8f, 0.0f);
      break;
    default:
      break;
    }
  }
}

uint16_t IslandMap::idAt(glm::vec2 worldXZ) const {
  if (mResolution == 0)
    return kNoIsland;
  const glm::vec2 rel = (worldXZ - mOrigin) / mWorldSize;
  if (rel.x < 0.0f || rel.y < 0.0f || rel.x > 1.0f || rel.y > 1.0f)
    return kNoIsland;
  const uint32_t i = std::min(mResolution - 1,
                              static_cast<uint32_t>(rel.x * (mResolution - 1) + 0.5f));
  const uint32_t j = std::min(mResolution - 1,
                              static_cast<uint32_t>(rel.y * (mResolution - 1) + 0.5f));
  return mLabels[static_cast<size_t>(j) * mResolution + i];
}

const IslandInfo &IslandMap::infoAt(glm::vec2 worldXZ) const {
  const uint16_t id = idAt(worldXZ);
  if (id == kNoIsland || id >= mIslands.size())
    return mNeutral;
  return mIslands[id];
}
