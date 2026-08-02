#pragma once
// MeshDecimate.h — polygon reduction for imported meshes.
//
// This exists for Option A (AI_ASSET_PIPELINE_PLAN.md §3.5): text-to-3D
// services and asset stores hand back 30k-200k triangle meshes, and this
// renderer builds ONE ACCELERATION STRUCTURE PER UNIQUE MESH. Dropping raw
// output into a scatter layer is a frame-time cliff, so an import that is not
// conditioned is not usable. Decimation is the conditioning step with teeth.
//
// Method: quadric error metric (Garland & Heckbert) edge collapse. Each vertex
// accumulates the squared-distance quadrics of its incident faces; the edge
// whose collapse adds least error goes first. Chosen over vertex clustering,
// which is far simpler but bulldozes silhouettes -- and silhouette is exactly
// what survives at the distances scattered props are seen from.
//
// Scope, stated plainly:
//   * operates per submesh, so material boundaries are never merged;
//   * welds coincident positions first (imported meshes split vertices at UV
//     seams, which would otherwise leave the topology full of holes and stop
//     collapses dead);
//   * attributes (uv, normal) come from the surviving vertex rather than being
//     interpolated -- adequate at these reduction ratios, and it cannot
//     produce the smeared UVs a naive blend does;
//   * refuses collapses that flip a face normal, which is what stops the
//     surface folding through itself.

#include "MeshData.h"

#include <cstddef>
#include <string>
#include <vector>

namespace gen {

struct DecimateResult {
  size_t trianglesBefore = 0;
  size_t trianglesAfter = 0;
  std::vector<std::string> notes;
  bool changed() const { return trianglesAfter != trianglesBefore; }
};

// Reduces `mesh` toward `targetTriangles` total, in place. Submeshes are
// reduced proportionally so a small detail submesh is not wiped out to save a
// large one. A target at or above the current count is a no-op.
DecimateResult decimateMesh(MeshData &mesh, size_t targetTriangles);

// Single-submesh entry point. `targetTriangles` is that submesh's budget.
size_t decimateSubmesh(MeshSubmeshData &submesh, size_t targetTriangles);

} // namespace gen
