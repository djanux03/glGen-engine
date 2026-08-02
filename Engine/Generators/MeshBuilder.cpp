#include "MeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace gen {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// An orthonormal frame whose +Y maps onto `axis` -- how a tube/branch built in
// local space gets oriented along an arbitrary direction.
void basisFromAxis(const glm::vec3 &axis, glm::vec3 &outU, glm::vec3 &outV) {
  const glm::vec3 up = (std::fabs(axis.y) > 0.99f) ? glm::vec3(1.0f, 0.0f, 0.0f)
                                                   : glm::vec3(0.0f, 1.0f, 0.0f);
  outU = glm::normalize(glm::cross(up, axis));
  outV = glm::cross(axis, outU);
}

// Quantized position key for welding/normal averaging.
struct PosKey {
  int32_t x, y, z;
  bool operator==(const PosKey &o) const {
    return x == o.x && y == o.y && z == o.z;
  }
};
struct PosKeyHash {
  size_t operator()(const PosKey &k) const {
    // FNV-ish mix; collisions only cost a bucket comparison.
    size_t h = 1469598103934665603ull;
    for (int32_t v : {k.x, k.y, k.z}) {
      h ^= static_cast<size_t>(v);
      h *= 1099511628211ull;
    }
    return h;
  }
};
PosKey keyOf(const glm::vec3 &p, float eps) {
  const float inv = 1.0f / std::max(eps, 1e-6f);
  return PosKey{static_cast<int32_t>(std::lround(p.x * inv)),
                static_cast<int32_t>(std::lround(p.y * inv)),
                static_cast<int32_t>(std::lround(p.z * inv))};
}

} // namespace

MeshBuilder::MeshBuilder(std::string sourcePath)
    : mSourcePath(std::move(sourcePath)) {}

void MeshBuilder::beginSubmesh(const std::string &name,
                               const MaterialAsset &material) {
  MeshSubmeshData sm;
  sm.objectName = name;
  sm.materialName = material.id.empty() ? name : material.id;
  sm.debugName = name;
  sm.material = material;
  mSubmeshes.push_back(std::move(sm));
}

void MeshBuilder::ensureSubmesh_() {
  if (mSubmeshes.empty())
    beginSubmesh("default", MaterialAsset{});
}

MeshSubmeshData &MeshBuilder::current() {
  ensureSubmesh_();
  return mSubmeshes.back();
}

void MeshBuilder::pushTransform(const glm::mat4 &m) {
  const glm::mat4 composed = mStack.back() * m;
  mStack.push_back(composed);
  mNormalStack.push_back(glm::transpose(glm::inverse(glm::mat3(composed))));
}

void MeshBuilder::popTransform() {
  // The identity at the bottom is the builder's own world frame; popping it
  // would leave the stack empty and every later transform() call undefined.
  if (mStack.size() > 1) {
    mStack.pop_back();
    mNormalStack.pop_back();
  }
}

uint32_t MeshBuilder::emit_(const glm::vec3 &pos, const glm::vec3 &normal,
                            const glm::vec2 &uv) {
  MeshSubmeshData &sm = current();
  MeshVertex v;
  v.pos = glm::vec3(mStack.back() * glm::vec4(pos, 1.0f));
  const glm::vec3 n = mNormalStack.back() * normal;
  const float len2 = glm::dot(n, n);
  v.normal = (len2 > 1e-12f) ? n * (1.0f / std::sqrt(len2)) : glm::vec3(0, 1, 0);
  v.uv = uv;
  sm.vertices.push_back(v);
  return static_cast<uint32_t>(sm.vertices.size() - 1);
}

void MeshBuilder::tri_(uint32_t a, uint32_t b, uint32_t c) {
  MeshSubmeshData &sm = current();
  sm.indices.push_back(a);
  sm.indices.push_back(b);
  sm.indices.push_back(c);
}

