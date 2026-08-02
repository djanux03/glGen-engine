#include "MeshDecimate.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <unordered_map>

namespace gen {
namespace {

// A symmetric 4x4 quadric, stored as its 10 unique coefficients. Adding the
// quadrics of the faces around a vertex gives the squared distance from any
// point to all of those planes -- the error a collapse to that point costs.
struct Quadric {
  double m[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  void addPlane(double a, double b, double c, double d) {
    m[0] += a * a; m[1] += a * b; m[2] += a * c; m[3] += a * d;
    m[4] += b * b; m[5] += b * c; m[6] += b * d;
    m[7] += c * c; m[8] += c * d;
    m[9] += d * d;
  }
  Quadric &operator+=(const Quadric &o) {
    for (int i = 0; i < 10; ++i)
      m[i] += o.m[i];
    return *this;
  }
  // v^T Q v for v = (x,y,z,1).
  double evaluate(const glm::vec3 &v) const {
    const double x = v.x, y = v.y, z = v.z;
    return m[0] * x * x + 2 * m[1] * x * y + 2 * m[2] * x * z + 2 * m[3] * x +
           m[4] * y * y + 2 * m[5] * y * z + 2 * m[6] * y +
           m[7] * z * z + 2 * m[8] * z +
           m[9];
  }
};

struct PosKey {
  int32_t x, y, z;
  bool operator<(const PosKey &o) const {
    if (x != o.x) return x < o.x;
    if (y != o.y) return y < o.y;
    return z < o.z;
  }
};

PosKey keyOf(const glm::vec3 &p, float eps) {
  const float inv = 1.0f / eps;
  return PosKey{static_cast<int32_t>(std::lround(p.x * inv)),
                static_cast<int32_t>(std::lround(p.y * inv)),
                static_cast<int32_t>(std::lround(p.z * inv))};
}

struct Collapse {
  uint32_t a = 0, b = 0;
  double cost = 0.0;
  glm::vec3 target{0.0f};
  uint64_t version = 0; // stale entries are skipped rather than removed
  bool operator>(const Collapse &o) const { return cost > o.cost; }
};

} // namespace

size_t decimateSubmesh(MeshSubmeshData &submesh, size_t targetTriangles) {
  // Non-indexed submeshes are a legal MeshData shape; give them indices so the
  // topology below has something to work with.
  if (submesh.indices.empty()) {
    submesh.indices.resize(submesh.vertices.size());
    for (uint32_t i = 0; i < submesh.vertices.size(); ++i)
      submesh.indices[i] = i;
  }

  const size_t triangleCount = submesh.indices.size() / 3;
  if (triangleCount <= targetTriangles || targetTriangles == 0)
    return triangleCount;

  // --- weld coincident positions -----------------------------------------
  // Imported meshes split vertices at UV/normal seams. Without welding, the
  // mesh is topologically shredded: almost no edge has two adjacent faces, so
  // collapses either do nothing or tear holes.
  glm::vec3 mn(1e30f), mx(-1e30f);
  for (const auto &v : submesh.vertices) {
    mn = glm::min(mn, v.pos);
    mx = glm::max(mx, v.pos);
  }
  const glm::vec3 extent = mx - mn;
  const float scale = std::max({extent.x, extent.y, extent.z, 1e-4f});
  const float weldEps = scale * 1e-5f;

  std::map<PosKey, uint32_t> byPos;
  std::vector<uint32_t> toWelded(submesh.vertices.size());
  std::vector<uint32_t> weldedToOriginal; // representative original vertex
  std::vector<glm::vec3> positions;
  for (uint32_t i = 0; i < submesh.vertices.size(); ++i) {
    const PosKey key = keyOf(submesh.vertices[i].pos, weldEps);
    const auto it = byPos.find(key);
    if (it != byPos.end()) {
      toWelded[i] = it->second;
    } else {
      const uint32_t idx = static_cast<uint32_t>(positions.size());
      byPos[key] = idx;
      toWelded[i] = idx;
      positions.push_back(submesh.vertices[i].pos);
      weldedToOriginal.push_back(i);
    }
  }

  struct Face {
    uint32_t v[3];
    bool alive = true;
  };
  std::vector<Face> faces;
  faces.reserve(triangleCount);
  for (size_t i = 0; i + 2 < submesh.indices.size(); i += 3) {
    Face f;
    f.v[0] = toWelded[submesh.indices[i]];
    f.v[1] = toWelded[submesh.indices[i + 1]];
    f.v[2] = toWelded[submesh.indices[i + 2]];
    // Degenerate after welding: two corners were the same point already.
    if (f.v[0] == f.v[1] || f.v[1] == f.v[2] || f.v[0] == f.v[2])
      continue;
    faces.push_back(f);
  }
  if (faces.size() <= targetTriangles)
    return faces.size();

  // --- per-vertex quadrics ------------------------------------------------
  std::vector<Quadric> quadrics(positions.size());
  std::vector<std::vector<uint32_t>> vertexFaces(positions.size());
  for (uint32_t fi = 0; fi < faces.size(); ++fi) {
    const Face &f = faces[fi];
    const glm::vec3 &p0 = positions[f.v[0]];
    const glm::vec3 &p1 = positions[f.v[1]];
    const glm::vec3 &p2 = positions[f.v[2]];
    glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
    const float len = glm::length(n);
    if (len < 1e-12f)
      continue;
    n /= len;
    const double d = -static_cast<double>(glm::dot(n, p0));
    Quadric q;
    q.addPlane(n.x, n.y, n.z, d);
    for (int k = 0; k < 3; ++k) {
      quadrics[f.v[k]] += q;
      vertexFaces[f.v[k]].push_back(fi);
    }
  }

  // --- candidate edges ----------------------------------------------------
  std::vector<uint64_t> edgeKeys;
  auto edgeKey = [](uint32_t a, uint32_t b) {
    return (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
  };
  {
    std::vector<uint64_t> all;
    all.reserve(faces.size() * 3);
    for (const Face &f : faces) {
      all.push_back(edgeKey(f.v[0], f.v[1]));
      all.push_back(edgeKey(f.v[1], f.v[2]));
      all.push_back(edgeKey(f.v[2], f.v[0]));
    }
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    edgeKeys.swap(all);
  }

  std::vector<uint64_t> vertexVersion(positions.size(), 0);
  std::vector<uint32_t> collapsedTo(positions.size());
  for (uint32_t i = 0; i < positions.size(); ++i)
    collapsedTo[i] = i;
  // Union-find with path halving. Iterative, so no std::function needed.
  auto resolve = [&](uint32_t v) -> uint32_t {
    while (collapsedTo[v] != v)
      v = collapsedTo[v] = collapsedTo[collapsedTo[v]];
    return v;
  };

  // Candidate positions are the two endpoints and the midpoint. The optimal
  // point needs a 3x3 solve that is singular on flat regions -- exactly where
  // the midpoint is already correct -- so this trades a little quality for
  // never having to special-case a degenerate matrix.
  auto bestCollapse = [&](uint32_t a, uint32_t b) {
    Quadric q = quadrics[a];
    q += quadrics[b];
    const glm::vec3 candidates[3] = {positions[a], positions[b],
                                     (positions[a] + positions[b]) * 0.5f};
    Collapse c;
    c.a = a;
    c.b = b;
    c.cost = 1e300;
    for (const glm::vec3 &p : candidates) {
      const double error = q.evaluate(p);
      if (error < c.cost) {
        c.cost = error;
        c.target = p;
      }
    }
    c.version = vertexVersion[a] + vertexVersion[b];
    return c;
  };

  std::priority_queue<Collapse, std::vector<Collapse>, std::greater<Collapse>> queue;
  for (uint64_t key : edgeKeys)
    queue.push(bestCollapse(static_cast<uint32_t>(key >> 32),
                            static_cast<uint32_t>(key & 0xFFFFFFFF)));

  // --- collapse ----------------------------------------------------------
  size_t liveFaces = faces.size();
  while (liveFaces > targetTriangles && !queue.empty()) {
    Collapse c = queue.top();
    queue.pop();

    const uint32_t a = resolve(c.a);
    const uint32_t b = resolve(c.b);
    if (a == b)
      continue;
    // Stale: one endpoint moved since this entry was queued, so its cost is
    // no longer right. Re-cost and requeue rather than trusting it.
    if (c.version != vertexVersion[a] + vertexVersion[b]) {
      queue.push(bestCollapse(a, b));
      continue;
    }

    // Reject collapses that would flip a face. Without this the surface folds
    // through itself and the silhouette develops spikes.
    bool flips = false;
    for (uint32_t fi : vertexFaces[a]) {
      Face &f = faces[fi];
      if (!f.alive)
        continue;
      uint32_t v[3] = {resolve(f.v[0]), resolve(f.v[1]), resolve(f.v[2])};
      if ((v[0] == a || v[0] == b) + (v[1] == a || v[1] == b) +
              (v[2] == a || v[2] == b) >= 2)
        continue; // this face disappears in the collapse
      glm::vec3 before[3] = {positions[v[0]], positions[v[1]], positions[v[2]]};
      glm::vec3 after[3];
      for (int k = 0; k < 3; ++k)
        after[k] = (v[k] == a || v[k] == b) ? c.target : before[k];
      const glm::vec3 n0 = glm::cross(before[1] - before[0], before[2] - before[0]);
      const glm::vec3 n1 = glm::cross(after[1] - after[0], after[2] - after[0]);
      if (glm::dot(n0, n1) <= 0.0f) {
        flips = true;
        break;
      }
    }
    if (flips)
      continue;

    // Perform it: b folds into a, which moves to the target point.
    positions[a] = c.target;
    quadrics[a] += quadrics[b];
    collapsedTo[b] = a;
    ++vertexVersion[a];
    ++vertexVersion[b];
    vertexFaces[a].insert(vertexFaces[a].end(), vertexFaces[b].begin(),
                          vertexFaces[b].end());
    vertexFaces[b].clear();

    for (uint32_t fi : vertexFaces[a]) {
      Face &f = faces[fi];
      if (!f.alive)
        continue;
      const uint32_t v0 = resolve(f.v[0]), v1 = resolve(f.v[1]),
                     v2 = resolve(f.v[2]);
      if (v0 == v1 || v1 == v2 || v0 == v2) {
        f.alive = false;
        --liveFaces;
      }
    }

    // Requeue the neighbourhood at its new cost.
    for (uint32_t fi : vertexFaces[a]) {
      const Face &f = faces[fi];
      if (!f.alive)
        continue;
      for (int k = 0; k < 3; ++k) {
        const uint32_t other = resolve(f.v[k]);
        if (other != a)
          queue.push(bestCollapse(a, other));
      }
    }
  }

  // --- rebuild ------------------------------------------------------------
  // Surviving welded vertices keep their representative original vertex's
  // attributes, with the position updated to wherever the collapses left it.
  std::vector<uint32_t> weldedToNew(positions.size(), UINT32_MAX);
  std::vector<MeshVertex> newVertices;
  std::vector<uint32_t> newIndices;
  newIndices.reserve(liveFaces * 3);

  for (const Face &f : faces) {
    if (!f.alive)
      continue;
    uint32_t tri[3];
    bool ok = true;
    for (int k = 0; k < 3; ++k) {
      const uint32_t w = resolve(f.v[k]);
      if (weldedToNew[w] == UINT32_MAX) {
        MeshVertex v = submesh.vertices[weldedToOriginal[w]];
        v.pos = positions[w];
        weldedToNew[w] = static_cast<uint32_t>(newVertices.size());
        newVertices.push_back(v);
      }
      tri[k] = weldedToNew[w];
    }
    if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2])
      ok = false;
    if (!ok)
      continue;
    newIndices.push_back(tri[0]);
    newIndices.push_back(tri[1]);
    newIndices.push_back(tri[2]);
  }

