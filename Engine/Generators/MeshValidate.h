#pragma once
// MeshValidate.h — the text half of the feedback loop.
//
// AI_ASSET_PIPELINE_PLAN.md Phase 6. A rendered image tells a model about
// proportion and silhouette; it says nothing about NaN vertices, a pivot in
// the wrong place, or UVs that were never assigned. Those arrive here, as
// sentences, alongside the picture -- and a sentence is something a model can
// act on directly, where a subtly wrong image just looks odd.
//
// Every check answers a question someone would otherwise have to debug:
//
//   non-finite positions   a NaN vertex corrupts the acceleration structure
//                          build and takes the renderer down, far from here
//   degenerate triangles   zero-area faces produce undefined normals and
//                          waste BLAS nodes
//   pivot placement        a prop whose base is not at y=0 floats above or
//                          sinks into terrain at every placement site
//   off-origin authoring   geometry far from its own origin breaks culling
//                          bounds and rotates around nothing
//   absurd extent          a scale mistake is much easier to read as a number
//                          than to notice in a render
//   missing UVs            a textured mesh with no UV variation samples one
//                          texel and renders flat, which reads as "the
//                          texture generator is broken" when it is not
//
// These are WARNINGS, not rejections. A deliberately flat card has no UVs
// worth speaking of and a deliberately huge asset is legitimate; only the
// caller knows. The exception is non-finite data, which cannot be intentional.

#include "MeshData.h"

#include <cstddef>
#include <string>
#include <vector>

namespace gen {

struct MeshChecks {
  // 0 = no budget. Exceeding it warns; every unique mesh costs an
  // acceleration structure (plan §3.3).
  size_t polyBudget = 0;
  // Expect the mesh to sit on y=0, as everything placed on terrain must.
  bool expectPivotAtBase = true;
  // Beyond this, a scale mistake is more likely than intent.
  float maxExtentMeters = 500.0f;
  // Below this, the mesh is probably empty or collapsed.
  float minExtentMeters = 0.001f;
};

// Returns human-readable findings, most serious first. Empty means clean.
std::vector<std::string> validateMesh(const MeshData &mesh,
                                      const MeshChecks &checks);

// True if `mesh` contains non-finite positions or normals -- the one condition
// that must never reach the renderer, since it corrupts the BLAS build and
// crashes somewhere unrelated.
bool meshHasNonFiniteData(const MeshData &mesh);

} // namespace gen
