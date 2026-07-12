#pragma once
#include "Material.h"
#include "Texture.h"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <map>
#include <string>
#include <vector>

struct MeshData;

struct FBXVertex {
  glm::vec3 pos;
  glm::vec2 uv;
  glm::vec3 normal;
};

struct FBXSubmesh {
  std::string name;
  std::string materialName;
  GLuint vao = 0, vbo = 0, ebo = 0;
  GLsizei indexCount = 0;

  MaterialAsset material;
};

class FBXModel {
public:
  bool loadFromFile(const std::string &path);
  // GPU-upload half: builds GL buffers/textures from parsed CPU data.
  bool loadFromData(const MeshData &data);
  void draw(class Shader &shader, const glm::vec3 &pos, const glm::vec3 &rot,
            const glm::vec3 &scale,
            const MaterialAsset *materialOverride = nullptr);
  void drawDepth(class Shader &shadowShader, const glm::vec3 &pos,
                 const glm::vec3 &rot, const glm::vec3 &scale);
  void shutdown();
  std::size_t submeshCount() const { return mSubmeshes.size(); }

  // Global AABB bounds for the entire loaded model
  bool getGlobalBounds(glm::vec3 &outMin, glm::vec3 &outMax) const;

private:
  GLuint textureFor_(const MeshData &data, const std::string &path,
                     TextureUsage usage);

  std::vector<FBXSubmesh> mSubmeshes;
  std::string mSourcePath;

  glm::vec3 mAabbMin{1e30f};
  glm::vec3 mAabbMax{-1e30f};
  bool mHasBounds = false;

  // Per-instance texture cache; entries are owned and freed in shutdown()
  std::map<std::string, GLuint> mTextureCache;
};
