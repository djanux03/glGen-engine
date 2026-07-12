#pragma once
#include "Material.h"
#include "Texture.h"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <map>
#include <string>
#include <vector>

struct MeshData;

struct UFBXVertex {
  glm::vec3 pos;
  glm::vec2 uv;
  glm::vec3 normal;
};

struct UFBXSubmesh {
  std::string name;
  std::string materialName;
  GLuint vao = 0, vbo = 0, ebo = 0;
  GLsizei indexCount = 0;

  MaterialAsset material;

  // Instancing cache (VAO-level state)
  GLuint instancedVBO = 0;
  bool instancingReady = false;
};

class UFBXModel {
public:
  bool loadFromFile(const std::string &path);
  // GPU-upload half: builds GL buffers/textures from parsed CPU data.
  bool loadFromData(const MeshData &data);
  void draw(class Shader &shader, const glm::vec3 &pos, const glm::vec3 &rot,
            const glm::vec3 &scale,
            const MaterialAsset *materialOverride = nullptr);
  void drawInstanced(class Shader &shader, unsigned int instanceVBO,
                     int instanceCount);

  void drawDepth(class Shader &shadowShader, const glm::vec3 &pos,
                 const glm::vec3 &rot, const glm::vec3 &scale);
  void drawDepthInstanced(class Shader &shadowShader, unsigned int instanceVBO,
                          int instanceCount);
  void shutdown();
  std::size_t submeshCount() const { return mSubmeshes.size(); }

  // Global AABB bounds for the entire loaded model
  bool getGlobalBounds(glm::vec3 &outMin, glm::vec3 &outMax) const;

private:
  struct ShadowMesh {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    GLsizei indexCount = 0;
    GLuint instancedVBO = 0;
    bool instancingReady = false;
  };

  GLuint textureFor_(const MeshData &data, const std::string &path,
                     TextureUsage usage);

  std::vector<UFBXSubmesh> mSubmeshes;
  std::string mSourcePath;

  glm::vec3 mAabbMin{1e30f};
  glm::vec3 mAabbMax{-1e30f};
  bool mHasBounds = false;
  ShadowMesh mShadowMesh;

  // Per-instance texture cache; entries are owned and freed in shutdown()
  std::map<std::string, GLuint> mTextureCache;
};
