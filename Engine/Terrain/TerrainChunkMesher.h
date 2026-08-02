#pragma once
// TerrainChunkMesher.h — turns a sampled height/ground-field grid (from
// TerrainNoise.h's sampleHeightGrid) into an API-agnostic MeshData grid
// mesh: a regular indexed triangle grid, per-vertex normals from finite
// differences on the height samples themselves (no extra noise evaluation --
// computeHeight() stays the single height authority).
//
// R1 (MEADOW_TERRAIN_REVAMP_PLAN.md §4c/§5): the same finite-difference
// stencil already used for normals also derives slope and curvature
// (second difference) per vertex here -- cheaper than having TerrainNoise
// re-evaluate height neighbors independently, and keeps computeHeight() the
// sole height authority. rockMask blends that real slope with
// TerrainGroundFields::rockNoise (an independent noise hint, for organic
// outcrop edges rather than a razor-sharp slope threshold).

#include "MeshData.h"
#include "TerrainNoise.h"
#include "TerrainSettings.h"
#include "TerrainTypes.h"

#include <cstdint>
#include <vector>

// `heights`/`fields` are row-major, samplesPerEdge^2 samples (as produced by
// sampleHeightGrid at any LOD -- pass a smaller samplesPerEdge for a coarser
// chunk, e.g. ((baseResolution-1) >> lod) + 1). The mesh is centered at the
// local origin (chunk-local XZ in [-chunkWorldSize/2, chunkWorldSize/2]) so
// an entity transform's position places the chunk's center in world space.
// `uvTileWorldSize` is how many world units one repeat of the bindless
// detail texture covers. `skirtDepth` (world units) adds a downward-facing
// curtain of geometry around the outer ring, dropped by that amount, to hide
// seams between neighboring chunks at different LOD without needing
// cross-chunk vertex sharing; pass 0 to disable. `outcropThreshold` is
// TerrainSettings::outcropThreshold (slope 0..1 where rock starts breaking
// through turf) -- passed directly rather than the whole settings struct
// since it's the only field this function needs.
MeshData buildTerrainChunkMesh(const std::vector<float> &heights,
                               const std::vector<TerrainGroundFields> &fields,
                               uint32_t samplesPerEdge, float chunkWorldSize,
                               float outcropThreshold = 0.55f,
                               float uvTileWorldSize = 16.0f,
                               float skirtDepth = 4.0f);
