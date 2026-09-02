#include "TerrainScatter.h"
#include "TerrainWater.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <unordered_map>

namespace {

// Small, fast, deterministic hash-based PRNG -- avoids constructing a
// std::mt19937 (heavier state) per grid cell when a chunk may have on the
// order of a hundred candidates.
uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

float rand01(uint32_t &state) {
  state = hash32(state);
  return static_cast<float>(state & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
}

uint32_t cellSeed(uint32_t layerSeed, int i, int j) {
  return hash32(layerSeed ^ hash32(static_cast<uint32_t>(i) * 92821u +
                                   static_cast<uint32_t>(j) * 68917u));
}

// A jittered regular grid over the chunk footprint, one candidate per cell.
// Documented stand-in for true Poisson-disk sampling (plan §6b): a real
// blue-noise sampler (e.g. Bridson's algorithm) gives more uniform spacing
// with no residual grid-alignment tell, but needs a spatial acceleration
// structure and a min-distance rejection pool per chunk to do properly.
// The jittered grid, combined with the biome/slope/moisture/clustering
// rejection gates below (which already break up any regularity by
// accepting only a fraction of candidates), reads as organically irregular
// in practice at the density this system runs; revisit if scatter density
// increases enough for the grid tell to become visible.
template <typename PlaceFn>
void jitteredGrid(glm::vec2 chunkOrigin, float chunkWorldSize, float cellSize,
                  uint32_t layerSeed, PlaceFn place) {
  const int cells = std::max(1, static_cast<int>(chunkWorldSize / cellSize));
  for (int j = 0; j < cells; ++j) {
    for (int i = 0; i < cells; ++i) {
      uint32_t state = cellSeed(layerSeed, i, j);
      const float jx = rand01(state) - 0.5f;
      const float jz = rand01(state) - 0.5f;
      const float worldX =
          chunkOrigin.x + (static_cast<float>(i) + 0.5f + jx * 0.8f) * cellSize;
      const float worldZ =
          chunkOrigin.y + (static_cast<float>(j) + 0.5f + jz * 0.8f) * cellSize;
      place(worldX, worldZ, state);
    }
  }
}

// Min-distance ("dart throwing") acceptance for one layer within one chunk.
// Accepted positions are bucketed into a hash grid whose cell size IS the
// spacing radius, so a candidate only has to test the 3x3 neighborhood --
// any instance closer than `spacing` is necessarily in one of those 9 cells.
// That keeps acceptance O(1) amortized even at grass candidate counts, where
// an O(n) scan over accepted instances would be quadratic and unusable.
class SpacingGrid {
public:
  explicit SpacingGrid(float spacing)
      : mSpacing(spacing), mSpacingSq(spacing * spacing),
        mInvCell(spacing > 1e-4f ? 1.0f / spacing : 0.0f) {}

  bool accepts(float x, float z) const {
    const int cx = cellOf(x), cz = cellOf(z);
    for (int dz = -1; dz <= 1; ++dz) {
      for (int dx = -1; dx <= 1; ++dx) {
        auto it = mCells.find(key(cx + dx, cz + dz));
        if (it == mCells.end())
          continue;
        for (const glm::vec2 &p : it->second) {
          const float ddx = p.x - x, ddz = p.y - z;
          if (ddx * ddx + ddz * ddz < mSpacingSq)
            return false;
        }
      }
    }
    return true;
  }

  void insert(float x, float z) {
    mCells[key(cellOf(x), cellOf(z))].emplace_back(x, z);
  }

private:
  static int64_t key(int cx, int cz) {
    return (static_cast<int64_t>(cx) << 32) ^ static_cast<uint32_t>(cz);
  }
  int cellOf(float v) const {
    return static_cast<int>(std::floor(v * mInvCell));
  }

  float mSpacing, mSpacingSq, mInvCell;
  std::unordered_map<int64_t, std::vector<glm::vec2>> mCells;
};

glm::vec3 normalAt(const TerrainNoiseSet &noiseSet, const TerrainSettings &settings,
                   glm::vec2 pos, float eps = 0.75f) {
  auto heightAt = [&](glm::vec2 p) {
    TerrainMacroSample s = sampleMacro(noiseSet, p, settings);
    return computeHeight(s, noiseSet, p, settings, nullptr);
  };
  const float hL = heightAt(pos - glm::vec2(eps, 0.0f));
  const float hR = heightAt(pos + glm::vec2(eps, 0.0f));
  const float hD = heightAt(pos - glm::vec2(0.0f, eps));
  const float hU = heightAt(pos + glm::vec2(0.0f, eps));
  return glm::normalize(glm::vec3(-(hR - hL), 2.0f * eps, -(hU - hD)));
}

} // namespace

EffectiveScatterLayer effectiveLayer(const ScatterLayer &layer,
                                     const TerrainSettings &settings) {
  EffectiveScatterLayer e;
  const bool isTree = layer.type == ScatterLayerType::Tree;
  const bool isGrass = layer.type == ScatterLayerType::Grass;

  float densityMult = 1.0f;
  if (isTree)
    densityMult = settings.treeDensityMultiplier;
  else if (isGrass)
    densityMult = settings.grassDensityMultiplier;
  else
    densityMult = settings.rockDensityMultiplier;
  e.density = std::max(layer.density * std::max(densityMult, 0.0f), 0.0f);

  e.minSpacing = layer.minSpacing;
  if (isTree)
    e.minSpacing *= std::max(settings.treeSpacingMultiplier, 0.0f);

  // Uniform size. This is the dial that makes a plant BIGGER: scaling all
  // three axes keeps the asset's authored proportions, so a large tree reads
  // as a large tree rather than as a stretched mesh.
  float sizeMult = 1.0f;
  if (isTree)
    sizeMult = std::max(settings.treeSizeMultiplier, 0.01f);
  else if (isGrass)
    sizeMult = std::max(settings.grassSizeMultiplier, 0.01f);
  e.scaleMin = layer.scaleMin * sizeMult;
  e.scaleMax = std::max(layer.scaleMax * sizeMult, e.scaleMin);

  // Vertical-only stretch, applied on TOP of the uniform size above. Kept
  // separate so that growing a plant and distorting it are different
  // operations; the defaults leave this near 1.0.
  //
  // The variance re-spread happens after the multiplier and is centred on
  // the range's own midpoint, so it never shifts the AVERAGE -- it only
  // changes how ragged the canopy line is. That is what lets the size and
  // variance dials be used independently.
  float hMin = layer.heightScaleMin, hMax = layer.heightScaleMax;
  if (isTree) {
    const float m = std::max(settings.treeHeightMultiplier, 0.01f);
    hMin *= m;
    hMax *= m;
    const float mid = (hMin + hMax) * 0.5f;
    const float half = (hMax - hMin) * 0.5f * std::max(settings.treeHeightVariance, 0.0f);
    hMin = mid - half;
    hMax = mid + half;
  } else if (isGrass) {
    const float m = std::max(settings.grassHeightMultiplier, 0.01f);
    hMin *= m;
    hMax *= m;
  }
  e.heightScaleMin = std::max(hMin, 0.01f);
  e.heightScaleMax = std::max(hMax, e.heightScaleMin);

  e.groundOcclusion = glm::clamp(
      layer.groundOcclusion *
          (isGrass ? std::max(settings.grassOcclusionStrength, 0.0f) : 1.0f),
      0.0f, 1.0f);

  e.leanMaxDeg = layer.leanMaxDeg + (isTree ? std::max(settings.treeLeanExtraDeg, 0.0f) : 0.0f);

  e.clearingChance =
      glm::clamp(layer.clustering.clearingChance * std::max(settings.forestPatchiness, 0.0f),
                 0.0f, 0.98f);
  e.standRadius =
      std::max(layer.clustering.standRadius * std::max(settings.standRadiusMultiplier, 0.01f),
               1.0f);

  e.maxDrawDistance = layer.maxDrawDistance;
  e.densityFalloffStart = layer.densityFalloffStart;
  if (isGrass) {
    const float m = std::max(settings.grassDrawDistanceMultiplier, 0.01f);
    // Guard the "unlimited" sentinel against being scaled into a finite
    // number (or an overflow) by the multiplier.
    if (e.maxDrawDistance < 1.0e6f)
      e.maxDrawDistance *= m;
    if (e.densityFalloffStart < 1.0e6f)
      e.densityFalloffStart *= m;
  }

  e.windStrength =
      layer.wind ? layer.windStrength * std::max(settings.windStrength, 0.0f) : 0.0f;
  e.windSpeed = layer.windSpeed * std::max(settings.windSpeed, 0.0f);
  return e;
}

void scatterLayers(const ScatterManifest &manifest, const TerrainNoiseSet &noiseSet,
                   const TerrainSettings &settings, glm::vec2 chunkOrigin,
                   float chunkWorldSize, uint32_t chunkSeed,
                   int chunkChebyshevDist,
                   std::vector<ScatterInstance> &out) {
  out.clear();
  if (!settings.spawnVegetation)
    return;

  auto makeInstance = [&](const ScatterLayer &layer, const EffectiveScatterLayer &eff,
                          size_t layerIndex, glm::vec3 p, const glm::vec3 &normal,
                          uint32_t &state, const BiomeWeights &w) {
    const float yaw = layer.randomYaw ? rand01(state) * glm::two_pi<float>() : 0.0f;
    const float scale = glm::mix(eff.scaleMin, eff.scaleMax, rand01(state));
    // Vertical-only stretch on top of the uniform scale: this is what makes
    // a tree read as TALL rather than merely large (see
    // ScatterLayer::heightScaleMin).
    const float heightScale =
        glm::mix(eff.heightScaleMin, eff.heightScaleMax, rand01(state));
    p.y -= layer.sinkIntoGround;

    glm::mat4 t = glm::translate(glm::mat4(1.0f), p);
    if (layer.alignToNormal > 0.001f) {
      const glm::vec3 up(0.0f, 1.0f, 0.0f);
      const glm::vec3 axis = glm::cross(up, normal);
      const float axisLen = glm::length(axis);
      if (axisLen > 1e-4f) {
        const float angle =
            std::acos(glm::clamp(glm::dot(up, normal), -1.0f, 1.0f)) * layer.alignToNormal;
        t = glm::rotate(t, angle, axis / axisLen);
      }
    }
    // Random lean, about a random HORIZONTAL axis. Applied after the
    // normal alignment so it perturbs whatever pose that produced, rather
    // than competing with it.
    if (eff.leanMaxDeg > 0.01f) {
      const float leanAngle =
          glm::radians(eff.leanMaxDeg) * (rand01(state) * 2.0f - 1.0f);
      const float leanDir = rand01(state) * glm::two_pi<float>();
      t = glm::rotate(t, leanAngle,
                      glm::vec3(std::cos(leanDir), 0.0f, std::sin(leanDir)));
    }
    t = glm::rotate(t, yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    t = glm::scale(t, glm::vec3(scale, scale * heightScale, scale));

    ScatterInstance inst;
    inst.transform = t;
    inst.layerIndex = static_cast<int>(layerIndex);
    inst.biomeWeights = glm::vec3(w.meadow, w.forest, w.mountain);
    // Small hue rotation + value jitter -- enough to break up "obviously
    // the same mesh" without looking like discolored trees -- then the
    // layer's authored tint. They share one GPU varying, so folding the
    // tint in here costs nothing on the instance data.
    const float hueJit = (rand01(state) - 0.5f) * 0.12f;
    const float valJit = 0.9f + rand01(state) * 0.2f;
    inst.colorJitter =
        glm::vec3(1.0f + hueJit, 1.0f, 1.0f - hueJit) * valJit * layer.tint;
    inst.interactive = layer.interactive;
    out.push_back(inst);
  };

  // Approximate camera distance to this chunk, for the density falloff. The
  // build-job snapshot is per chunk, so per-instance precision would be
  // false precision: every candidate in the chunk shares this value.
  const float chunkCameraDist =
      static_cast<float>(std::max(chunkChebyshevDist, 0)) * chunkWorldSize;

  // Shared by every layer's waterline test below. Chunk-local and on the
  // stack: scatter runs on worker threads, one chunk each, so this must not
  // be shared between them.
  WaterCellCache waterCache;

  for (size_t li = 0; li < manifest.layers.size(); ++li) {
    const ScatterLayer &layer = manifest.layers[li];
    const bool isGrass = layer.type == ScatterLayerType::Grass;

    if (isGrass) {
      if (!settings.spawnGrass)
        continue;
      // Hard radius cap -- see TerrainSettings::grassChunkRadius for why
      // this is separate from (and stricter than) the draw distance.
      if (chunkChebyshevDist > settings.grassChunkRadius)
        continue;
    }

    const EffectiveScatterLayer eff = effectiveLayer(layer, settings);
    if (eff.density <= 1e-6f)
      continue;

    // Distance density falloff: thins toward zero between
    // densityFalloffStart and maxDrawDistance, so a layer fades out instead
    // of vanishing at a hard ring. Applied as a per-candidate acceptance
    // probability rather than by changing the grid spacing, so instances
    // that survive stay in the same positions they would have had at full
    // density -- a chunk rebuilt at a different distance doesn't shuffle
    // its whole scatter, it just drops some of it.
    float distanceKeep = 1.0f;
    if (eff.maxDrawDistance < 1.0e6f && eff.densityFalloffStart < eff.maxDrawDistance) {
      if (chunkCameraDist >= eff.maxDrawDistance)
        continue; // nothing here could ever be drawn
      if (chunkCameraDist > eff.densityFalloffStart) {
        const float t = (chunkCameraDist - eff.densityFalloffStart) /
                        (eff.maxDrawDistance - eff.densityFalloffStart);
        distanceKeep = glm::clamp(1.0f - t, 0.0f, 1.0f);
      }
    }

    const float cellSize =
        glm::clamp(1.0f / std::sqrt(eff.density), 0.35f, chunkWorldSize);
    const uint32_t layerSeed =
        hash32(chunkSeed ^ hash32(static_cast<uint32_t>(li) * 2654435761u));

    // Only allocated when the layer actually asks for spacing -- trees do,
    // grass generally does not (its density is the spacing).
    SpacingGrid spacing(std::max(eff.minSpacing, 1e-3f));
    const bool useSpacing = eff.minSpacing > 1e-3f;

    // Decorrelate each layer's patch mask by offsetting its sample position
    // by the layer index, so grass_meadow and grass_tuft don't share
    // patches (which would defeat having two layers at all).
    const float patchOffset = static_cast<float>(li) * 137.0f;

    jitteredGrid(chunkOrigin, chunkWorldSize, cellSize, layerSeed,
                [&](float x, float z, uint32_t state) {
                  const glm::vec2 pos(x, z);
                  TerrainMacroSample sample = sampleMacro(noiseSet, pos, settings);
                  const float h = computeHeight(sample, noiseSet, pos, settings, nullptr);
                  const BiomeWeights w = sampleBiomeWeights(sample, settings, h);

                  // Biome gate: this layer's per-biome multipliers dotted
                  // with the ACTUAL weights at this point (not the chunk
                  // average) -- the one mechanism that gives meadow/forest/
                  // mountain their distinct vegetation identity.
                  const float biomeMult = w.meadow * layer.biomeMeadow +
                                          w.forest * layer.biomeForest +
                                          w.mountain * layer.biomeMountain;
                  if (rand01(state) > glm::clamp(biomeMult, 0.0f, 1.0f))
                    return;

                  if (distanceKeep < 1.0f && rand01(state) > distanceKeep)
                    return;

                  // Grass patch mask: mid-frequency, so patches are metres
                  // across rather than the tens of metres of a tree stand.
                  // Rejecting here (before the height/normal derivative
                  // sampling below, which costs 4 extra height evaluations)
                  // is why this test sits so early.
                  if (layer.patchScale > 1e-4f) {
                    const float f = 0.06f * layer.patchScale;
                    const float patch =
                        noiseSet.detail.noise(x * f + patchOffset, z * f - patchOffset) *
                            0.5f + 0.5f;
                    if (patch < layer.patchThreshold)
                      return;
                  }

                  if (layer.clustering.stands) {
                    // Independent low-frequency mask (own frequency/offset,
                    // decorrelated from the forest-coverage field itself) so
                    // clearings don't just retrace the meadow/forest border.
                    // Frequency tracks standRadius so the "grove size" dial
                    // actually changes grove size rather than just how often
                    // a candidate is rejected.
                    const float standFreq = 0.375f / eff.standRadius;
                    const float standNoise =
                        noiseSet.detail.noise(x * standFreq + 500.0f,
                                              z * standFreq - 500.0f) *
                            0.5f + 0.5f;
                    if (standNoise < eff.clearingChance)
                      return;
                  }

                  const glm::vec3 normal = normalAt(noiseSet, settings, pos);
                  const float slope = 1.0f - glm::clamp(normal.y, 0.0f, 1.0f);
                  if (slope > layer.slopeMax)
                    return;
                  if (sample.moisture < layer.moistureMin)
                    return;

                  // Waterline. Nothing is planted below the local water
                  // surface -- an ocean's or a lake's, whichever covers this
                  // point -- plus a dry margin, so a shoreline reads as a
                  // beach the vegetation stops at rather than as a forest
                  // standing in the shallows. This runs per candidate rather
                  // than per cell because the surface differs between lakes.
                  {
                    const float surface = waterSurfaceAt(noiseSet, settings, pos,
                                                         /*edits=*/nullptr, &waterCache);
                    if (surface > kNoWater * 0.5f &&
                        h < surface + settings.shoreScatterMargin)
                      return;
                  }

                  if (layer.type == ScatterLayerType::Rock && layer.clustering.outcrops) {
                    // Bias acceptance toward the ground field's rockNoise
                    // signal so boulders cluster where the terrain is
                    // already reading "rocky" rather than sprinkling evenly.
                    const float outcropSignal =
                        glm::clamp(sample.outcropField * 0.5f + 0.5f, 0.0f, 1.0f);
                    if (rand01(state) > glm::mix(0.12f, 1.0f, outcropSignal))
                      return;
                  }

                  // Last gate: everything above is a probability, this one
                  // is a hard geometric guarantee, so it runs after the
                  // cheap rejections have already thinned the candidates.
                  if (useSpacing) {
                    if (!spacing.accepts(x, z))
                      return;
                    spacing.insert(x, z);
                  }

                  makeInstance(layer, eff, li, glm::vec3(x, h, z), normal, state, w);

                  // Rock outcrop clustering: anchor + 2-5 satellites in a
                  // ring, each independently height/biome-sampled -- natural
                  // boulder fields rather than a single stone per candidate.
                  if (layer.type == ScatterLayerType::Rock && layer.clustering.outcrops) {
                    const int satellites = 2 + static_cast<int>(rand01(state) * 4.0f);
                    for (int s = 0; s < satellites; ++s) {
                      const float ring = 1.2f + rand01(state) * 2.8f;
                      const float ang = rand01(state) * glm::two_pi<float>();
                      const glm::vec2 satPos(x + std::cos(ang) * ring,
                                             z + std::sin(ang) * ring);
                      TerrainMacroSample satSample = sampleMacro(noiseSet, satPos, settings);
                      const float satH =
                          computeHeight(satSample, noiseSet, satPos, settings, nullptr);
                      const BiomeWeights satW = sampleBiomeWeights(satSample, settings, satH);
                      const glm::vec3 satNormal = normalAt(noiseSet, settings, satPos);
                      makeInstance(layer, eff, li, glm::vec3(satPos.x, satH, satPos.y),
                                  satNormal, state, satW);
                    }
                  }
                });
  }
}
