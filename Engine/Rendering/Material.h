#pragma once

#include "Shader.h"

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>

enum class ShaderVariant {
  Lit = 0,
  Transparent = 1,
  Additive = 2,
};

struct MaterialAsset {
  std::string id;
  ShaderVariant variant = ShaderVariant::Lit;

  glm::vec4 baseColor = glm::vec4(1.0f);
  GLuint texDiffuse = 0;
  GLuint texNormal = 0;
  GLuint texRoughness = 0;
  GLuint texMetallic = 0;

  void apply(Shader &shader) const;
};

