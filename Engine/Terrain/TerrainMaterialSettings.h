#pragma once

#include <glm/glm.hpp>
#include <string>

struct TerrainMaterialSettings {
  bool enableCustom = true;

  float macroScale = 0.045f;
  float detailScale = 1.22f;
  float normalDetailScale = 2.15f;
  float normalStrength = 0.95f;

  float cliffStart = 0.22f;
  float cliffEnd = 0.75f;
  float snowStartHeight = 8.0f;
  float snowEndHeight = 20.0f;
  float lowStartHeight = -1.0f;
  float lowEndHeight = 4.0f;

  float macroVariationStrength = 0.28f;
  float cliffDesatStrength = 0.28f;

  glm::vec3 grassA = glm::vec3(0.12f, 0.31f, 0.10f);
  glm::vec3 grassB = glm::vec3(0.34f, 0.58f, 0.20f);
  glm::vec3 dirtA = glm::vec3(0.18f, 0.13f, 0.08f);
  glm::vec3 dirtB = glm::vec3(0.40f, 0.28f, 0.15f);
  glm::vec3 rockA = glm::vec3(0.24f, 0.25f, 0.26f);
  glm::vec3 rockB = glm::vec3(0.52f, 0.49f, 0.43f);
  glm::vec3 sandA = glm::vec3(0.58f, 0.49f, 0.29f);
  glm::vec3 sandB = glm::vec3(0.88f, 0.78f, 0.52f);
  glm::vec3 snowA = glm::vec3(0.72f, 0.78f, 0.86f);
  glm::vec3 snowB = glm::vec3(0.97f, 0.98f, 1.00f);

  float roughGrass = 0.84f;
  float roughDirt = 0.90f;
  float roughRock = 0.63f;
  float roughSand = 0.88f;
  float roughSnow = 0.42f;

  // Optional per-layer color textures for procedural terrain layers.
  bool useLayerTextures = false;
  std::string grassAlbedoPath;
  std::string grassNormalPath;
  std::string grassRoughnessPath;
  std::string dirtAlbedoPath;
  std::string dirtNormalPath;
  std::string dirtRoughnessPath;
  float layerTextureTiling = 0.18f;
  float layerTextureStrength = 0.85f;
  float layerNormalStrength = 0.75f;
  float layerRoughnessStrength = 1.0f;

  // Optional realistic ground layer.
  bool useGroundTextures = false;
  std::string groundAlbedoPath;
  std::string groundNormalPath;
  std::string groundRoughnessPath;
  std::string groundHeightPath;
  float groundTiling = 0.18f;
  float groundBlendStrength = 1.0f;
  float groundRoughness = 0.82f;
  float groundHeightStrength = 0.25f;
  bool groundPseudoHeightEnabled = false;
  int groundPseudoHeightSource = 0; // 0=Luma, 1=Normal
  float groundPseudoHeightContrast = 1.0f;
  float groundPseudoHeightBias = 0.0f;
  bool groundGradeEnabled = false;
  float groundGradeSaturation = 1.0f;
  float groundGradeContrast = 1.0f;
  float groundGradeGamma = 1.0f;
  glm::vec3 groundGradeTint = glm::vec3(1.0f);
  float groundBrightness = 1.0f;
  float groundVariationStrength = 0.35f;
  float groundVariationScale = 0.03f;
  bool groundFullOverride = false;
  bool sunGlintEnabled = true;
  float sunGlintIntensity = 1.15f;
  float sunGlintSharpness = 72.0f;
  float sunGlintMaskScale = 0.014f;
  float sunGlintMaskStrength = 0.55f;
  float sunGlintBaseSpecular = 0.08f;
  bool sunGlintUseSceneSun = true;
  float sunGlintDirectionAzimuth = 145.0f;
  float sunGlintDirectionElevation = 28.0f;
  float sunGlintBandWidth = 0.16f;

  // Stylized option: flatten green-biome terrain into a single green tint.
  // Keep this opt-in so new terrain uses the richer procedural material by default.
  bool flatGreenEnabled = false;
  glm::vec3 flatGreenColor = glm::vec3(0.26f, 0.62f, 0.27f);
};
