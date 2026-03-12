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
  float roughness = 0.8f;
  float metallic = 0.0f;
  float ao = 1.0f;

  // Channel selectors: 0=R, 1=G, 2=B, 3=A.
  int roughnessChannel = 0;
  int metallicChannel = 0;
  int aoChannel = 0;
  int opacityChannel = 3;

  GLuint texDiffuse = 0;
  GLuint texNormal = 0;
  GLuint texRoughness = 0;
  GLuint texMetallic = 0;
  GLuint texAO = 0;
  GLuint texEmissive = 0;
  GLuint texOpacity = 0;

  glm::vec3 emissiveColor = glm::vec3(0.0f);
  float emissiveStrength = 1.0f;
  float alphaCutoff = 0.0f;
  bool roughnessMapIsGloss = false;

  void apply(Shader &shader) const;
};
