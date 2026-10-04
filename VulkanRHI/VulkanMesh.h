#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace vkrhi {

// CPU-side mesh. Deliberately holds no Vulkan objects so the loader stays
// decoupled from both the renderer and the OpenGL engine; the renderer turns
// this into device-local buffers and bindless textures.
struct MeshVertex {
  glm::vec3 pos;
  glm::vec3 normal;
  glm::vec2 uv;
  // Terrain-only ground fields, copied from ::MeshVertex::terrainParams
  // during buildMeshFromData()'s flatten step; zero/unused for every
  // non-terrain mesh. See ::MeshVertex's own comment (Engine/Assets/
  // MeshData.h) for why this exists on the CPU-side struct too.
  glm::vec4 terrainParams{0.0f};
  float grassGroundOcclusion = 0.0f;
};

// A contiguous run of the index buffer that shares one material. Drawn with a
// single vkCmdDrawIndexed using the material's bindless texture index.
struct SubMesh {
  uint32_t indexOffset = 0;
  uint32_t indexCount = 0;
  int materialId = -1; // index into MeshData::materials, or -1 for none
};

struct MeshMaterial {
  // Absolute path to the diffuse (base-color) texture, resolved relative to the
  // OBJ. Empty when the material has no map_Kd.
  std::string diffuseTexturePath;
};

struct MeshData {
  std::vector<MeshVertex> vertices;
  std::vector<uint32_t> indices; // grouped by material (one run per submesh)
  std::vector<SubMesh> submeshes;
  std::vector<MeshMaterial> materials;
};

// Loads an OBJ via tinyobjloader:
//  * triangulated, vertices de-duplicated on (position, normal, uv)
//  * normals synthesized from face geometry when the file has none
//  * indices grouped into per-material submeshes
//  * material map_Kd paths resolved relative to the OBJ's directory
//  * normalized so the largest dimension spans 1 unit, centered at the origin.
bool loadObj(const std::string &path, MeshData &out);

} // namespace vkrhi
