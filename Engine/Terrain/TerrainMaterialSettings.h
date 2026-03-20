#pragma once

#include <glm/glm.hpp>
#include <string>

struct TerrainMaterialSettings {
  bool enableCustom = true;

  float macroScale = 0.05f;
  float detailScale = 1.0f;
  float normalDetailScale = 1.9f;
  float normalStrength = 0.85f;

  float cliffStart = 0.22f;
  float cliffEnd = 0.75f;
  float snowStartHeight = 8.0f;
  float snowEndHeight = 20.0f;
  float lowStartHeight = -1.0f;
  float lowEndHeight = 4.0f;

  float macroVariationStrength = 0.20f;
  float cliffDesatStrength = 0.35f;

  glm::vec3 grassA = glm::vec3(0.17f, 0.39f, 0.12f);
  glm::vec3 grassB = glm::vec3(0.30f, 0.56f, 0.18f);
  glm::vec3 dirtA = glm::vec3(0.24f, 0.18f, 0.11f);
  glm::vec3 dirtB = glm::vec3(0.36f, 0.26f, 0.14f);
  glm::vec3 rockA = glm::vec3(0.31f, 0.31f, 0.32f);
  glm::vec3 rockB = glm::vec3(0.46f, 0.43f, 0.39f);
  glm::vec3 sandA = glm::vec3(0.63f, 0.55f, 0.35f);
  glm::vec3 sandB = glm::vec3(0.85f, 0.76f, 0.54f);
  glm::vec3 snowA = glm::vec3(0.78f, 0.83f, 0.90f);
  glm::vec3 snowB = glm::vec3(0.97f, 0.98f, 1.00f);

  float roughGrass = 0.84f;
  float roughDirt = 0.90f;
  float roughRock = 0.63f;
  float roughSand = 0.88f;
  float roughSnow = 0.42f;

  // Optional realistic ground layer.
  bool useGroundTextures = false;
  std::string groundAlbedoPath;
  std::string groundNormalPath;
  std::string groundRoughnessPath;
  float groundTiling = 0.18f;
  float groundBlendStrength = 1.0f;
  float groundRoughness = 0.82f;

  // Stylized option: flatten green-biome terrain into a single green tint.
  bool flatGreenEnabled = true;
  glm::vec3 flatGreenColor = glm::vec3(0.26f, 0.62f, 0.27f);
};
