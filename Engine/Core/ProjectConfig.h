#pragma once

#include <string>

struct ProjectConfig {
  std::string projectRoot = ".";
  std::string shaderRoot = "shaders/glsl";
  std::string assetRoot = "assets";

  std::string mainVertexShader = "vertex_core.glsl";
  std::string mainFragmentShader = "fragment_core.glsl";
  std::string shadowVertexShader = "point_shadow_depth.vert";
  std::string shadowFragmentShader = "point_shadow_depth.frag";
  std::string hdrSkyVertexShader = "hdr_sky.vert";
  std::string hdrSkyFragmentShader = "hdr_sky.frag";
  std::string fireBillboardVertexShader = "fire_billboard.vert";
  std::string fireBillboardFragmentShader = "fire_billboard.frag";
  std::string smokeBillboardFragmentShader = "smoke_billboard.frag";
  std::string projectileVertexShader = "projectile.vert";
  std::string projectileFragmentShader = "projectile.frag";

  std::string screenQuadVertexShader = "screen_quad.vert";
  std::string bloomExtractFragmentShader = "bloom_extract.frag";
  std::string bloomBlurFragmentShader = "bloom_blur.frag";
  std::string bloomCompositeFragmentShader = "bloom_composite.frag";

  std::string grassSideTexture = "grass_side.png";
  std::string grassTopTexture = "grass_top.png";
  std::string skyHDR = "hdr/hdr_1/cloudy.hdr";
  std::string fireTexture =
      "pngtree-realistic-3d-fire-flame-effect-for-designs-png-image_13631567."
      "png";

  bool loadFromFile(const std::string &path);
  bool saveToFile(const std::string &path) const;

  std::string shaderPath(const std::string &rel) const;
  std::string assetPath(const std::string &rel) const;
  std::string projectPath(const std::string &rel) const;
};
