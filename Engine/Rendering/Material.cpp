#include "Material.h"

#include "GLStateCache.h"
#include <algorithm>

void MaterialAsset::apply(Shader &shader) const {
  auto &state = GLStateCache::instance();

  shader.setInt("texDiffuse", 0);
  shader.setInt("texNormal", 2);
  shader.setInt("texRoughness", 3);
  shader.setInt("texMetallic", 4);
  shader.setInt("texAO", 5);
  shader.setInt("texEmissive", 6);
  shader.setInt("texOpacity", 7);

  shader.setFloat("uRoughness", std::clamp(roughness, 0.0f, 1.0f));
  shader.setFloat("uMetallic", std::clamp(metallic, 0.0f, 1.0f));
  shader.setFloat("uAO", std::clamp(ao, 0.0f, 1.0f));
  shader.setInt("uRoughnessChannel", roughnessChannel);
  shader.setInt("uMetallicChannel", metallicChannel);
  shader.setInt("uAOChannel", aoChannel);
  shader.setInt("uOpacityChannel", opacityChannel);
  shader.setBool("uRoughnessMapIsGloss", roughnessMapIsGloss);
  shader.setVec3("uEmissiveColor", emissiveColor);
  shader.setFloat("uEmissiveStrength", std::max(0.0f, emissiveStrength));
  shader.setFloat("uAlphaCutoff", std::clamp(alphaCutoff, 0.0f, 1.0f));

  if (texDiffuse != 0) {
    shader.setBool("uUseColor", false);
    state.bindTexture2D(0, texDiffuse);
  } else {
    shader.setBool("uUseColor", true);
    shader.setVec4("uColor", baseColor);
    state.bindTexture2D(0, 0);
  }

  // Roughness
  if (texRoughness != 0) {
    shader.setBool("uHasRoughnessMap", true);
    state.bindTexture2D(3, texRoughness);
  } else {
    shader.setBool("uHasRoughnessMap", false);
    state.bindTexture2D(3, 0);
  }

  // Metallic
  if (texMetallic != 0) {
    shader.setBool("uHasMetallicMap", true);
    state.bindTexture2D(4, texMetallic);
  } else {
    shader.setBool("uHasMetallicMap", false);
    state.bindTexture2D(4, 0);
  }

  if (texAO != 0) {
    shader.setBool("uHasAOMap", true);
    state.bindTexture2D(5, texAO);
  } else {
    shader.setBool("uHasAOMap", false);
    state.bindTexture2D(5, 0);
  }

  if (texEmissive != 0) {
    shader.setBool("uHasEmissiveMap", true);
    state.bindTexture2D(6, texEmissive);
  } else {
    shader.setBool("uHasEmissiveMap", false);
    state.bindTexture2D(6, 0);
  }

  if (texOpacity != 0) {
    shader.setBool("uHasOpacityMap", true);
    state.bindTexture2D(7, texOpacity);
  } else {
    shader.setBool("uHasOpacityMap", false);
    state.bindTexture2D(7, 0);
  }

  shader.setBool("uHasNormalMap", texNormal != 0);
  state.bindTexture2D(2, texNormal);
}
