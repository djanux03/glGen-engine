#pragma once
#include "Material.h"
#include "Texture.h"
#include "tiny_gltf.h"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

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
  void draw(class Shader &shader, const glm::vec3 &pos, const glm::vec3 &rot,
            const glm::vec3 &scale);
  void drawDepth(class Shader &shadowShader, const glm::vec3 &pos,
                 const glm::vec3 &rot, const glm::vec3 &scale);
  void shutdown();
  std::size_t submeshCount() const { return mSubmeshes.size(); }

  // Global AABB bounds for the entire loaded model
  bool getGlobalBounds(glm::vec3 &outMin, glm::vec3 &outMax) const;

private:
  std::vector<FBXSubmesh> mSubmeshes;
  std::string mDirectory;
  tinygltf::Model mModel;

  glm::vec3 mAabbMin{1e30f};
  glm::vec3 mAabbMax{-1e30f};
  bool mHasBounds = false;

  void processNode(int nodeIndex);
  void processMesh(const tinygltf::Mesh &mesh);
  GLuint LoadTextureFromGLTF(int textureIndex);
  GLuint CreateTextureFromImage(const tinygltf::Image &image);

  // Per-instance texture cache (replaces leaked static global)
  std::map<std::string, GLuint> mTextureCache;
};
