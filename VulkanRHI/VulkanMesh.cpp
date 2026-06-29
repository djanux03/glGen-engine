#include "VulkanMesh.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include <cstdio>
#include <limits>
#include <map>
#include <unordered_map>

namespace vkrhi {

namespace {

struct IndexKey {
  int v, n, t;
  bool operator==(const IndexKey &o) const {
    return v == o.v && n == o.n && t == o.t;
  }
};
struct IndexKeyHash {
  size_t operator()(const IndexKey &k) const {
    size_t h = 1469598103934665603ull;
    for (int x : {k.v, k.n, k.t}) {
      h ^= static_cast<size_t>(x);
      h *= 1099511628211ull;
    }
    return h;
  }
};

struct Triangle {
  uint32_t i[3];
  int material;
};

std::string directoryOf(const std::string &path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string(".")
                                    : path.substr(0, slash);
}

} // namespace

bool loadObj(const std::string &path, MeshData &out) {
  const std::string baseDir = directoryOf(path);

  tinyobj::attrib_t attrib;
  std::vector<tinyobj::shape_t> shapes;
  std::vector<tinyobj::material_t> materials;
  std::string warn, err;

  // Pass the OBJ directory so the .mtl and its textures resolve correctly.
  const bool ok = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err,
                                   path.c_str(), baseDir.c_str(),
                                   /*triangulate=*/true);
  if (!warn.empty())
    std::fprintf(stderr, "[VulkanMesh] warn: %s\n", warn.c_str());
  if (!ok || !err.empty()) {
    std::fprintf(stderr, "[VulkanMesh] failed to load '%s': %s\n", path.c_str(),
                 err.c_str());
    return false;
  }

  const bool haveNormals = !attrib.normals.empty();
  std::unordered_map<IndexKey, uint32_t, IndexKeyHash> unique;
  std::vector<Triangle> triangles;

  for (const auto &shape : shapes) {
    const size_t faceCount = shape.mesh.indices.size() / 3;
    for (size_t f = 0; f < faceCount; ++f) {
      Triangle tri{};
      tri.material = f < shape.mesh.material_ids.size()
                         ? shape.mesh.material_ids[f]
                         : -1;
      for (int c = 0; c < 3; ++c) {
        const tinyobj::index_t idx = shape.mesh.indices[f * 3 + c];
        IndexKey key{idx.vertex_index, idx.normal_index, idx.texcoord_index};
        auto found = unique.find(key);
        if (found != unique.end()) {
          tri.i[c] = found->second;
          continue;
        }
        MeshVertex vert{};
        vert.pos = {attrib.vertices[3 * idx.vertex_index + 0],
                    attrib.vertices[3 * idx.vertex_index + 1],
                    attrib.vertices[3 * idx.vertex_index + 2]};
        if (haveNormals && idx.normal_index >= 0) {
          vert.normal = {attrib.normals[3 * idx.normal_index + 0],
                         attrib.normals[3 * idx.normal_index + 1],
                         attrib.normals[3 * idx.normal_index + 2]};
        }
        if (idx.texcoord_index >= 0) {
          vert.uv = {attrib.texcoords[2 * idx.texcoord_index + 0],
                     1.0f - attrib.texcoords[2 * idx.texcoord_index + 1]};
        }
        const uint32_t newIndex = static_cast<uint32_t>(out.vertices.size());
        unique.emplace(key, newIndex);
        out.vertices.push_back(vert);
        tri.i[c] = newIndex;
      }
      triangles.push_back(tri);
    }
  }

  if (out.vertices.empty()) {
    std::fprintf(stderr, "[VulkanMesh] '%s' produced no geometry\n",
                 path.c_str());
    return false;
  }

  // Group triangles into contiguous index runs per material (ordered map keeps
  // output deterministic; -1 = no material).
  std::map<int, std::vector<uint32_t>> byMaterial;
  for (const Triangle &tri : triangles) {
    auto &bucket = byMaterial[tri.material];
    bucket.push_back(tri.i[0]);
    bucket.push_back(tri.i[1]);
    bucket.push_back(tri.i[2]);
  }
  for (auto &kv : byMaterial) {
    SubMesh sub;
    sub.materialId = kv.first;
    sub.indexOffset = static_cast<uint32_t>(out.indices.size());
    sub.indexCount = static_cast<uint32_t>(kv.second.size());
    out.indices.insert(out.indices.end(), kv.second.begin(), kv.second.end());
    out.submeshes.push_back(sub);
  }

  // Resolve material diffuse textures.
  out.materials.resize(materials.size());
  for (size_t i = 0; i < materials.size(); ++i) {
    if (!materials[i].diffuse_texname.empty())
      out.materials[i].diffuseTexturePath =
          baseDir + "/" + materials[i].diffuse_texname;
  }

  // Synthesize smooth normals if the file had none.
  if (!haveNormals) {
    for (size_t i = 0; i + 2 < out.indices.size(); i += 3) {
      MeshVertex &a = out.vertices[out.indices[i + 0]];
      MeshVertex &b = out.vertices[out.indices[i + 1]];
      MeshVertex &c = out.vertices[out.indices[i + 2]];
      const glm::vec3 faceN = glm::cross(b.pos - a.pos, c.pos - a.pos);
      a.normal += faceN;
      b.normal += faceN;
      c.normal += faceN;
    }
    for (auto &v : out.vertices) {
      if (glm::dot(v.normal, v.normal) > 0.0f)
        v.normal = glm::normalize(v.normal);
      else
        v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
    }
  }

  // Normalize to a unit box centered at the origin.
  glm::vec3 lo(std::numeric_limits<float>::max());
  glm::vec3 hi(std::numeric_limits<float>::lowest());
  for (const auto &v : out.vertices) {
    lo = glm::min(lo, v.pos);
    hi = glm::max(hi, v.pos);
  }
  const glm::vec3 center = (lo + hi) * 0.5f;
  const glm::vec3 extent = hi - lo;
  const float maxExtent = glm::max(extent.x, glm::max(extent.y, extent.z));
  const float scale = maxExtent > 0.0f ? 1.0f / maxExtent : 1.0f;
  for (auto &v : out.vertices)
    v.pos = (v.pos - center) * scale;

  std::fprintf(stderr,
               "[VulkanMesh] Loaded '%s': %zu vertices, %zu indices, "
               "%zu submeshes, %zu materials (normals %s)\n",
               path.c_str(), out.vertices.size(), out.indices.size(),
               out.submeshes.size(), out.materials.size(),
               haveNormals ? "from file" : "synthesized");
  return true;
}

} // namespace vkrhi
