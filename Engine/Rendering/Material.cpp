#include "Material.h"

#include "GLStateCache.h"

void MaterialAsset::apply(Shader &shader) const {
  auto &state = GLStateCache::instance();

  shader.setInt("texDiffuse", 0);
  shader.setInt("texNormal", 1);
  shader.setInt("texRoughness", 2);
  shader.setInt("texMetallic", 3);

  if (texDiffuse != 0) {
    shader.setBool("uUseColor", false);
    state.bindTexture2D(0, texDiffuse);
  } else {
    shader.setBool("uUseColor", true);
    shader.setVec4("uColor", baseColor);
    state.bindTexture2D(0, 0);
  }

  state.bindTexture2D(1, texNormal);
  state.bindTexture2D(2, texRoughness);
  state.bindTexture2D(3, texMetallic);
}

