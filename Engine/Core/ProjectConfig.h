#pragma once

#include <array>
#include <string>

struct BlackHolePresetDefaults {
  bool valid = false;
  bool worldMode = false;
  float azimuth = 225.0f;
  float elevation = 34.0f;
  std::array<float, 3> worldPosition = {0.0f, 35.0f, -120.0f};
  float worldRadius = 12.0f;
  float viewPitchDeg = 0.0f;
  float sizeDeg = 6.4f;
  float diskTiltDeg = -32.0f;
  float diskInclinationDeg = 76.0f;
  std::array<float, 3> color = {1.0f, 0.56f, 0.12f};
  float ringIntensity = 5.8f;
  float ringWidth = 0.18f;
  float distortion = 0.86f;
  float haloIntensity = 0.34f;
  float diskSpinSpeed = 5.40f;
  float diskFlowShear = 0.22f;
  float diskTurbulence = 0.92f;
  float chromaticAberration = 0.08f;
  float eclipseStrength = 0.78f;
  float photonRingIntensity = 3.15f;
  float dopplerBoost = 0.84f;
  float jetIntensity = 0.0f;
  float coronaIntensity = 0.34f;
  float starLensIntensity = 1.45f;
  float shadowStrength = 0.94f;
  float innerDiskRadius = 1.25f;
  float outerDiskRadius = 6.8f;
  float diskTemperature = 1.12f;
  float diskDensity = 1.15f;
  float lensingStrength = 0.90f;
  float backgroundStarIntensity = 1.15f;
  float exposure = 1.0f;
  int quality = 1;
};

struct ProjectConfig {
  std::string projectRoot = ".";
  std::string shaderRoot = "shaders/glsl";
  std::string assetRoot = "assets";
  std::string startupScene;
  BlackHolePresetDefaults blackHoleDefaults;

  std::string mainVertexShader = "vertex_core.glsl";
  std::string mainFragmentShader = "fragment_core.glsl";
  std::string shadowVertexShader = "shadow_depth.vert";
  std::string shadowFragmentShader = "shadow_depth.frag";
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
  std::string ssaoFragmentShader = "ssao.frag";
  std::string ssaoBlurFragmentShader = "ssao_blur.frag";
  std::string volumetricFogFragmentShader = "volumetric_fog.frag";
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
