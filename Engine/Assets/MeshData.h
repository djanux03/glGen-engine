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

  // Some assets are authored far from their own origin. Shifts all vertices
  // and bounds so the model sits at the origin (CPU counterpart of
  // OBJModel::centerAtOrigin). BaseY: base at y=0, centered in XZ (props).
  // Center: fully centered (bodies that tumble). Idempotent.
  enum class Recenter { BaseY, Center };
  void recenter(Recenter mode) {
    glm::vec3 mn, mx;
    if (!getGlobalBounds(mn, mx))
      return;
    const glm::vec3 c = (mn + mx) * 0.5f;
    const glm::vec3 offset = (mode == Recenter::Center)
                                 ? -c
                                 : glm::vec3(-c.x, -mn.y, -c.z);
    if (glm::dot(offset, offset) < 1e-8f)
      return;
    for (auto &sm : submeshes) {
      for (auto &v : sm.vertices)
        v.pos += offset;
      if (sm.hasBounds) {
        sm.aabbMin += offset;
        sm.aabbMax += offset;
      }
    }
    for (auto &ob : objectBounds) {
      if (ob.second.hasBounds) {
        ob.second.aabbMin += offset;
        ob.second.aabbMax += offset;
      }
    }
  }
};
