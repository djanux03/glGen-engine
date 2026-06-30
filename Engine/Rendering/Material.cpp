#include "Material.h"

#include "GLStateCache.h"
#include "Shader.h"
#include <algorithm>

void applyMaterial(const MaterialAsset &m, Shader &shader) {
  auto &state = GLStateCache::instance();

  shader.setInt("texDiffuse", 0);
  shader.setInt("texNormal", 2);
  shader.setInt("texRoughness", 3);
  shader.setInt("texMetallic", 4);
  shader.setInt("texAO", 5);
  shader.setInt("texEmissive", 6);
  shader.setInt("texOpacity", 7);

  shader.setFloat("uRoughness", std::clamp(m.roughness, 0.0f, 1.0f));
  shader.setFloat("uMetallic", std::clamp(m.metallic, 0.0f, 1.0f));
  shader.setFloat("uAO", std::clamp(m.ao, 0.0f, 1.0f));
  shader.setInt("uRoughnessChannel", m.roughnessChannel);
  shader.setInt("uMetallicChannel", m.metallicChannel);
  shader.setInt("uAOChannel", m.aoChannel);
  shader.setInt("uOpacityChannel", m.opacityChannel);
  shader.setBool("uRoughnessMapIsGloss", m.roughnessMapIsGloss);
  shader.setVec3("uEmissiveColor", m.emissiveColor);
  shader.setFloat("uEmissiveStrength", std::max(0.0f, m.emissiveStrength));
  shader.setFloat("uAlphaCutoff", std::clamp(m.alphaCutoff, 0.0f, 1.0f));

  if (m.texDiffuse != 0) {
    shader.setBool("uUseColor", false);
    state.bindTexture2D(0, m.texDiffuse);
  } else {
    shader.setBool("uUseColor", true);
    shader.setVec4("uColor", m.baseColor);
    state.bindTexture2D(0, 0);
  }

  if (m.texRoughness != 0) {
    shader.setBool("uHasRoughnessMap", true);
    state.bindTexture2D(3, m.texRoughness);
  } else {
    shader.setBool("uHasRoughnessMap", false);
    state.bindTexture2D(3, 0);
  }

  if (m.texMetallic != 0) {
    shader.setBool("uHasMetallicMap", true);
    state.bindTexture2D(4, m.texMetallic);
  } else {
    shader.setBool("uHasMetallicMap", false);
    state.bindTexture2D(4, 0);
  }

  if (m.texAO != 0) {
    shader.setBool("uHasAOMap", true);
    state.bindTexture2D(5, m.texAO);
  } else {
    shader.setBool("uHasAOMap", false);
    state.bindTexture2D(5, 0);
  }

  if (m.texEmissive != 0) {
    shader.setBool("uHasEmissiveMap", true);
    state.bindTexture2D(6, m.texEmissive);
  } else {
    shader.setBool("uHasEmissiveMap", false);
    state.bindTexture2D(6, 0);
  }

  if (m.texOpacity != 0) {
    shader.setBool("uHasOpacityMap", true);
    state.bindTexture2D(7, m.texOpacity);
  } else {
    shader.setBool("uHasOpacityMap", false);
    state.bindTexture2D(7, 0);
  }

  shader.setBool("uHasNormalMap", m.texNormal != 0);
  state.bindTexture2D(2, m.texNormal);
}