void MeshBuilder::addBox(const glm::vec3 &min, const glm::vec3 &max) {
  const glm::vec3 c[8] = {
      {min.x, min.y, min.z}, {max.x, min.y, min.z}, {max.x, max.y, min.z},
      {min.x, max.y, min.z}, {min.x, min.y, max.z}, {max.x, min.y, max.z},
      {max.x, max.y, max.z}, {min.x, max.y, max.z}};
  // corner indices per face (CCW from outside) + that face's normal
  const int faces[6][4] = {{4, 5, 6, 7}, {1, 0, 3, 2}, {5, 1, 2, 6},
                           {0, 4, 7, 3}, {3, 2, 6, 7}, {0, 1, 5, 4}};
  const glm::vec3 normals[6] = {{0, 0, 1},  {0, 0, -1}, {1, 0, 0},
                                {-1, 0, 0}, {0, 1, 0},  {0, -1, 0}};
  const glm::vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  for (int f = 0; f < 6; ++f) {
    const uint32_t base = emit_(c[faces[f][0]], normals[f], uv[0]);
    for (int k = 1; k < 4; ++k)
      emit_(c[faces[f][k]], normals[f], uv[k]);
    tri_(base, base + 1, base + 2);
    tri_(base, base + 2, base + 3);
  }
}

void MeshBuilder::addTaperedCylinder(const glm::vec3 &p0, float r0,
                                     const glm::vec3 &p1, float r1,
                                     int segments, bool capStart, bool capEnd,
                                     float vTile) {
  segments = std::max(3, segments);
  const glm::vec3 delta = p1 - p0;
  const float length = glm::length(delta);
  if (length < 1e-6f)
    return;
  const glm::vec3 axis = delta / length;
  glm::vec3 u, v;
  basisFromAxis(axis, u, v);

  // The side normal has to account for the taper, or a strongly tapered
  // segment (a branch tip, a cone) lights like a straight tube.
  const float slope = (r0 - r1) / length;
  const float nScale = 1.0f / std::sqrt(1.0f + slope * slope);

  const uint32_t base = static_cast<uint32_t>(current().vertices.size());
  for (int i = 0; i <= segments; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(segments);
    const float a = t * 2.0f * kPi;
    const glm::vec3 radial = u * std::cos(a) + v * std::sin(a);
    const glm::vec3 n = (radial + axis * slope) * nScale;
    emit_(p0 + radial * r0, n, glm::vec2(t, 0.0f));
    emit_(p1 + radial * r1, n, glm::vec2(t, vTile));
  }
  for (int i = 0; i < segments; ++i) {
    const uint32_t a = base + i * 2;
    tri_(a, a + 2, a + 3);
    tri_(a, a + 3, a + 1);
  }

  auto cap = [&](const glm::vec3 &center, float radius, const glm::vec3 &n,
                 bool flip) {
    if (radius < 1e-6f)
      return;
    const uint32_t c = emit_(center, n, glm::vec2(0.5f, 0.5f));
    for (int i = 0; i <= segments; ++i) {
      const float a = static_cast<float>(i) / segments * 2.0f * kPi;
      const glm::vec3 radial = u * std::cos(a) + v * std::sin(a);
      emit_(center + radial * radius, n,
            glm::vec2(0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a)));
    }
    for (int i = 0; i < segments; ++i) {
      if (flip)
        tri_(c, c + 1 + i + 1, c + 1 + i);
      else
        tri_(c, c + 1 + i, c + 1 + i + 1);
    }
  };
  if (capEnd)
    cap(p1, r1, axis, false);
  if (capStart)
    cap(p0, r0, -axis, true);
}

