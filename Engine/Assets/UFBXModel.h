#pragma once
#include "Material.h"
#include "Texture.h"
#include "ufbx.h"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <map>
#include <string>
#include <vector>

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
};

class UFBXModel {
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
  std::vector<UFBXSubmesh> mSubmeshes;
  std::string mDirectory;
  ufbx_scene *mScene = nullptr;

  glm::vec3 mAabbMin{1e30f};
  glm::vec3 mAabbMax{-1e30f};
  bool mHasBounds = false;

  void processNode(ufbx_node *node);
  void processMesh(ufbx_mesh *mesh, ufbx_node *node);
  GLuint loadTextureFromUFBX(ufbx_texture *tex);

  // Per-instance texture cache (replaces leaked static global)
  std::map<std::string, GLuint> mTextureCache;
};
