#include "TerrainChunkMesher.h"

#include <algorithm>
#include <cstdint>
#include <glm/glm.hpp>

MeshData buildTerrainChunkMesh(const std::vector<float> &heights,
                               const std::vector<TerrainGroundFields> &fields,
                               uint32_t samplesPerEdge, float chunkWorldSize,
                               float outcropThreshold, float uvTileWorldSize,
                               float skirtDepth) {
  MeshData data;
  data.sourcePath = "terrain:generated";
  if (samplesPerEdge < 2 ||
      heights.size() != static_cast<size_t>(samplesPerEdge) * samplesPerEdge)
    return data;

  MeshSubmeshData sub;
  sub.objectName = "TerrainChunk";
  sub.materialName = "TerrainChunk";
  sub.debugName = "TerrainChunk";

  const float denom = static_cast<float>(samplesPerEdge - 1);
  const float spacing = chunkWorldSize / denom;

  auto heightAt = [&](int i, int j) -> float {
    i = std::clamp(i, 0, static_cast<int>(samplesPerEdge) - 1);
    j = std::clamp(j, 0, static_cast<int>(samplesPerEdge) - 1);
    return heights[static_cast<size_t>(j) * samplesPerEdge +
                   static_cast<size_t>(i)];
  };
  auto fieldAt = [&](int i, int j) -> TerrainGroundFields {
    if (fields.empty())
      return TerrainGroundFields{};
    i = std::clamp(i, 0, static_cast<int>(samplesPerEdge) - 1);
    j = std::clamp(j, 0, static_cast<int>(samplesPerEdge) - 1);
    return fields[static_cast<size_t>(j) * samplesPerEdge +
                 static_cast<size_t>(i)];
  };

  sub.vertices.reserve(static_cast<size_t>(samplesPerEdge) * samplesPerEdge);
  glm::vec3 aabbMin(1e30f), aabbMax(-1e30f);

  for (uint32_t j = 0; j < samplesPerEdge; ++j) {
    for (uint32_t i = 0; i < samplesPerEdge; ++i) {
      const float worldX =
          (static_cast<float>(i) / denom - 0.5f) * chunkWorldSize;
      const float worldZ =
          (static_cast<float>(j) / denom - 0.5f) * chunkWorldSize;
      const float h = heightAt(static_cast<int>(i), static_cast<int>(j));

      const float hL = heightAt(static_cast<int>(i) - 1, static_cast<int>(j));
      const float hR = heightAt(static_cast<int>(i) + 1, static_cast<int>(j));
      const float hD = heightAt(static_cast<int>(i), static_cast<int>(j) - 1);
      const float hU = heightAt(static_cast<int>(i), static_cast<int>(j) + 1);
      const glm::vec3 normal = glm::normalize(
          glm::vec3(-(hR - hL), 2.0f * spacing, -(hU - hD)));

      // Slope (0 flat .. 1 vertical) from the same normal, and curvature
      // (discrete Laplacian: neighbor average minus center, positive =
      // concave/valley, negative = convex/ridge) from the same 4-tap
      // stencil -- no extra height evaluations beyond what the normal
      // already needed.
      const float slope = 1.0f - std::clamp(normal.y, 0.0f, 1.0f);
      const float laplacian =
          ((hL + hR + hD + hU) * 0.25f - h) / std::max(0.0001f, spacing * spacing);
      // Found via a probe while verifying R3 (MEADOW_TERRAIN_REVAMP_PLAN.md):
      // the previous *8*0.5 (=4x) multiplier saturated curvature01 to a hard
      // 0 or 1 almost everywhere -- typical micro-relief height deltas
      // between 0.5m-spaced neighbors already exceed the ~0.03m window that
      // multiplier left before clamping. A gentler multiplier keeps it
      // actually varying near 0.5 (flat) for typical terrain, only
      // saturating at genuinely sharp features.
      const float curvature01 = std::clamp(laplacian * 1.0f + 0.5f, 0.0f, 1.0f);

      const TerrainGroundFields field = fieldAt(static_cast<int>(i), static_cast<int>(j));
      const float slopeMask =
          glm::smoothstep(outcropThreshold - 0.15f, outcropThreshold + 0.15f, slope);
      const float rockMask =
          std::clamp(slopeMask * glm::mix(0.7f, 1.15f, field.rockNoise), 0.0f, 1.0f);

      MeshVertex v;
      v.pos = glm::vec3(worldX, h, worldZ);
      v.uv = glm::vec2(worldX / uvTileWorldSize, field.moisture);
      v.normal = normal;
      v.terrainParams = glm::vec4(curvature01, rockMask, field.wForest, field.wMountain);
      sub.vertices.push_back(v);

      aabbMin = glm::min(aabbMin, v.pos);
      aabbMax = glm::max(aabbMax, v.pos);
    }
  }

  sub.indices.reserve(static_cast<size_t>(samplesPerEdge - 1) *
                      (samplesPerEdge - 1) * 6);
  for (uint32_t j = 0; j + 1 < samplesPerEdge; ++j) {
    for (uint32_t i = 0; i + 1 < samplesPerEdge; ++i) {
      const uint32_t v00 = j * samplesPerEdge + i;
      const uint32_t v10 = j * samplesPerEdge + i + 1;
      const uint32_t v01 = (j + 1) * samplesPerEdge + i;
      const uint32_t v11 = (j + 1) * samplesPerEdge + i + 1;
      sub.indices.push_back(v00);
      sub.indices.push_back(v01);
      sub.indices.push_back(v10);
      sub.indices.push_back(v10);
      sub.indices.push_back(v01);
      sub.indices.push_back(v11);
    }
  }

  if (skirtDepth > 0.0f) {
    // Walk the border ring clockwise (viewed from above) so that, combined
    // with the (a0,a1,b0)/(a1,b1,b0) winding below, the skirt quads' normals
    // (edge1 x edge2, matching the top face's v00,v01,v10 convention above)
    // point outward, away from the chunk's interior.
    const uint32_t n = samplesPerEdge;
    std::vector<uint32_t> ring;
    ring.reserve(static_cast<size_t>(n) * 4 - 4);
    for (uint32_t i = 0; i < n; ++i)                 // top:    j=0,   i: 0..n-1
      ring.push_back(0 * n + i);
    for (uint32_t j = 1; j < n; ++j)                 // right:  i=n-1, j: 1..n-1
      ring.push_back(j * n + (n - 1));
    for (int32_t i = static_cast<int32_t>(n) - 2; i >= 0; --i) // bottom: j=n-1, i: n-2..0
      ring.push_back((n - 1) * n + static_cast<uint32_t>(i));
    for (int32_t j = static_cast<int32_t>(n) - 2; j >= 1; --j) // left: i=0, j: n-2..1
      ring.push_back(static_cast<uint32_t>(j) * n + 0);

    const size_t ringLen = ring.size();
    std::vector<uint32_t> skirtIdx(ringLen);
    for (size_t k = 0; k < ringLen; ++k) {
      MeshVertex v = sub.vertices[ring[k]];
      v.pos.y -= skirtDepth;
      skirtIdx[k] = static_cast<uint32_t>(sub.vertices.size());
      sub.vertices.push_back(v);
      aabbMin = glm::min(aabbMin, v.pos);
      aabbMax = glm::max(aabbMax, v.pos);
    }
    for (size_t k = 0; k < ringLen; ++k) {
      const size_t k1 = (k + 1) % ringLen;
      const uint32_t a0 = ring[k], a1 = ring[k1];
      const uint32_t b0 = skirtIdx[k], b1 = skirtIdx[k1];
      sub.indices.push_back(a0);
      sub.indices.push_back(a1);
      sub.indices.push_back(b0);
      sub.indices.push_back(a1);
      sub.indices.push_back(b1);
      sub.indices.push_back(b0);
    }
  }

  sub.aabbMin = aabbMin;
  sub.aabbMax = aabbMax;
  sub.hasBounds = true;

  data.submeshes.push_back(std::move(sub));
  return data;
}