void MeshBuilder::addIcosphere(const glm::vec3 &center, float radius,
                               int subdivisions) {
  subdivisions = std::clamp(subdivisions, 0, 5);

  const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
  std::vector<glm::vec3> verts = {
      {-1, t, 0}, {1, t, 0},  {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
      {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1},  {-t, 0, -1}, {-t, 0, 1}};
  for (auto &p : verts)
    p = glm::normalize(p);
  std::vector<glm::ivec3> tris = {
      {0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
      {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
      {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
      {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};

  for (int s = 0; s < subdivisions; ++s) {
    std::vector<glm::ivec3> next;
    next.reserve(tris.size() * 4);
    std::unordered_map<uint64_t, int> midpoints;
    auto midpoint = [&](int a, int b) {
      const uint64_t key = (static_cast<uint64_t>(std::min(a, b)) << 32) |
                           static_cast<uint32_t>(std::max(a, b));
      auto it = midpoints.find(key);
      if (it != midpoints.end())
        return it->second;
      verts.push_back(glm::normalize((verts[a] + verts[b]) * 0.5f));
      const int idx = static_cast<int>(verts.size() - 1);
      midpoints[key] = idx;
      return idx;
    };
    for (const glm::ivec3 &tri : tris) {
      const int a = midpoint(tri.x, tri.y);
      const int b = midpoint(tri.y, tri.z);
      const int c = midpoint(tri.z, tri.x);
      next.push_back({tri.x, a, c});
      next.push_back({a, tri.y, b});
      next.push_back({c, b, tri.z});
      next.push_back({a, b, c});
    }
    tris.swap(next);
  }

  // Per-triangle emission with a dominant-axis planar UV. Sharing vertices
  // here would force a single global UV mapping, which on a sphere means
  // either a seam or pole stretching -- neither survives displacement well.
  for (const glm::ivec3 &tri : tris) {
    const glm::vec3 n0 = verts[tri.x], n1 = verts[tri.y], n2 = verts[tri.z];
    const glm::vec3 faceN = glm::normalize(n0 + n1 + n2);
    const glm::vec3 a = glm::abs(faceN);
    auto planarUV = [&](const glm::vec3 &p) -> glm::vec2 {
      if (a.x >= a.y && a.x >= a.z)
        return glm::vec2(p.z, p.y) * 0.5f + 0.5f;
      if (a.y >= a.z)
        return glm::vec2(p.x, p.z) * 0.5f + 0.5f;
      return glm::vec2(p.x, p.y) * 0.5f + 0.5f;
    };
    const uint32_t i0 = emit_(center + n0 * radius, n0, planarUV(n0));
    emit_(center + n1 * radius, n1, planarUV(n1));
    emit_(center + n2 * radius, n2, planarUV(n2));
    tri_(i0, i0 + 1, i0 + 2);
  }
}

void MeshBuilder::addQuad(const glm::vec3 &a, const glm::vec3 &b,
                          const glm::vec3 &c, const glm::vec3 &d) {
  glm::vec3 n = glm::cross(b - a, d - a);
  const float len2 = glm::dot(n, n);
  n = (len2 > 1e-12f) ? n * (1.0f / std::sqrt(len2)) : glm::vec3(0, 1, 0);
  const uint32_t base = emit_(a, n, glm::vec2(0, 0));
  emit_(b, n, glm::vec2(1, 0));
  emit_(c, n, glm::vec2(1, 1));
  emit_(d, n, glm::vec2(0, 1));
  tri_(base, base + 1, base + 2);
  tri_(base, base + 2, base + 3);
}

void MeshBuilder::addCard(float width, float height, float bendDeg,
                          int segments, float taper) {
  segments = std::max(1, segments);
  taper = std::clamp(taper, 0.0f, 1.0f);
  const float halfW = width * 0.5f;
  const uint32_t base = static_cast<uint32_t>(current().vertices.size());

  // Walk up the blade turning by a constant angle per row, so the bend is a
  // smooth arc rather than a kink at the tip.
  glm::vec3 p(0.0f);
  glm::vec3 dir(0.0f, 1.0f, 0.0f);
  const float segLen = height / static_cast<float>(segments);
  const float stepRad = glm::radians(bendDeg) / static_cast<float>(segments);

  std::vector<float> rowWidth(static_cast<size_t>(segments) + 1);
  for (int i = 0; i <= segments; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(segments);
    const float w = halfW * (1.0f - taper * t * t);
    rowWidth[i] = w;
    const glm::vec3 n = glm::normalize(glm::cross(glm::vec3(1, 0, 0), dir));
    emit_(p + glm::vec3(-w, 0, 0), n, glm::vec2(0.0f, t));
    emit_(p + glm::vec3(w, 0, 0), n, glm::vec2(1.0f, t));
    if (i < segments) {
      p += dir * segLen;
      const float ca = std::cos(stepRad), sa = std::sin(stepRad);
      dir = glm::normalize(glm::vec3(dir.x, dir.y * ca - dir.z * sa,
                                     dir.y * sa + dir.z * ca));
    }
  }
  // A fully tapered card ends in a point, so its top row's two vertices are
  // the same position and one of that band's two triangles has zero area.
  // Emitting it anyway wasted a triangle per blade -- 25% of a grass clump,
  // multiplied by every instance scattered across the terrain.
  const float degenerateEps = std::max(halfW, 1e-6f) * 1e-3f;
  for (int i = 0; i < segments; ++i) {
    const uint32_t a = base + i * 2;
    const bool bottomIsPoint = rowWidth[i] <= degenerateEps;
    const bool topIsPoint = rowWidth[i + 1] <= degenerateEps;
    if (bottomIsPoint && topIsPoint)
      continue; // a band between two points has no area at all
    if (topIsPoint) {
      tri_(a, a + 1, a + 3); // single triangle up to the tip
    } else if (bottomIsPoint) {
      tri_(a, a + 3, a + 2); // single triangle out of the base point
    } else {
      tri_(a, a + 1, a + 3);
      tri_(a, a + 3, a + 2);
    }
  }
}

void MeshBuilder::addLeafCard(float width, float height, float bendDeg) {
  const float halfW = width * 0.5f;
  const float bend = glm::radians(bendDeg);

  // Spine as a constant-curvature arc of arc-length `height` turning through
  // `bend` in total, so the leaf curves rather than kinking. The straight case
  // is the limit as bend -> 0, handled explicitly to avoid dividing by it.
  auto spine = [&](float t) -> glm::vec3 {
    if (std::fabs(bend) < 1e-4f)
      return glm::vec3(0.0f, height * t, 0.0f);
    const float r = height / bend;
    return glm::vec3(0.0f, r * std::sin(bend * t), r * (1.0f - std::cos(bend * t)));
  };
  auto normalAt = [&](float t) -> glm::vec3 {
    // Face normal follows the spine's tangent, rotated to point along +Z.
    const float a = bend * t;
    return glm::normalize(glm::vec3(0.0f, -std::sin(a), std::cos(a)));
  };

  // Outline: base point, two shoulder pairs, tip point.
  const float ts[4] = {0.0f, 0.3f, 0.7f, 1.0f};
  const float ws[4] = {0.0f, 1.0f, 0.78f, 0.0f};

  const glm::vec3 n1 = normalAt(ts[1]);
  const glm::vec3 n2 = normalAt(ts[2]);

  const uint32_t base = emit_(spine(ts[0]), normalAt(ts[0]), glm::vec2(0.5f, 0.0f));
  const uint32_t l1 = emit_(spine(ts[1]) - glm::vec3(halfW * ws[1], 0, 0), n1,
                            glm::vec2(0.5f - 0.5f * ws[1], ts[1]));
  const uint32_t r1 = emit_(spine(ts[1]) + glm::vec3(halfW * ws[1], 0, 0), n1,
                            glm::vec2(0.5f + 0.5f * ws[1], ts[1]));
  const uint32_t l2 = emit_(spine(ts[2]) - glm::vec3(halfW * ws[2], 0, 0), n2,
                            glm::vec2(0.5f - 0.5f * ws[2], ts[2]));
  const uint32_t r2 = emit_(spine(ts[2]) + glm::vec3(halfW * ws[2], 0, 0), n2,
                            glm::vec2(0.5f + 0.5f * ws[2], ts[2]));
  const uint32_t tip = emit_(spine(ts[3]), normalAt(ts[3]), glm::vec2(0.5f, 1.0f));

  tri_(base, r1, l1);
  tri_(l1, r1, r2);
  tri_(l1, r2, l2);
  tri_(l2, r2, tip);
}

void MeshBuilder::addRevolve(const std::vector<glm::vec2> &profile,
                             int segments) {
  if (profile.size() < 2)
    return;
  segments = std::max(3, segments);
  const uint32_t base = static_cast<uint32_t>(current().vertices.size());
  const int rows = static_cast<int>(profile.size());

  for (int r = 0; r < rows; ++r) {
    // Profile-space tangent, so the normal follows the silhouette's slope.
    const glm::vec2 prev = profile[std::max(0, r - 1)];
    const glm::vec2 next = profile[std::min(rows - 1, r + 1)];
    const glm::vec2 tangent = next - prev;
    const glm::vec2 nProfile = glm::normalize(glm::vec2(tangent.y, -tangent.x));
    const float v = static_cast<float>(r) / static_cast<float>(rows - 1);
    for (int i = 0; i <= segments; ++i) {
      const float u = static_cast<float>(i) / static_cast<float>(segments);
      const float a = u * 2.0f * kPi;
      const float ca = std::cos(a), sa = std::sin(a);
      emit_(glm::vec3(profile[r].x * ca, profile[r].y, profile[r].x * sa),
            glm::normalize(glm::vec3(nProfile.x * ca, nProfile.y, nProfile.x * sa)),
            glm::vec2(u, v));
    }
  }
  const int stride = segments + 1;
  for (int r = 0; r < rows - 1; ++r) {
    for (int i = 0; i < segments; ++i) {
      const uint32_t a = base + r * stride + i;
      const uint32_t b = a + stride;
      tri_(a, b, b + 1);
      tri_(a, b + 1, a + 1);
    }
  }
}

void MeshBuilder::displace(
    const std::function<glm::vec3(const glm::vec3 &, const glm::vec3 &)> &fn) {
  if (mSubmeshes.empty())
    return;
  for (auto &v : mSubmeshes.back().vertices)
    v.pos = fn(v.pos, v.normal);
}

void MeshBuilder::recomputeSmoothNormals(float weldEps) {
  if (mSubmeshes.empty())
    return;
  MeshSubmeshData &sm = mSubmeshes.back();

  std::vector<glm::vec3> accum(sm.vertices.size(), glm::vec3(0.0f));
  for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
    const uint32_t a = sm.indices[i], b = sm.indices[i + 1], c = sm.indices[i + 2];
    const glm::vec3 &p0 = sm.vertices[a].pos;
    const glm::vec3 &p1 = sm.vertices[b].pos;
    const glm::vec3 &p2 = sm.vertices[c].pos;
    // Un-normalized cross product: area-weights each face's contribution,
    // which is what keeps a displaced surface's shading stable when triangle
    // sizes vary wildly.
    const glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
    accum[a] += n;
    accum[b] += n;
    accum[c] += n;
  }

  // Merge contributions across coincident-but-separate vertices (UV seams),
  // or every seam shades as a hard crease.
  std::unordered_map<PosKey, glm::vec3, PosKeyHash> byPos;
  for (size_t i = 0; i < sm.vertices.size(); ++i)
    byPos[keyOf(sm.vertices[i].pos, weldEps)] += accum[i];

  for (size_t i = 0; i < sm.vertices.size(); ++i) {
    const glm::vec3 n = byPos[keyOf(sm.vertices[i].pos, weldEps)];
    const float len2 = glm::dot(n, n);
    if (len2 > 1e-12f)
      sm.vertices[i].normal = n * (1.0f / std::sqrt(len2));
  }
}

void MeshBuilder::recomputeFlatNormals() {
  if (mSubmeshes.empty())
    return;
  MeshSubmeshData &sm = mSubmeshes.back();

  std::vector<MeshVertex> verts;
  std::vector<uint32_t> indices;
  verts.reserve(sm.indices.size());
  indices.reserve(sm.indices.size());
  for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
    MeshVertex v0 = sm.vertices[sm.indices[i]];
    MeshVertex v1 = sm.vertices[sm.indices[i + 1]];
    MeshVertex v2 = sm.vertices[sm.indices[i + 2]];
    glm::vec3 n = glm::cross(v1.pos - v0.pos, v2.pos - v0.pos);
    const float len2 = glm::dot(n, n);
    n = (len2 > 1e-12f) ? n * (1.0f / std::sqrt(len2)) : glm::vec3(0, 1, 0);
    v0.normal = v1.normal = v2.normal = n;
    const uint32_t base = static_cast<uint32_t>(verts.size());
    verts.push_back(v0);
    verts.push_back(v1);
    verts.push_back(v2);
    indices.push_back(base);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
  }
  sm.vertices.swap(verts);
  sm.indices.swap(indices);
}

size_t MeshBuilder::triangleCount() const {
  size_t n = 0;
  for (const auto &sm : mSubmeshes)
    n += sm.indices.size() / 3;
  return n;
}

size_t MeshBuilder::vertexCount() const {
  size_t n = 0;
  for (const auto &sm : mSubmeshes)
    n += sm.vertices.size();
  return n;
}

std::unique_ptr<MeshData> MeshBuilder::build() {
  auto data = std::make_unique<MeshData>();
  data->sourcePath = mSourcePath;

  for (auto &sm : mSubmeshes) {
    if (sm.vertices.empty() || sm.indices.empty())
      continue;
    sm.aabbMin = glm::vec3(1e30f);
    sm.aabbMax = glm::vec3(-1e30f);
    for (const auto &v : sm.vertices) {
      sm.aabbMin = glm::min(sm.aabbMin, v.pos);
      sm.aabbMax = glm::max(sm.aabbMax, v.pos);
    }
    sm.hasBounds = true;

    MeshObjectBounds ob{sm.aabbMin, sm.aabbMax, true};
    data->objectBounds.emplace_back(sm.objectName, ob);
    data->submeshes.push_back(std::move(sm));
  }

  mSubmeshes.clear();
  mStack.assign(1, glm::mat4(1.0f));
  mNormalStack.assign(1, glm::mat3(1.0f));
  return data;
}

} // namespace gen
