#pragma once
// TerrainScatter.h — R4 of MEADOW_TERRAIN_REVAMP_PLAN.md §6b: manifest-
// driven placement, replacing the old per-biome-switch TerrainVegetation.h
// (deleted). Deterministic per chunk (same settings.seed + coord always
// scatters identically), invoked from TerrainChunkManager's worker job --
// reuses that job's TerrainNoiseSet (stateless per call, safe to share
// read-only across threads, same contract as TerrainNoise.h).

#include "ScatterManifest.h"
#include "TerrainNoise.h"
#include "TerrainSettings.h"
#include "TerrainTypes.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

// One layer's placement rules after TerrainSettings' global multipliers have
// been folded in. Exposed (rather than kept private to the .cpp) because the
// renderer glue needs the SAME effective values the placement used -- most
// importantly the effective draw distance and wind strength, which it must
// hand to the GPU batch. Deriving them twice from settings + manifest would
// be two places to keep in sync.
struct EffectiveScatterLayer {
  float density = 0.0f;
  float minSpacing = 0.0f;
  // Uniform scale (all three axes) -- the primary size control.
  float scaleMin = 1.0f;
  float scaleMax = 1.0f;
  // EXTRA vertical-only multiplier on top of the uniform scale. Near 1.0 for
  // everything by default: growing a plant is `scale`'s job, and stretching
  // one visibly distorts the asset.
  float heightScaleMin = 1.0f;
  float heightScaleMax = 1.0f;
  float groundOcclusion = 0.0f;
  float leanMaxDeg = 0.0f;
  float clearingChance = 0.0f;
  float standRadius = 25.0f;
  float maxDrawDistance = 1.0e6f;
  float densityFalloffStart = 1.0e6f;
  float windStrength = 0.0f;
  float windSpeed = 1.0f;
};

// Applies TerrainSettings' per-type multipliers to one authored layer.
// Pure function of its inputs; safe to call from worker threads.
EffectiveScatterLayer effectiveLayer(const ScatterLayer &layer,
                                     const TerrainSettings &settings);

// Expected clump coverage in [0,1], using the placement authority's biome, patch,
// track, slope and water gates. World-space evaluation keeps chunk/LOD borders
// continuous without querying neighbouring chunks or tracing individual blades.
float grassGroundOcclusionAt(const ScatterManifest &manifest,
    const TerrainNoiseSet &noise, const TerrainSettings &settings,
    glm::vec3 worldPos, glm::vec3 normal, glm::vec3 biomeWeights,
    float moisture, WaterCellCache *waterCache = nullptr);

// Replaces the old fixed-species VegetationInstance. `layerIndex` indexes
// into the SAME ScatterManifest passed to scatterLayers() -- the caller
// (VkTerrainSubsystem) resolves it back to a mesh handle.
struct ScatterInstance {
  glm::mat4 transform{1.0f};
  int layerIndex = -1;
  // Baked at placement time (R3's per-pixel biome lighting extended to
  // props, deferred from R3 to here): the exact biome weights at this
  // instance's position, threaded through to the GPU-instanced pipeline so
  // a tree standing in the forest is lit forest-dim even from a meadow
  // camera angle.
  glm::vec3 biomeWeights{1.0f, 0.0f, 0.0f};
  // Small per-instance hue/value multiply so one mesh doesn't read as
  // obviously copy-pasted across a whole grove (plan §6c.5).
  glm::vec3 colorJitter{1.0f, 1.0f, 1.0f};
  // Trees only (from ScatterLayer::interactive): eligible for promotion to
  // a physically-collidable capsule marker entity at runtime.
  bool interactive = false;
};

// Scatters every layer (R5: including Grass, which R4 skipped) across one
// chunk's world-space footprint ([chunkOrigin, chunkOrigin + chunkWorldSize)
// per axis).
//
// Candidate generation is a jittered grid sized so ~1 candidate falls per
// cell at the layer's effective density. Each candidate is then gated by,
// in order (cheapest test first, since grass runs this loop tens of
// thousands of times per chunk):
//   1. the layer's per-biome density multiplier, dotted with the ACTUAL
//      biome weights at that exact point (not the chunk average)
//   2. the grass patch mask (patchScale/patchThreshold) -- bare ground
//      between clumps
//   3. the tree-stand clearing mask (clustering.stands)
//   4. distance density falloff toward maxDrawDistance
//   5. slope / moisture rejection
//   6. rocks only: outcrop bias toward the ground field's rockNoise signal
//   7. minSpacing dart-throwing rejection against already-accepted
//      instances of the SAME layer in this chunk
//
// R5 replaces R4's "documented stand-in for Poisson-disk sampling" excuse
// with the real thing where it matters: step 7 is genuine min-distance
// rejection, backed by a per-layer spatial hash, so `minSpacing` is an
// honest guarantee (within a chunk) rather than an emergent property of
// grid cell size. The jittered grid survives as the candidate GENERATOR;
// only the acceptance rule changed.
//
// `chunkChebyshevDist` is the chunk's Chebyshev distance in chunks from the
// camera chunk at the time the build job was enqueued. Grass layers are
// skipped entirely beyond TerrainSettings::grassChunkRadius, and it drives
// the distance density falloff. It is a build-time snapshot: a chunk keeps
// whatever scatter it was built with until it is rebuilt or unloaded.
//
// Height/biome resolution goes through computeHeight()/sampleBiomeWeights()
// directly (the single height authority).
void scatterLayers(const ScatterManifest &manifest, const TerrainNoiseSet &noiseSet,
                   const TerrainSettings &settings, glm::vec2 chunkOrigin,
                   float chunkWorldSize, uint32_t chunkSeed,
                   int chunkChebyshevDist,
                   std::vector<ScatterInstance> &out);
