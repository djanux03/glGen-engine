#include "MeshValidate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gen {
namespace {

bool finite(const glm::vec3 &v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

std::string fmt(float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3g", static_cast<double>(v));
  return buf;
}

// Iterates a submesh's triangles whether it is indexed or a raw triangle list
// -- both are legal MeshData shapes, and the parsers emit the latter.
template <typename Fn>
void forEachTriangle(const MeshSubmeshData &sm, Fn &&fn) {
  if (!sm.indices.empty()) {
    for (size_t i = 0; i + 2 < sm.indices.size(); i += 3)
      fn(sm.indices[i], sm.indices[i + 1], sm.indices[i + 2]);
  } else {
    for (uint32_t i = 0; i + 2 < sm.vertices.size(); i += 3)
      fn(i, i + 1, i + 2);
  }
}

} // namespace

bool meshHasNonFiniteData(const MeshData &mesh) {
  for (const auto &sm : mesh.submeshes)
    for (const auto &v : sm.vertices)
      if (!finite(v.pos) || !finite(v.normal))
        return true;
  return false;
}

std::vector<std::string> validateMesh(const MeshData &mesh,
                                      const MeshChecks &checks) {
  std::vector<std::string> issues;

  if (mesh.submeshes.empty()) {
    issues.push_back("mesh has no submeshes");
    return issues;
  }

  size_t triangles = 0;
  size_t degenerate = 0;
  size_t nonFinite = 0;
  size_t badIndices = 0;
  size_t zeroNormals = 0;

  for (const auto &sm : mesh.submeshes) {
    for (const auto &v : sm.vertices) {
      if (!finite(v.pos) || !finite(v.normal))
        ++nonFinite;
      else if (glm::dot(v.normal, v.normal) < 1e-12f)
        ++zeroNormals;
    }

    forEachTriangle(sm, [&](uint32_t a, uint32_t b, uint32_t c) {
      ++triangles;
      if (a >= sm.vertices.size() || b >= sm.vertices.size() ||
          c >= sm.vertices.size()) {
        ++badIndices;
        return;
      }
      const glm::vec3 &p0 = sm.vertices[a].pos;
      const glm::vec3 &p1 = sm.vertices[b].pos;
      const glm::vec3 &p2 = sm.vertices[c].pos;
      if (!finite(p0) || !finite(p1) || !finite(p2))
        return;
      // Twice the triangle's area; a zero here means the three corners are
      // collinear or coincident.
      if (glm::length(glm::cross(p1 - p0, p2 - p0)) < 1e-12f)
        ++degenerate;
    });
  }

  // --- fatal-ish -----------------------------------------------------------
  if (nonFinite > 0)
    issues.push_back(
        std::to_string(nonFinite) +
        " vertices have NaN or infinite positions/normals. This corrupts the "
        "ray-tracing acceleration structure build and will crash the renderer "
        "somewhere unrelated -- it is never intentional.");
  if (badIndices > 0)
    issues.push_back(std::to_string(badIndices) +
                     " triangles reference vertices that do not exist");

  // --- geometry quality ----------------------------------------------------
  if (triangles == 0) {
    issues.push_back("mesh has no triangles");
    return issues;
  }
  if (degenerate > 0) {
    const double pct = 100.0 * static_cast<double>(degenerate) /
                       static_cast<double>(triangles);
    if (pct > 1.0)
      issues.push_back(std::to_string(degenerate) + " of " +
                       std::to_string(triangles) + " triangles (" + fmt(pct) +
                       "%) have zero area; their normals are undefined");
  }
  if (zeroNormals > 0)
    issues.push_back(std::to_string(zeroNormals) +
                     " vertices have zero-length normals and will shade black");

  // NO non-manifold check. It was here and has been removed on evidence: a
  // tree is overlapping tapered cylinders plus loose foliage cards, and most
  // real game assets contain double-sided planes and intersecting parts, so it
  // fired on nearly everything legitimate. A warning that cries wolf does not
  // merely waste a line -- it teaches whoever reads these to skim past the
  // ones that matter, which costs more than the check was ever worth.

  if (checks.polyBudget > 0 && triangles > checks.polyBudget)
    issues.push_back(
        "generated " + std::to_string(triangles) + " triangles, over the " +
        std::to_string(checks.polyBudget) +
        " budget -- every unique mesh costs a ray-tracing acceleration "
        "structure, so lower the detail parameters if this is scattered");

  // --- placement -----------------------------------------------------------
  glm::vec3 mn, mx;
  if (!mesh.getGlobalBounds(mn, mx)) {
    issues.push_back("mesh reports no bounds; culling and placement will "
                     "misbehave");
    return issues;
  }
  const glm::vec3 extent = mx - mn;
  const float longest = std::max({extent.x, extent.y, extent.z});

  if (!finite(mn) || !finite(mx)) {
    issues.push_back("bounds are not finite");
  } else {
    if (longest > checks.maxExtentMeters)
      issues.push_back("mesh is " + fmt(longest) +
                       " m across, which is almost certainly a unit or scale "
                       "mistake rather than intent");
    if (longest < checks.minExtentMeters)
      issues.push_back("mesh is only " + fmt(longest) +
                       " m across; it will be invisible at any normal scale");

    if (checks.expectPivotAtBase) {
      // Tolerance scales with the asset: 1 cm matters for a pebble and not
      // for a tree.
      const float tolerance = std::max(0.02f, longest * 0.02f);
      if (std::fabs(mn.y) > tolerance)
        issues.push_back(
            "pivot is not at the base: the lowest point is y=" + fmt(mn.y) +
            " rather than 0, so every placement will float or sink by that "
            "much. Set recenter to 'baseY', or move the geometry.");
      // Threshold is half the object's own size, not a small tolerance: a
      // tree's crown is legitimately asymmetric over its trunk, and flagging
      // that taught the reader to ignore the warning. What this is actually
      // for is geometry authored tens of metres from its own origin, which is
      // many multiples of its size away, not a fraction of it.
      const float centerXZ =
          std::max(std::fabs((mn.x + mx.x) * 0.5f), std::fabs((mn.z + mx.z) * 0.5f));
      if (centerXZ > longest * 0.5f)
        issues.push_back(
            "geometry is centred " + fmt(centerXZ) +
            " m away from its own origin in XZ, which is more than its own "
            "size; it will rotate about a point outside itself and its "
            "culling bounds will be oversized");
    }
  }

  // --- materials -----------------------------------------------------------
  for (const auto &sm : mesh.submeshes) {
    const bool textured = !sm.material.texDiffusePath.empty();
    if (!textured || sm.vertices.size() < 3)
      continue;
    glm::vec2 uvMin(1e30f), uvMax(-1e30f);
    for (const auto &v : sm.vertices) {
      uvMin = glm::min(uvMin, v.uv);
      uvMax = glm::max(uvMax, v.uv);
    }
    const glm::vec2 uvExtent = uvMax - uvMin;
    if (uvExtent.x < 1e-5f && uvExtent.y < 1e-5f)
      issues.push_back("submesh '" + sm.objectName +
                       "' has a texture but no UV variation, so it samples a "
                       "single texel and renders flat");
  }

  return issues;
}

} // namespace gen