  if (newIndices.empty())
    return triangleCount; // refuse to hand back an empty mesh

  submesh.vertices.swap(newVertices);
  submesh.indices.swap(newIndices);

  submesh.aabbMin = glm::vec3(1e30f);
  submesh.aabbMax = glm::vec3(-1e30f);
  for (const auto &v : submesh.vertices) {
    submesh.aabbMin = glm::min(submesh.aabbMin, v.pos);
    submesh.aabbMax = glm::max(submesh.aabbMax, v.pos);
  }
  submesh.hasBounds = true;
  return submesh.indices.size() / 3;
}

DecimateResult decimateMesh(MeshData &mesh, size_t targetTriangles) {
  DecimateResult result;
  for (const auto &sm : mesh.submeshes)
    result.trianglesBefore +=
        (sm.indices.empty() ? sm.vertices.size() : sm.indices.size()) / 3;
  result.trianglesAfter = result.trianglesBefore;

  if (targetTriangles == 0 || result.trianglesBefore <= targetTriangles)
    return result;

  // Proportional budgets: a submesh holding 5% of the geometry keeps 5% of the
  // budget. Reducing every submesh to the same count would erase small detail
  // parts (hinges, handles) to preserve one large one.
  const double ratio = static_cast<double>(targetTriangles) /
                       static_cast<double>(result.trianglesBefore);
  size_t after = 0;
  for (auto &sm : mesh.submeshes) {
    const size_t before =
        (sm.indices.empty() ? sm.vertices.size() : sm.indices.size()) / 3;
    if (before == 0)
      continue;
    // Never take a submesh below 4 triangles -- past that it stops being a
    // shape at all, and a 2-triangle "handle" is worse than none.
    const size_t budget = std::max<size_t>(
        4, static_cast<size_t>(static_cast<double>(before) * ratio));
    const size_t got = decimateSubmesh(sm, budget);
    after += got;
    if (got > budget)
      result.notes.push_back(
          "submesh '" + sm.objectName + "' stopped at " +
          std::to_string(got) + " triangles (wanted " +
          std::to_string(budget) +
          "); further collapses would have flipped faces");
  }
  result.trianglesAfter = after;

  // Object-level bounds are per-submesh copies; refresh them or the editor and
  // any placement code keep the pre-decimation extents.
  mesh.objectBounds.clear();
  for (const auto &sm : mesh.submeshes)
    if (sm.hasBounds)
      mesh.objectBounds.emplace_back(
          sm.objectName, MeshObjectBounds{sm.aabbMin, sm.aabbMax, true});

  return result;
}

} // namespace gen
