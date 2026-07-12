#pragma once
#include "Rendering/Material.h"
#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <utility>
#include <vector>

// API-agnostic CPU-side model data. This is the "parse" half of the asset
// pipeline: file loaders (tinyobj/tinygltf/ufbx) produce MeshData, and each
// rendering backend (OpenGL today, Vulkan next) uploads it to the GPU.

struct MeshVertex {
  glm::vec3 pos;
  glm::vec2 uv;
  glm::vec3 normal;
};

// Decoded 8-bit image payload for textures that cannot be (re)read from disk
// at upload time — embedded glTF/FBX images, or glTF images tinygltf already
// decoded. `component` is 1, 3 or 4 channels, tightly packed.
struct MeshImage {
  std::string key; // matches a MaterialAsset tex*Path entry
  int width = 0;
  int height = 0;
  int component = 4;
  std::vector<unsigned char> pixels;
};

struct MeshSubmeshData {
  std::string objectName;   // OBJ "o ..." / node name
  std::string materialName; // MTL "newmtl ..." / material name
  std::string debugName;

  // Texture ids are 0 here; tex*Path (or a MeshImage payload with the same
  // key) identifies each texture for the uploading backend.
  MaterialAsset material;

  std::vector<MeshVertex> vertices;
  std::vector<uint32_t> indices; // empty = non-indexed triangle list

  glm::vec3 aabbMin{1e30f};
  glm::vec3 aabbMax{-1e30f};
  bool hasBounds = false;
};

struct MeshObjectBounds {
  glm::vec3 aabbMin{1e30f};
  glm::vec3 aabbMax{-1e30f};
  bool hasBounds = false;
};

struct MeshData {
  std::string sourcePath;
  std::vector<MeshSubmeshData> submeshes;
  std::vector<MeshImage> images;
  // OBJ files: aggregate bounds per object name ("o ..." statements).
  std::vector<std::pair<std::string, MeshObjectBounds>> objectBounds;

  bool getGlobalBounds(glm::vec3 &outMin, glm::vec3 &outMax) const {
    glm::vec3 mn(1e30f), mx(-1e30f);
    bool has = false;
    for (const auto &sm : submeshes) {
      if (!sm.hasBounds)
        continue;
      mn = glm::min(mn, sm.aabbMin);
      mx = glm::max(mx, sm.aabbMax);
      has = true;
    }
    if (!has)
      return false;
    outMin = mn;
    outMax = mx;
    return true;
  }

  const MeshImage *findImage(const std::string &key) const {
    for (const auto &img : images)
      if (img.key == key)
        return &img;
    return nullptr;
  }
};
