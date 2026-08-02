#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

// MaterialAsset is pure, backend-neutral data so it can live in ECS components
// without pulling in any graphics API. The renderer interprets the texture
// handles and applies the material.

enum class ShaderVariant {
  Lit = 0,
  Transparent = 1,
  Additive = 2,
};

struct MaterialAsset {
  std::string id;
  std::string sourceAssetPath;
  std::string sourceMaterialName;
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

  // GPU texture handles. Backend-neutral: an OpenGL texture name or a Vulkan
  // bindless index, depending on the active renderer. 0 = none.
  uint32_t texDiffuse = 0;
  std::string texDiffusePath;
  uint32_t texNormal = 0;
  std::string texNormalPath;
  uint32_t texRoughness = 0;
  std::string texRoughnessPath;
  uint32_t texMetallic = 0;
  std::string texMetallicPath;
  uint32_t texAO = 0;
  std::string texAOPath;
  uint32_t texEmissive = 0;
  std::string texEmissivePath;
  uint32_t texOpacity = 0;
  std::string texOpacityPath;

  glm::vec3 emissiveColor = glm::vec3(0.0f);
  float emissiveStrength = 1.0f;
  float alphaCutoff = 0.0f;
  bool roughnessMapIsGloss = false;

  bool usesAnyTextureMaps() const {
    return texDiffuse != 0 || texNormal != 0 || texRoughness != 0 ||
           texMetallic != 0 || texAO != 0 || texEmissive != 0 ||
           texOpacity != 0;
  }
};
