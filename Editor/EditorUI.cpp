#include "EditorUI.h"
#include "AssetManager.h"
#include "CloudFX.h"
#include "Core/EngineEvents.h"
#include "Core/EventBus.h"
#include "Core/Logger.h"
#include "Core/ProjectConfig.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "ECS/Systems/DestructionSystem.h"
#include "ECS/Systems/EditorCamera.h"
#include "EditorIcons.h"
#include "FBXModel.h"
#include "FireFX.h"
#include "HDRSky.h"
#include "OBJModel.h"
#include "PostProcessor.h"
#include "Scene.h"
#include "SunFX.h"
#include "Terrain/TerrainSystem.h"
#include "Texture.h"
#include "UFBXModel.h"
#include "AudioSettings.h"
#include "../Runtime/Framework/EditorState.h"
#include "../Runtime/Framework/GameplayState.h"

#include <imgui.h>

#include "tinyfiledialogs.h"
#include <ImGuizmo.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

// Helper: build TRS matrix from euler-degrees rotation
static auto buildTRS = [](const glm::vec3 &pos, const glm::vec3 &rotDeg,
                          const glm::vec3 &scale) {
  glm::mat4 m(1.0f);
  m = glm::translate(m, pos);
  m = glm::rotate(m, glm::radians(rotDeg.y), glm::vec3(0, 1, 0));
  m = glm::rotate(m, glm::radians(rotDeg.x), glm::vec3(1, 0, 0));
  m = glm::rotate(m, glm::radians(rotDeg.z), glm::vec3(0, 0, 1));
  m = glm::scale(m, scale);
  return m;
};

static float normalizeAngleDeg(float angle) {
  while (angle > 180.0f)
    angle -= 360.0f;
  while (angle < -180.0f)
    angle += 360.0f;
  return angle;
}

static void normalizeEulerDeg(glm::vec3 &euler) {
  euler.x = normalizeAngleDeg(euler.x);
  euler.y = normalizeAngleDeg(euler.y);
  euler.z = normalizeAngleDeg(euler.z);
}

// Matches TransformComponent::getMatrix/buildTRS: T * Ry * Rx * Rz * S.
// ImGuizmo's built-in Euler decomposition uses a different convention, which
// makes entity rotations jump or drift after gizmo edits.
static bool decomposeTRSYXZ(const glm::mat4 &m, glm::vec3 &pos,
                            glm::vec3 &rotDeg, glm::vec3 &scale) {
  constexpr float kEpsilon = 1e-5f;

  pos = glm::vec3(m[3]);

  glm::vec3 c0(m[0]);
  glm::vec3 c1(m[1]);
  glm::vec3 c2(m[2]);
  scale = glm::vec3(glm::length(c0), glm::length(c1), glm::length(c2));

  if (scale.x < kEpsilon || scale.y < kEpsilon || scale.z < kEpsilon)
    return false;

  glm::mat3 r;
  r[0] = c0 / scale.x;
  r[1] = c1 / scale.y;
  r[2] = c2 / scale.z;

  if (glm::determinant(r) < 0.0f) {
    scale.x = -scale.x;
    r[0] = -r[0];
  }

  const float pitch = std::asin(std::clamp(-r[2][1], -1.0f, 1.0f));
  const float cosPitch = std::cos(pitch);

  float yaw = 0.0f;
  float roll = 0.0f;
  if (std::abs(cosPitch) > kEpsilon) {
    yaw = std::atan2(r[2][0], r[2][2]);
    roll = std::atan2(r[0][1], r[1][1]);
  } else {
    // At the singularity yaw and roll are coupled. Preserve a stable result
    // instead of letting the editor explode into large equivalent angles.
    yaw = std::atan2(-r[0][2], r[0][0]);
    roll = 0.0f;
  }

  rotDeg = glm::degrees(glm::vec3(pitch, yaw, roll));
  normalizeEulerDeg(rotDeg);
  return true;
}

static bool pathUsesLossyImage(const std::string &path) {
  if (path.empty())
    return false;
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return ext == ".jpg" || ext == ".jpeg";
}

static const char *textureUsageLabel(TextureUsage usage) {
  return usage == TextureUsage::Color ? "Color (sRGB)" : "Data (Linear)";
}

static bool autoFitBoxColliderFromMesh(Registry &reg, uint32_t entity,
                                       ColliderComponent &col) {
  if (!reg.has<MeshComponent>(entity))
    return false;

  auto &mesh = reg.get<MeshComponent>(entity);
  glm::vec3 minAABB(0.0f), maxAABB(0.0f);
  bool hasBounds = false;
  if (mesh.objModel && mesh.objModel->getGlobalBounds(minAABB, maxAABB)) {
    hasBounds = true;
  } else if (mesh.gltfModel &&
             mesh.gltfModel->getGlobalBounds(minAABB, maxAABB)) {
    hasBounds = true;
  } else if (mesh.ufbxModel &&
             mesh.ufbxModel->getGlobalBounds(minAABB, maxAABB)) {
    hasBounds = true;
  }

  if (!hasBounds)
    return false;

  col.shape = ColliderComponent::Shape::Box;
  col.offset = (minAABB + maxAABB) * 0.5f;
  col.dimensions = glm::max(maxAABB - minAABB, glm::vec3(0.1f));
  return true;
}

static void drawTextureStatusRow(const char *label, const std::string &path,
                                 TextureUsage usage, bool required = false) {
  ImGui::PushID(label);

  const bool hasPath = !path.empty();
  const bool exists = hasPath && std::filesystem::exists(path);
  const bool lossy = pathUsesLossyImage(path);
  GLuint tex = 0;
  if (exists) {
    tex = LoadTexture2DCached(path, true, usage);
  }

  ImGui::BeginGroup();
  if (tex != 0) {
    ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(56.0f, 56.0f));
  } else {
    ImGui::Dummy(ImVec2(56.0f, 56.0f));
  }
  ImGui::EndGroup();

  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::Text("%s", label);
  ImGui::TextDisabled("%s", textureUsageLabel(usage));
  if (!hasPath) {
    ImGui::TextColored(required ? ImVec4(1.0f, 0.55f, 0.2f, 1.0f)
                                : ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                       required ? "Missing path" : "Optional");
  } else if (!exists) {
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f), "File not found");
  } else if (tex == 0) {
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Failed to load");
  } else {
    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "Loaded");
  }
  if (lossy && usage == TextureUsage::Data) {
    ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.25f, 1.0f),
                       "JPG may damage data precision");
  } else if (lossy && usage == TextureUsage::Color) {
    ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.55f, 1.0f),
                       "JPG is acceptable but lossy");
  }
  ImGui::EndGroup();
  ImGui::PopID();
}

static void applySsaoQualityPreset(PostProcessor &pp, int quality) {
  switch (quality) {
  case 0: // Low
    pp.ssaoSamples = 8;
    pp.ssaoScale = 0.40f;
    pp.ssaoRadius = 0.45f;
    pp.ssaoBias = 0.025f;
    pp.ssaoPower = 1.05f;
    break;
  case 1: // Medium
    pp.ssaoSamples = 16;
    pp.ssaoScale = 0.50f;
    pp.ssaoRadius = 0.60f;
    pp.ssaoBias = 0.020f;
    pp.ssaoPower = 1.15f;
    break;
  case 2: // High
    pp.ssaoSamples = 24;
    pp.ssaoScale = 0.65f;
    pp.ssaoRadius = 0.70f;
    pp.ssaoBias = 0.018f;
    pp.ssaoPower = 1.20f;
    break;
  case 3: // Ultra
    pp.ssaoSamples = 48;
    pp.ssaoScale = 0.80f;
    pp.ssaoRadius = 0.90f;
    pp.ssaoBias = 0.015f;
    pp.ssaoPower = 1.35f;
    break;
  default:
    break;
  }
}

static void applyVolumetricQualityPreset(PostProcessor &pp, int quality) {
  switch (quality) {
  case 0: // Low
    pp.volumetricSamples = 8;
    pp.volumetricScale = 0.35f;
    pp.volumetricFogDensity = 0.012f;
    pp.volumetricLightExposure = 0.20f;
    pp.volumetricLightWeight = 0.065f;
    pp.volumetricLightDecay = 0.93f;
    break;
  case 1: // Medium
    pp.volumetricSamples = 16;
    pp.volumetricScale = 0.50f;
    pp.volumetricFogDensity = 0.018f;
    pp.volumetricLightExposure = 0.28f;
    pp.volumetricLightWeight = 0.095f;
    pp.volumetricLightDecay = 0.95f;
    break;
  case 2: // High
    pp.volumetricSamples = 24;
    pp.volumetricScale = 0.65f;
    pp.volumetricFogDensity = 0.020f;
    pp.volumetricLightExposure = 0.34f;
    pp.volumetricLightWeight = 0.110f;
    pp.volumetricLightDecay = 0.955f;
    break;
  case 3: // Ultra
    pp.volumetricSamples = 32;
    pp.volumetricScale = 0.80f;
    pp.volumetricFogDensity = 0.022f;
    pp.volumetricLightExposure = 0.40f;
    pp.volumetricLightWeight = 0.125f;
    pp.volumetricLightDecay = 0.96f;
    break;
  default:
    break;
  }
}

static void drawGraphicsStatusRow(const char *label, bool active,
                                  const char *state,
                                  const char *detail = nullptr) {
  const ImVec4 color =
      active ? ImVec4(0.35f, 0.85f, 0.45f, 1.0f)
             : ImVec4(1.0f, 0.62f, 0.24f, 1.0f);
  ImGui::TextDisabled("%s", label);
  ImGui::SameLine(170.0f);
  ImGui::TextColored(color, "%s", state);
  if (detail && detail[0] != '\0') {
    ImGui::SameLine();
    ImGui::TextDisabled("- %s", detail);
  }
}

static BlackHolePresetDefaults captureBlackHoleDefaults(
    const EditorContext &ctx) {
  BlackHolePresetDefaults bh;
  bh.valid = true;
  bh.worldMode = ctx.blackHoleWorldMode;
  bh.azimuth = ctx.blackHoleAzimuth;
  bh.elevation = ctx.blackHoleElevation;
  bh.worldPosition = {ctx.blackHoleWorldPosition.x, ctx.blackHoleWorldPosition.y,
                      ctx.blackHoleWorldPosition.z};
  bh.worldRadius = ctx.blackHoleWorldRadius;
  bh.viewPitchDeg = ctx.blackHoleViewPitchDeg;
  bh.sizeDeg = ctx.blackHoleSizeDeg;
  bh.diskTiltDeg = ctx.blackHoleDiskTiltDeg;
  bh.diskInclinationDeg = ctx.blackHoleDiskInclinationDeg;
  bh.color = {ctx.blackHoleColor.r, ctx.blackHoleColor.g,
              ctx.blackHoleColor.b};
  bh.ringIntensity = ctx.blackHoleRingIntensity;
  bh.ringWidth = ctx.blackHoleRingWidth;
  bh.distortion = ctx.blackHoleDistortion;
  bh.haloIntensity = ctx.blackHoleHaloIntensity;
  bh.diskSpinSpeed = ctx.blackHoleDiskSpinSpeed;
  bh.diskFlowShear = ctx.blackHoleDiskFlowShear;
  bh.diskTurbulence = ctx.blackHoleDiskTurbulence;
  bh.chromaticAberration = ctx.blackHoleChromaticAberration;
  bh.eclipseStrength = ctx.blackHoleEclipseStrength;
  bh.photonRingIntensity = ctx.blackHolePhotonRingIntensity;
  bh.dopplerBoost = ctx.blackHoleDopplerBoost;
  bh.jetIntensity = ctx.blackHoleJetIntensity;
  bh.coronaIntensity = ctx.blackHoleCoronaIntensity;
  bh.starLensIntensity = ctx.blackHoleStarLensIntensity;
  bh.shadowStrength = ctx.blackHoleShadowStrength;
  bh.innerDiskRadius = ctx.blackHoleInnerDiskRadius;
  bh.outerDiskRadius = ctx.blackHoleOuterDiskRadius;
  bh.diskTemperature = ctx.blackHoleDiskTemperature;
  bh.diskDensity = ctx.blackHoleDiskDensity;
  bh.lensingStrength = ctx.blackHoleLensingStrength;
  bh.backgroundStarIntensity = ctx.blackHoleBackgroundStarIntensity;
  bh.exposure = ctx.blackHoleExposure;
  bh.quality = ctx.blackHoleQuality;
  return bh;
}

static void applyBlackHoleDefaults(EditorContext &ctx,
                                   const BlackHolePresetDefaults &bh) {
  ctx.blackHoleAzimuth = bh.azimuth;
  ctx.blackHoleElevation = bh.elevation;
  ctx.blackHoleWorldMode = bh.worldMode;
  ctx.blackHoleWorldPosition =
      glm::vec3(bh.worldPosition[0], bh.worldPosition[1],
                bh.worldPosition[2]);
  ctx.blackHoleWorldRadius = bh.worldRadius;
  ctx.blackHoleViewPitchDeg = bh.viewPitchDeg;
  ctx.blackHoleSizeDeg = bh.sizeDeg;
  ctx.blackHoleDiskTiltDeg = bh.diskTiltDeg;
  ctx.blackHoleDiskInclinationDeg = bh.diskInclinationDeg;
  ctx.blackHoleColor = glm::vec3(bh.color[0], bh.color[1], bh.color[2]);
  ctx.blackHoleRingIntensity = bh.ringIntensity;
  ctx.blackHoleRingWidth = bh.ringWidth;
  ctx.blackHoleDistortion = bh.distortion;
  ctx.blackHoleHaloIntensity = bh.haloIntensity;
  ctx.blackHoleDiskSpinSpeed = bh.diskSpinSpeed;
  ctx.blackHoleDiskFlowShear = bh.diskFlowShear;
  ctx.blackHoleDiskTurbulence = bh.diskTurbulence;
  ctx.blackHoleChromaticAberration = bh.chromaticAberration;
  ctx.blackHoleEclipseStrength = bh.eclipseStrength;
  ctx.blackHolePhotonRingIntensity = bh.photonRingIntensity;
  ctx.blackHoleDopplerBoost = bh.dopplerBoost;
  ctx.blackHoleJetIntensity = bh.jetIntensity;
  ctx.blackHoleCoronaIntensity = bh.coronaIntensity;
  ctx.blackHoleStarLensIntensity = bh.starLensIntensity;
  ctx.blackHoleShadowStrength = bh.shadowStrength;
  ctx.blackHoleInnerDiskRadius = bh.innerDiskRadius;
  ctx.blackHoleOuterDiskRadius = bh.outerDiskRadius;
  ctx.blackHoleDiskTemperature = bh.diskTemperature;
  ctx.blackHoleDiskDensity = bh.diskDensity;
  ctx.blackHoleLensingStrength = bh.lensingStrength;
  ctx.blackHoleBackgroundStarIntensity = bh.backgroundStarIntensity;
  ctx.blackHoleExposure = bh.exposure;
  ctx.blackHoleQuality = bh.quality;
}

static void applyValheimAmbientPreset(EditorContext &ctx, int preset) {
  ctx.ambientHemisphereEnabled = true;
  ctx.toonEnabled = false;
  ctx.ambientRampEnabled = false;

  if (preset == 0) {
    ctx.sun.ambientStrength = 0.48f;
    ctx.ambientHemisphereIntensity = 1.08f;
    ctx.ambientSkyInfluence = 0.58f;
    ctx.ambientHorizonStrength = 0.50f;
    ctx.ambientTerrainBoost = 1.18f;
    ctx.ambientSkyColor = glm::vec3(0.66f, 0.78f, 0.90f);
    ctx.ambientHorizonColor = glm::vec3(0.58f, 0.64f, 0.50f);
    ctx.ambientGroundColor = glm::vec3(0.20f, 0.24f, 0.16f);
    ctx.fogDensity = 0.0013f;
    ctx.fogColor = glm::vec3(0.58f, 0.66f, 0.70f);
    ctx.aerialPerspectiveEnabled = true;
    ctx.aerialPerspectiveDensity = 0.0012f;
    ctx.aerialPerspectiveStart = 50.0f;
    ctx.aerialPerspectiveHeightFalloff = 0.0060f;
    ctx.aerialPerspectiveSkyBlend = 0.70f;
    ctx.aerialPerspectiveSunGlow = 0.30f;
    ctx.aerialPerspectiveDesaturation = 0.25f;
  } else if (preset == 1) {
    ctx.sun.ambientStrength = 0.36f;
    ctx.ambientHemisphereIntensity = 1.00f;
    ctx.ambientSkyInfluence = 0.42f;
    ctx.ambientHorizonStrength = 0.62f;
    ctx.ambientTerrainBoost = 1.24f;
    ctx.ambientSkyColor = glm::vec3(0.36f, 0.48f, 0.60f);
    ctx.ambientHorizonColor = glm::vec3(0.22f, 0.30f, 0.22f);
    ctx.ambientGroundColor = glm::vec3(0.07f, 0.10f, 0.07f);
    ctx.fogDensity = 0.0020f;
    ctx.fogColor = glm::vec3(0.28f, 0.36f, 0.36f);
    ctx.aerialPerspectiveEnabled = true;
    ctx.aerialPerspectiveDensity = 0.0018f;
    ctx.aerialPerspectiveStart = 35.0f;
    ctx.aerialPerspectiveHeightFalloff = 0.0050f;
    ctx.aerialPerspectiveSkyBlend = 0.80f;
    ctx.aerialPerspectiveSunGlow = 0.22f;
    ctx.aerialPerspectiveDesaturation = 0.36f;
  } else if (preset == 2) {
    ctx.sun.ambientStrength = 0.34f;
    ctx.ambientHemisphereIntensity = 1.08f;
    ctx.ambientSkyInfluence = 0.35f;
    ctx.ambientHorizonStrength = 0.78f;
    ctx.ambientTerrainBoost = 1.28f;
    ctx.ambientSkyColor = glm::vec3(0.34f, 0.40f, 0.38f);
    ctx.ambientHorizonColor = glm::vec3(0.30f, 0.34f, 0.22f);
    ctx.ambientGroundColor = glm::vec3(0.08f, 0.10f, 0.06f);
    ctx.fogDensity = 0.0032f;
    ctx.fogColor = glm::vec3(0.30f, 0.36f, 0.30f);
    ctx.aerialPerspectiveEnabled = true;
    ctx.aerialPerspectiveDensity = 0.0026f;
    ctx.aerialPerspectiveStart = 20.0f;
    ctx.aerialPerspectiveHeightFalloff = 0.0030f;
    ctx.aerialPerspectiveSkyBlend = 0.86f;
    ctx.aerialPerspectiveSunGlow = 0.15f;
    ctx.aerialPerspectiveDesaturation = 0.45f;
  } else {
    ctx.sun.ambientStrength = 0.42f;
    ctx.ambientHemisphereIntensity = 0.92f;
    ctx.ambientSkyInfluence = 0.72f;
    ctx.ambientHorizonStrength = 0.36f;
    ctx.ambientTerrainBoost = 1.10f;
    ctx.ambientSkyColor = glm::vec3(0.60f, 0.72f, 0.90f);
    ctx.ambientHorizonColor = glm::vec3(0.72f, 0.78f, 0.82f);
    ctx.ambientGroundColor = glm::vec3(0.18f, 0.20f, 0.22f);
    ctx.fogDensity = 0.0010f;
    ctx.fogColor = glm::vec3(0.66f, 0.70f, 0.76f);
    ctx.aerialPerspectiveEnabled = true;
    ctx.aerialPerspectiveDensity = 0.0008f;
    ctx.aerialPerspectiveStart = 100.0f;
    ctx.aerialPerspectiveHeightFalloff = 0.0100f;
    ctx.aerialPerspectiveSkyBlend = 0.60f;
    ctx.aerialPerspectiveSunGlow = 0.25f;
    ctx.aerialPerspectiveDesaturation = 0.18f;
  }
}

static void applyHdrRealisticGraphicsPreset(EditorContext &ctx) {
  ctx.disableHDR = true;
  ctx.disableClouds = false;
  ctx.disableShadows = false;
  ctx.shadowCameraCulling = true;
  ctx.shadowStrength = 1.15f;
  ctx.shadowFarPlane = 320.0f;
  ctx.enableCascadedShadows = true;
  ctx.shadowCascadeCount = 4;
  ctx.shadowMapResolution = 2048;
  ctx.shadowCascadeDistance = 760.0f;
  ctx.shadowCascadeLambda = 0.68f;
  ctx.shadowNormalBias = 0.030f;
  ctx.shadowDepthBias = 0.0014f;
  ctx.shadowSoftness = 1.0f;
  ctx.showShadowCascades = false;
  ctx.shadowUpdateInterval = 1;
  ctx.shadowUpdateDistance = 0.35f;
  ctx.shadowUpdateAngle = 1.4f;

  ctx.exposure = 1.12f;
  ctx.gamma = 2.2f;
  ctx.fogDensity = 0.0014f;
  ctx.fogHeightFalloff = 0.018f;
  ctx.fogColor = glm::vec3(0.58f, 0.66f, 0.76f);
  ctx.aerialPerspectiveEnabled = true;
  ctx.aerialPerspectiveDensity = 0.00145f;
  ctx.aerialPerspectiveStart = 45.0f;
  ctx.aerialPerspectiveHeightFalloff = 0.0055f;
  ctx.aerialPerspectiveSkyBlend = 0.76f;
  ctx.aerialPerspectiveSunGlow = 0.42f;
  ctx.aerialPerspectiveDesaturation = 0.38f;
  ctx.ambientHemisphereEnabled = true;
  ctx.ambientHemisphereIntensity = 1.05f;
  ctx.ambientSkyInfluence = 0.65f;
  ctx.ambientHorizonStrength = 0.45f;
  ctx.ambientTerrainBoost = 1.15f;
  ctx.ambientSkyColor = glm::vec3(0.62f, 0.74f, 0.88f);
  ctx.ambientHorizonColor = glm::vec3(0.50f, 0.58f, 0.48f);
  ctx.ambientGroundColor = glm::vec3(0.18f, 0.21f, 0.16f);

  ctx.sun.lightIntensity = 2.15f;
  ctx.sun.ambientStrength = 0.42f;
  ctx.sun.sunSize = 0.58f;
  ctx.visualSunColor = glm::vec3(1.0f, 0.86f, 0.58f);
  ctx.visualSunDayColor = glm::vec3(1.0f, 0.88f, 0.62f);
  ctx.visualSunDuskColor = glm::vec3(1.0f, 0.48f, 0.18f);
  ctx.visualSunNightColor = glm::vec3(0.16f, 0.22f, 0.45f);
  ctx.skyAtmosphereStrength = 0.28f;
  ctx.skyGradientPower = 1.15f;
  ctx.skyHorizonGlow = 0.16f;
  ctx.skySunDiscIntensity = 14.0f;
  ctx.skySunHaloIntensity = 0.45f;
  ctx.skySunRaysIntensity = 0.12f;
  ctx.skySunDiscSoftness = 0.0014f;
  ctx.skySunHaloSize = 0.18f;
  ctx.skySunRaySharpness = 12.0f;
  ctx.sky.skyCloudsEnabled = true;
  ctx.sky.skyCloudScale = 2.0f;
  ctx.sky.skyCloudCoverage = 0.48f;
  ctx.sky.skyCloudDensity = 0.74f;
  ctx.sky.skyCloudSoftness = 0.22f;
  ctx.sky.skyCloudSpeed = 0.006f;
  ctx.cloud.enabled = false;
  ctx.cloud.cover = 0.50f;
  ctx.cloud.density = 0.72f;
  ctx.cloud.alpha = 0.28f;

  ctx.toonEnabled = false;
  ctx.shadowBandEnabled = false;
  ctx.ambientRampEnabled = false;
  ctx.rimEnabled = false;

  PostProcessor &pp = ctx.postProcessor;
  pp.brightness = 1.0f;
  pp.bloomThreshold = 0.72f;
  pp.bloomIntensity = 0.85f;
  pp.bloomScale = 0.55f;
  pp.blurIterations = 7;
  pp.enableSSAO = true;
  pp.ssaoQuality = 2;
  applySsaoQualityPreset(pp, pp.ssaoQuality);
  pp.ssaoIntensity = 1.25f;
  pp.enableVolumetricFog = true;
  pp.volumetricQuality = 2;
  applyVolumetricQualityPreset(pp, pp.volumetricQuality);
  pp.enableTAA = true;
  pp.taaHistoryBlend = 0.86f;
  pp.taaMotionReset = 0.35f;
  pp.enableFXAA = true;
  pp.enableAutoExposure = true;
  pp.autoExposureMin = 0.72f;
  pp.autoExposureMax = 1.55f;
  pp.autoExposureSpeed = 0.08f;
  pp.autoExposureTarget = 0.44f;
  pp.enableColorGrade = true;
  pp.gradeSaturation = 1.08f;
  pp.gradeContrast = 1.06f;
  pp.gradeLift = -0.015f;
  pp.gradeGamma = 1.0f;
  pp.gradeGain = 1.03f;
  pp.gradeTint = glm::vec3(1.03f, 1.0f, 0.96f);
  pp.enablePaletteQuantize = false;
  pp.enableDistanceTint = false;
}

static void applyBalancedPerformanceGraphicsPreset(EditorContext &ctx) {
  ctx.disableHDR = true;
  ctx.disableClouds = true;
  ctx.disableShadows = false;
  ctx.shadowCameraCulling = true;
  ctx.shadowStrength = 1.0f;
  ctx.shadowFarPlane = 260.0f;
  ctx.enableCascadedShadows = true;
  ctx.shadowCascadeCount = 3;
  ctx.shadowMapResolution = 1536;
  ctx.shadowCascadeDistance = 520.0f;
  ctx.shadowCascadeLambda = 0.62f;
  ctx.shadowNormalBias = 0.036f;
  ctx.shadowDepthBias = 0.0018f;
  ctx.shadowSoftness = 1.15f;
  ctx.showShadowCascades = false;
  ctx.shadowUpdateInterval = 2;
  ctx.shadowUpdateDistance = 1.0f;
  ctx.shadowUpdateAngle = 3.0f;

  ctx.exposure = 1.0f;
  ctx.gamma = 2.2f;
  ctx.fogDensity = 0.0009f;
  ctx.fogHeightFalloff = 0.02f;
  ctx.aerialPerspectiveEnabled = true;
  ctx.aerialPerspectiveDensity = 0.00110f;
  ctx.aerialPerspectiveStart = 70.0f;
  ctx.aerialPerspectiveHeightFalloff = 0.0065f;
  ctx.aerialPerspectiveSkyBlend = 0.68f;
  ctx.aerialPerspectiveSunGlow = 0.30f;
  ctx.aerialPerspectiveDesaturation = 0.30f;
  ctx.ambientHemisphereEnabled = true;
  ctx.ambientHemisphereIntensity = 0.95f;
  ctx.ambientSkyInfluence = 0.60f;
  ctx.ambientHorizonStrength = 0.38f;
  ctx.ambientTerrainBoost = 1.08f;
  ctx.ambientSkyColor = glm::vec3(0.58f, 0.70f, 0.84f);
  ctx.ambientHorizonColor = glm::vec3(0.46f, 0.54f, 0.46f);
  ctx.ambientGroundColor = glm::vec3(0.16f, 0.19f, 0.14f);

  ctx.sun.lightIntensity = 1.8f;
  ctx.sun.ambientStrength = 0.38f;
  ctx.visualSunColor = glm::vec3(1.0f, 0.86f, 0.58f);
  ctx.visualSunDayColor = glm::vec3(1.0f, 0.88f, 0.62f);
  ctx.visualSunDuskColor = glm::vec3(1.0f, 0.50f, 0.22f);
  ctx.visualSunNightColor = glm::vec3(0.14f, 0.20f, 0.42f);
  ctx.skyAtmosphereStrength = 0.35f;
  ctx.skyGradientPower = 1.05f;
  ctx.skyHorizonGlow = 0.16f;
  ctx.skySunDiscIntensity = 10.0f;
  ctx.skySunHaloIntensity = 0.55f;
  ctx.skySunRaysIntensity = 0.18f;
  ctx.skySunDiscSoftness = 0.0020f;
  ctx.skySunHaloSize = 0.28f;
  ctx.skySunRaySharpness = 10.0f;
  ctx.toonEnabled = false;
  ctx.shadowBandEnabled = false;
  ctx.ambientRampEnabled = false;
  ctx.rimEnabled = false;

  PostProcessor &pp = ctx.postProcessor;
  pp.brightness = 1.0f;
  pp.bloomThreshold = 0.9f;
  pp.bloomIntensity = 0.55f;
  pp.bloomScale = 0.45f;
  pp.blurIterations = 5;
  pp.enableSSAO = true;
  pp.ssaoQuality = 1;
  applySsaoQualityPreset(pp, pp.ssaoQuality);
  pp.ssaoIntensity = 0.9f;
  pp.enableVolumetricFog = false;
  pp.enableTAA = false;
  pp.enableFXAA = true;
  pp.enableAutoExposure = true;
  pp.autoExposureMin = 0.78f;
  pp.autoExposureMax = 1.35f;
  pp.autoExposureSpeed = 0.08f;
  pp.autoExposureTarget = 0.45f;
  pp.enableColorGrade = false;
  pp.enablePaletteQuantize = false;
  pp.enableDistanceTint = false;
}

static void applyTerrainGeneratorDefaults(TerrainSettings &s) {
  s.seed = 42;
  s.chunkSize = 32;
  s.heightScale = 10.0f;
  s.noiseFrequency = 0.02f;
  s.landscapeScale = 2.2f;
  s.macroStrength = 1.35f;
  s.mountainSpan = 1.4f;
  s.valleySpan = 1.25f;
  s.viewDistance = 6;
  s.chunkWorldSize = 64.0f;
  s.maxConcurrentChunkJobs = 10;
  s.maxChunkLoadsPerUpdate = 24;
  s.maxChunkUploadsPerFrame = 2;
  s.collisionChunkRadius = 1;
  s.useGpuTerrain = true;
  s.enableHorizonCulling = true;
  s.horizonCullingSectors = 256;
  s.terrainGpuPageCapacity = 1024;
  s.terrainWorkerThreads = 4;
  s.maxCompletedChunksPerFrame = 4;
  s.maxGpuUploadBytesPerFrame = 8 * 1024 * 1024;
  s.collisionUpdatesPerFrame = 2;
  s.meshVegetationDistance = 220;
  s.impostorVegetationDistance = 1200;
  s.vegetationShadowDistance = 260;
  s.materialQuality = TerrainMaterialQuality::Balanced;
  s.useRidgeNoise = false;
  s.singleBiomeOnly = true;
  s.octaves = 5;
  s.lacunarity = 2.0f;
  s.gain = 0.5f;
  s.treeDensity = 0.15f;
  s.interactiveTreeChunkRadius = 1;
  s.interactiveTreeRatio = 0.22f;
  s.maxInteractiveTreesPerChunk = 28;
  s.biomeScale = 0.004f;
  s.seaLevel = -2.0f;
  s.rockDensity = 0.34f;
  s.grassDensity = 0.25f;
  s.rockScale = 1.2f;
  s.grassScale = 1.0f;
  s.spawnWater = true;
  s.spawnRocks = true;
  s.spawnVegetation = true;
}

int EditorUI::consoleInputCallback(ImGuiInputTextCallbackData *data) {
  if (!data || !data->UserData)
    return 0;
  EditorUI *ui = reinterpret_cast<EditorUI *>(data->UserData);
  if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
    const int historySize = (int)ui->mConsoleHistory.size();
    if (historySize == 0)
      return 0;
    if (data->EventKey == ImGuiKey_UpArrow) {
      if (ui->mConsoleHistoryPos < 0)
        ui->mConsoleHistoryPos = historySize - 1;
      else if (ui->mConsoleHistoryPos > 0)
        ui->mConsoleHistoryPos--;
    } else if (data->EventKey == ImGuiKey_DownArrow) {
      if (ui->mConsoleHistoryPos >= 0) {
        ui->mConsoleHistoryPos++;
        if (ui->mConsoleHistoryPos >= historySize)
          ui->mConsoleHistoryPos = -1;
      }
    }

    if (ui->mConsoleHistoryPos >= 0) {
      const std::string &history = ui->mConsoleHistory[ui->mConsoleHistoryPos];
      data->DeleteChars(0, data->BufTextLen);
      data->InsertChars(0, history.c_str());
    } else {
      data->DeleteChars(0, data->BufTextLen);
    }
  }
  return 0;
}

// =============================================================================
// Main Editor Panel
// =============================================================================
EditorUIOutput EditorUI::draw(EditorContext &ctx) {
  EditorUIOutput out{};

  if (!ctx.uiMode)
    return out;

  const bool minimalUI = (ctx.playState == 1);

  ImGuiIO &io = ImGui::GetIO();

  // Shortcuts
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
    ctx.events.publish(UndoRequestedEvent{});
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
    ctx.events.publish(RedoRequestedEvent{});

  // Main Menu
  drawMainMenuBar(ctx);

  // ── Toolbar ────────────────────────────────────────────────────────────────
  if (!minimalUI) {
    // Sync toolbar state FROM context (in case external code changed it)
    if (ctx.selection.gizmoOp == ImGuizmo::TRANSLATE) {
      toolbarState.gizmoOp = ToolbarState::Translate;
    } else if (ctx.selection.gizmoOp == ImGuizmo::ROTATE) {
      toolbarState.gizmoOp = ToolbarState::Rotate;
    } else if (ctx.selection.gizmoOp == ImGuizmo::SCALE) {
      toolbarState.gizmoOp = ToolbarState::Scale;
    }

    toolbarState.shadingMode =
        ctx.wireframe ? ToolbarState::Wireframe : ToolbarState::Textured;

    EditorToolbar::draw(toolbarState);
    EditorToolbar::processShortcuts(toolbarState);

    // Sync toolbar state BACK to context
    if (toolbarState.gizmoOp == ToolbarState::Translate) {
      ctx.selection.gizmoOp = ImGuizmo::TRANSLATE;
    } else if (toolbarState.gizmoOp == ToolbarState::Rotate) {
      ctx.selection.gizmoOp = ImGuizmo::ROTATE;
    } else if (toolbarState.gizmoOp == ToolbarState::Scale) {
      ctx.selection.gizmoOp = ImGuizmo::SCALE;
    }

    ctx.wireframe = (toolbarState.shadingMode == ToolbarState::Wireframe);
  }
#ifdef IMGUI_HAS_DOCK
  ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::SetNextWindowViewport(viewport->ID);

  ImGuiWindowFlags host_window_flags = 0;
  host_window_flags |= ImGuiWindowFlags_NoTitleBar |
                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove;
  host_window_flags |=
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
  host_window_flags |= ImGuiWindowFlags_NoBackground;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

  ImGui::Begin("MainDockSpaceHost", nullptr, host_window_flags);

  // If the host window is hovered, it means the mouse is over the empty
  // viewport area and NOT over any docked panels. We record this to bypass
  // ImGui's mouse capture!
  bool isViewportHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

  ImGui::PopStyleVar(3);

  ImGuiID dockspaceId = ImGui::GetID("glGenDockspace");

  // Build default layout on first run or reset
  if (mResetLayout || ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImVec2 workSize = ImGui::GetMainViewport()->WorkSize;
    ImGui::DockBuilderSetNodeSize(dockspaceId, workSize);

    // Layout: Hierarchy top-left, Inspector below it, Assets bottom-center,
    // World right.
    ImGuiID dockLeft, dockCenter, dockRight;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.22f, &dockLeft,
                                &dockCenter);
    ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.26f, &dockRight,
                                &dockCenter);

    ImGuiID dockLeftBottom;
    ImGui::DockBuilderSplitNode(dockLeft, ImGuiDir_Down, 0.45f, &dockLeftBottom,
                                &dockLeft);

    ImGuiID dockBottom;
    ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, 0.30f, &dockBottom,
                                &dockCenter);

    ImGui::DockBuilderDockWindow("Hierarchy", dockLeft);
    ImGui::DockBuilderDockWindow("Inspector", dockLeftBottom);
    ImGui::DockBuilderDockWindow("World", dockRight);
    ImGui::DockBuilderDockWindow("Assets", dockBottom);
    ImGui::DockBuilderDockWindow("Statistics", dockBottom);
    ImGui::DockBuilderDockWindow("Profiler", dockBottom);

    ImGui::DockBuilderFinish(dockspaceId);

    // Ensure key panels are visible
    mShowHierarchy = true;
    mShowInspector = true;
    mShowAssets = true;
    mShowEnvironment = true;
    mShowStats = false;
    mShowLog = true;
    mShowProfiler = true;
    mShowScriptEditor = false;
    mResetLayout = false;
  }

  // Activate dockspace every frame
  ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f),
                   ImGuiDockNodeFlags_PassthruCentralNode);

  // Draw Crosshair if playing
  if (ctx.playState == 1) { // 1 = Playing
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    float size = 10.0f;
    float thickness = 2.0f;
    ImU32 color = IM_COL32(255, 255, 255, 200); // White with alpha

    drawList->AddLine(ImVec2(center.x - size, center.y),
                      ImVec2(center.x + size, center.y), color, thickness);
    drawList->AddLine(ImVec2(center.x, center.y - size),
                      ImVec2(center.x, center.y + size), color, thickness);
  }

  ImGui::End();
#endif

  // ── Draw Panels ────────────────────────────────────────────────────────────
  const ImGuiWindowFlags panelFlags =
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove;

  if (mShowLog) {
    mShowAssets = true;
  }

  if (!minimalUI && mShowHierarchy) {
    ImGui::SetNextWindowSize(ImVec2(280, 400), ImGuiCond_FirstUseEver);
    drawHierarchy(ctx);
  }
  if (!minimalUI && mShowInspector) {
    ImGui::SetNextWindowSize(ImVec2(320, 500), ImGuiCond_FirstUseEver);
    out.sceneModified |= drawInspector(ctx);
  }
  if (!minimalUI && mShowAssets) {
    ImGui::SetNextWindowSize(ImVec2(600, 250), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse;
    if (mLockLayout)
      wf |= ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("Assets", &mShowAssets, wf)) {
      if (ImGui::BeginTabBar("AssetsConsoleTabs")) {
        if (ImGui::BeginTabItem("Assets")) {
          drawAssetsContent(ctx);
          ImGui::EndTabItem();
        }
        if (mShowLog && ImGui::BeginTabItem("Console")) {
          drawLog(ctx);
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      } else {
        drawAssets(ctx);
      }
    }
    ImGui::End();
  }
  if (!minimalUI && mShowEnvironment) {
    ImGui::SetNextWindowSize(ImVec2(320, 250), ImGuiCond_FirstUseEver);
    drawEnvironment(ctx);
  }
  if (!minimalUI && mShowStats) {
    ImGui::SetNextWindowSize(ImVec2(250, 200), ImGuiCond_FirstUseEver);
    drawStats(ctx);
  }
  if (!minimalUI && mShowProfiler) {
    ImGui::SetNextWindowSize(ImVec2(300, 220), ImGuiCond_FirstUseEver);
    drawProfiler(ctx);
  }
  if (!minimalUI && mShowScriptEditor) {
    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
    drawScriptEditor(ctx);
  }

  if (!mPendingConsoleCommands.empty()) {
    out.consoleCommands = std::move(mPendingConsoleCommands);
    mPendingConsoleCommands.clear();
  }

  // Capture IO state
  out.wantCaptureMouse = io.WantCaptureMouse;
#ifdef IMGUI_HAS_DOCK
  if (isViewportHovered) {
    out.wantCaptureMouse = false;
  }
#endif
  out.wantCaptureKeyboard = io.WantCaptureKeyboard;

  return out;
}

void EditorUI::drawMainMenuBar(EditorContext &ctx) {
  if (ImGui::BeginMainMenuBar()) {
    const bool minimalUI = (ctx.playState == 1);
    if (!minimalUI) {
      if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
          const char *path = tinyfd_saveFileDialog("Save Scene", "scene.json",
                                                   0, NULL, "JSON Scene File");
          if (path) {
            ctx.events.publish(SaveSceneRequestedEvent{path});
          }
        }
        if (ImGui::MenuItem("Load Scene", "Ctrl+O")) {
          const char *path = tinyfd_openFileDialog("Load Scene", "", 0, NULL,
                                                   "JSON Scene File", 0);
          if (path) {
            ctx.events.publish(LoadSceneRequestedEvent{path});
            ctx.selection.selectedEntityId =
                0; // Clear selection on load to avoid dangling pointers
          }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save Project Settings"))
          ctx.events.publish(SaveProjectConfigRequestedEvent{});
        if (ImGui::MenuItem("Save Current Settings As Defaults")) {
          ctx.projectConfig.blackHoleDefaults = captureBlackHoleDefaults(ctx);
          ctx.events.publish(SaveProjectConfigRequestedEvent{});
          ctx.events.publish(SaveProjectDefaultsRequestedEvent{});
        }
        if (ImGui::MenuItem("Reset Saved Settings Defaults")) {
          ctx.projectConfig.blackHoleDefaults = BlackHolePresetDefaults{};
          ctx.events.publish(SaveProjectConfigRequestedEvent{});
          ctx.events.publish(ResetProjectDefaultsRequestedEvent{});
        }
        if (ImGui::MenuItem("Set Startup Scene...")) {
          const char *path = tinyfd_openFileDialog(
              "Set Startup Scene", "", 0, NULL, "JSON Scene File", 0);
          if (path) {
            ctx.projectConfig.startupScene = path;
            ctx.events.publish(SaveProjectConfigRequestedEvent{});
          }
        }
        if (!ctx.projectConfig.startupScene.empty()) {
          ImGui::TextDisabled("Startup Scene:");
          ImGui::TextWrapped("%s", ctx.projectConfig.startupScene.c_str());
          if (ImGui::MenuItem("Clear Startup Scene")) {
            ctx.projectConfig.startupScene.clear();
            ctx.events.publish(SaveProjectConfigRequestedEvent{});
          }
        }
        if (ImGui::MenuItem("Save Layout"))
          ImGui::SaveIniSettingsToDisk("imgui.ini");
        if (ImGui::MenuItem("Load Layout"))
          ImGui::LoadIniSettingsFromDisk("imgui.ini");
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4")) {
          // Request app exit via GLFW (if window available)
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z"))
          ctx.events.publish(UndoRequestedEvent{});
        if (ImGui::MenuItem("Redo", "Ctrl+Y"))
          ctx.events.publish(RedoRequestedEvent{});
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
          if (ctx.selection.selectedEntityId != 0)
            ctx.events.publish(
                DuplicateEntityRequestedEvent{ctx.selection.selectedEntityId});
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Entity")) {
        if (ImGui::MenuItem("Create Empty")) {
          ctx.events.publish(CreateEmptyEntityRequestedEvent{});
        }
        if (ImGui::BeginMenu("Create Primitive")) {
          if (ImGui::MenuItem("Cube"))
            ctx.events.publish(SpawnEntityRequestedEvent{"__primitive_cube"});
          if (ImGui::MenuItem("Sphere"))
            ctx.events.publish(SpawnEntityRequestedEvent{"__primitive_sphere"});
          if (ImGui::MenuItem("Plane"))
            ctx.events.publish(SpawnEntityRequestedEvent{"__primitive_plane"});
          if (ImGui::MenuItem("Cylinder"))
            ctx.events.publish(
                SpawnEntityRequestedEvent{"__primitive_cylinder"});
          if (ImGui::MenuItem("Cone"))
            ctx.events.publish(SpawnEntityRequestedEvent{"__primitive_cone"});
          ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Create Spaceship"))
          ctx.events.publish(SpawnEntityRequestedEvent{"__spaceship"});
        ImGui::Separator();
        if (ImGui::MenuItem("Delete Selected", "Delete")) {
          if (ctx.selection.selectedEntityId != 0)
            ctx.events.publish(
                DeleteEntityRequestedEvent{ctx.selection.selectedEntityId});
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Window")) {
        ImGui::MenuItem("Hierarchy", nullptr, &mShowHierarchy);
        ImGui::MenuItem("Inspector", nullptr, &mShowInspector);
        ImGui::MenuItem("Content Browser", nullptr, &mShowAssets);
        ImGui::MenuItem("World", nullptr, &mShowEnvironment);
        if (ImGui::MenuItem("Console", nullptr, &mShowLog)) {
          if (mShowLog)
            mShowAssets = true;
        }
        ImGui::MenuItem("Statistics", nullptr, &mShowStats);
        ImGui::MenuItem("Profiler", nullptr, &mShowProfiler);
        ImGui::Separator();
        ImGui::MenuItem("Lock Layout", nullptr, &mLockLayout);
        if (ImGui::MenuItem("Save Layout Now"))
          ImGui::SaveIniSettingsToDisk("imgui.ini");
        if (ImGui::MenuItem("Reset Layout")) {
          mResetLayout = true;
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About glGen Engine")) {
          // TODO: About popup
        }
        ImGui::EndMenu();
      }
    }

    // ── Play / Pause / Stop ──
    {
      float barW = ImGui::GetWindowWidth();
      float btnW = 60.0f;
      float totalW = btnW * 3 + ImGui::GetStyle().ItemSpacing.x * 2;
      float centerX = (barW - totalW) * 0.5f;
      if (centerX > ImGui::GetCursorPosX())
        ImGui::SameLine(centerX);
      else
        ImGui::SameLine();

      bool isStopped = (ctx.playState == 0);
      bool isPlaying = (ctx.playState == 1);
      bool isPaused = (ctx.playState == 2);

      // Play button
      if (isPlaying) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.5f, 0.1f, 1.0f));
      } else {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyle().Colors[ImGuiCol_Button]);
      }
      if (ImGui::Button("Play", ImVec2(btnW, 0))) {
        ctx.playState = 1; // Playing
      }
      ImGui::PopStyleColor();

      ImGui::SameLine();

      // Pause button
      if (isPaused) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.5f, 0.0f, 1.0f));
      } else {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyle().Colors[ImGuiCol_Button]);
      }
      if (ImGui::Button("Pause", ImVec2(btnW, 0))) {
        if (isPlaying)
          ctx.playState = 2; // Paused
        else if (isPaused)
          ctx.playState = 1; // Resume
      }
      ImGui::PopStyleColor();

      ImGui::SameLine();

      // Stop button
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
      if (ImGui::Button("Stop", ImVec2(btnW, 0))) {
        ctx.playState = 0; // Stopped
        // Reset all script components so they re-initialize on next Play
        for (auto eid : ctx.scene.registry().view<ScriptComponent>()) {
          auto &sc = ctx.scene.registry().get<ScriptComponent>(eid);
          sc.initialized = false;
        }
      }
      ImGui::PopStyleColor();

      // State indicator
      ImGui::SameLine();
      if (isPlaying)
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1), "Playing");
      else if (isPaused)
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.2f, 1), "Paused");
      else
        ImGui::TextDisabled("Stopped");

      if (isPlaying) {
        ImGui::SameLine();
        ImGui::Text("Wood: %d", ctx.gameplay.woodCount);
      }
    }

    // ── Right-aligned info ──
    if (!minimalUI) {
      float rightW = ImGui::CalcTextSize("FPS: 9999.9").x + 20;
      ImGui::SameLine(ImGui::GetWindowWidth() - rightW);
      ImGui::TextDisabled("FPS: %.1f", 1.0f / ctx.dt);
    }

    ImGui::EndMainMenuBar();
  }
}

// =============================================================================
// Hierarchy (Scene Graph)
// =============================================================================
void EditorUI::drawHierarchy(EditorContext &ctx) {
  ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse;
  if (mLockLayout)
    wf |= ImGuiWindowFlags_NoMove;
  if (ImGui::Begin("Hierarchy", &mShowHierarchy, wf)) {
    auto &s = ctx.selection;
    ImGui::InputTextWithHint("##filter", "Search...", s.outlinerFilter, 128);
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
      s.outlinerFilter[0] = 0;
    ImGui::Separator();

    auto &reg = ctx.scene.registry();

    // Filter helper
    auto passFilter = [&](const char *name) -> bool {
      if (s.outlinerFilter[0] == 0)
        return true;
      std::string a = name ? name : "";
      std::string b = s.outlinerFilter;
      for (auto &c : a)
        c = (char)tolower((unsigned char)c);
      for (auto &c : b)
        c = (char)tolower((unsigned char)c);
      return a.find(b) != std::string::npos;
    };

    auto drawEntityRow = [&](EntityId entity, const std::string &name) {
      uint32_t id = (uint32_t)entity;
      bool isSelected = false;
      for (auto selId : s.selectedEntities)
        if (selId == id)
          isSelected = true;

      ImGui::PushID((int)id);
      if (ImGui::Selectable(name.c_str(), isSelected)) {
        if (ImGui::GetIO().KeyCtrl) {
          if (isSelected) {
            s.selectedEntities.erase(std::remove(s.selectedEntities.begin(),
                                                 s.selectedEntities.end(), id),
                                     s.selectedEntities.end());
            if (s.selectedEntityId == id)
              s.selectedEntityId = 0;
          } else {
            s.selectedEntities.push_back(id);
            s.selectedEntityId = id;
          }
        } else {
          s.selectedEntities.clear();
          s.selectedEntities.push_back(id);
          s.selectedEntityId = id;
        }
        s.lastClickedEntity = id;
      }
      ImGui::PopID();
    };

    std::vector<std::pair<EntityId, std::string>> terrainEntities;
    std::vector<std::pair<EntityId, std::string>> sceneEntities;

    auto view = reg.view<TransformComponent>();
    for (auto entity : view) {
      std::string name = "Entity " + std::to_string((uint32_t)entity);
      if (reg.has<NameComponent>(entity))
        name = reg.get<NameComponent>(entity).name;

      if (!passFilter(name.c_str()))
        continue;

      bool isTerrainGroup = false;
      if (reg.has<MeshComponent>(entity)) {
        const auto &mesh = reg.get<MeshComponent>(entity);
        isTerrainGroup = mesh.isTerrain || mesh.isWater;
      }
      if (reg.has<InstancedMeshComponent>(entity))
        isTerrainGroup = true;
      if (name.rfind("prefab_", 0) == 0 || name.rfind("terrain_", 0) == 0 ||
          name.rfind("water_", 0) == 0)
        isTerrainGroup = true;

      if (isTerrainGroup)
        terrainEntities.push_back({entity, name});
      else
        sceneEntities.push_back({entity, name});
    }

    if (!terrainEntities.empty()) {
      if (ImGui::TreeNodeEx("Terrain",
                            ImGuiTreeNodeFlags_DefaultOpen |
                                ImGuiTreeNodeFlags_SpanFullWidth,
                            "Terrain (%d)", (int)terrainEntities.size())) {
        for (auto &it : terrainEntities)
          drawEntityRow(it.first, it.second);
        ImGui::TreePop();
      }
    }

    if (!sceneEntities.empty()) {
      if (ImGui::TreeNodeEx("Scene",
                            ImGuiTreeNodeFlags_DefaultOpen |
                                ImGuiTreeNodeFlags_SpanFullWidth,
                            "Scene (%d)", (int)sceneEntities.size())) {
        for (auto &it : sceneEntities)
          drawEntityRow(it.first, it.second);
        ImGui::TreePop();
      }
    }
  }
  ImGui::End();
}

// =============================================================================
// Content Browser
// =============================================================================
void EditorUI::drawAssets(EditorContext &ctx) {
  ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse;
  if (mLockLayout)
    wf |= ImGuiWindowFlags_NoMove;
  if (ImGui::Begin("Assets", &mShowAssets, wf)) {
    drawAssetsContent(ctx);
  }
  ImGui::End();
}

void EditorUI::drawAssetsContent(EditorContext &ctx) {
  if (mBrowsePath.empty())
    mBrowsePath = ctx.projectConfig.assetPath("");
  if (mBrowsePath.empty())
    mBrowsePath = "assets";

  if (ImGui::Button("Up")) {
    std::filesystem::path p = mBrowsePath;
    if (p.has_parent_path())
      mBrowsePath = p.parent_path().string();
  }
  ImGui::SameLine();
  ImGui::Text("%s", mBrowsePath.c_str());
  ImGui::SameLine();
  ImGui::SetNextItemWidth(200.0f);
  ImGui::InputTextWithHint("##AssetSearch", "Search assets...", mAssetSearch,
                           sizeof(mAssetSearch));

  static int assetFilter = 0;
  ImGui::SameLine();
  if (ImGui::SmallButton("All"))
    assetFilter = 0;
  ImGui::SameLine();
  if (ImGui::SmallButton("Models"))
    assetFilter = 1;
  ImGui::SameLine();
  if (ImGui::SmallButton("Textures"))
    assetFilter = 2;
  ImGui::SameLine();
  if (ImGui::SmallButton("Shaders"))
    assetFilter = 3;
  ImGui::SameLine();
  if (ImGui::SmallButton("Scenes"))
    assetFilter = 4;

  ImGui::Separator();

  // File list logic...
  namespace fs = std::filesystem;
  if (fs::exists(mBrowsePath) && fs::is_directory(mBrowsePath)) {
    float padding = 16.0f;
    float thumbnailSize = 64.0f;
    float cellSize = thumbnailSize + padding;
    float panelWidth = ImGui::GetContentRegionAvail().x;
    int columnCount = (int)(panelWidth / cellSize);
    if (columnCount < 1)
      columnCount = 1;

    if (ImGui::BeginTable("ContentTable", columnCount)) {
      for (auto const &dir_entry : fs::directory_iterator(mBrowsePath)) {
        const auto &path = dir_entry.path();
        std::string filename = path.filename().string();
        std::string ext = path.extension().string();
        for (auto &c : ext)
          c = (char)tolower((unsigned char)c);

        bool isDir = dir_entry.is_directory();
        bool isModel =
            (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb");
        bool isImage =
            (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".hdr");
        bool isShader = (ext == ".vert" || ext == ".frag" || ext == ".glsl");
        bool isScene = (ext == ".json" || ext == ".scene");

        if (!isDir && !isModel && !isImage)
          continue;

        if (mAssetSearch[0] != '\0') {
          std::string search = mAssetSearch;
          std::string hay = filename;
          std::transform(search.begin(), search.end(), search.begin(),
                         ::tolower);
          std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
          if (hay.find(search) == std::string::npos)
            continue;
        }

        if (!isDir && assetFilter != 0) {
          if (assetFilter == 1 && !isModel)
            continue;
          if (assetFilter == 2 && !isImage)
            continue;
          if (assetFilter == 3 && !isShader)
            continue;
          if (assetFilter == 4 && !isScene)
            continue;
        }

        ImGui::TableNextColumn();
        ImGui::PushID(filename.c_str());

        // Build a visual button using ImGui drawing API
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList *draw_list = ImGui::GetWindowDrawList();

        // Transparent button holding the space
        bool hovered = false, held = false;
        bool clicked = ImGui::InvisibleButton(
            "##AssetBtn", ImVec2(thumbnailSize, thumbnailSize));
        hovered = ImGui::IsItemHovered();
        held = ImGui::IsItemActive();

        ImVec4 colorBg = hovered ? ImVec4(0.3f, 0.3f, 0.3f, 0.5f)
                                 : ImVec4(0.2f, 0.2f, 0.2f, 0.3f);
        draw_list->AddRectFilled(
            pos, ImVec2(pos.x + thumbnailSize, pos.y + thumbnailSize),
            ImGui::ColorConvertFloat4ToU32(colorBg), 4.0f);

        // Draw custom icons based on type
        ImVec2 center =
            ImVec2(pos.x + thumbnailSize * 0.5f, pos.y + thumbnailSize * 0.35f);
        float iconR = thumbnailSize * 0.25f;

        if (isDir) {
          // Draw a Folder icon
          ImU32 colFolder =
              ImGui::ColorConvertFloat4ToU32(ImVec4(0.8f, 0.65f, 0.3f, 1.0f));
          draw_list->AddRectFilled(
              ImVec2(center.x - iconR, center.y - iconR + 4),
              ImVec2(center.x + iconR, center.y + iconR), colFolder, 2.0f);
          draw_list->AddRectFilled(
              ImVec2(center.x - iconR, center.y - iconR - 2),
              ImVec2(center.x - iconR + 10, center.y - iconR + 6), colFolder,
              2.0f);

          // Interaction logic
          if (clicked ||
              (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
            mBrowsePath = path.string();
          }
        } else if (isModel) {
          // Draw a Cube icon (Model)
          ImU32 colModel =
              ImGui::ColorConvertFloat4ToU32(ImVec4(0.4f, 0.7f, 0.9f, 1.0f));
          draw_list->AddRectFilled(ImVec2(center.x - iconR, center.y - iconR),
                                   ImVec2(center.x + iconR, center.y + iconR),
                                   colModel, 4.0f);
          draw_list->AddLine(ImVec2(center.x - iconR, center.y - iconR), center,
                             IM_COL32(0, 0, 0, 100), 2.0f);
          draw_list->AddLine(ImVec2(center.x + iconR, center.y + iconR), center,
                             IM_COL32(0, 0, 0, 100), 2.0f);
        } else if (isImage) {
          // Draw a Image icon (Picture)
          ImU32 colImg =
              ImGui::ColorConvertFloat4ToU32(ImVec4(0.5f, 0.9f, 0.5f, 1.0f));
          draw_list->AddRectFilled(ImVec2(center.x - iconR, center.y - iconR),
                                   ImVec2(center.x + iconR, center.y + iconR),
                                   colImg, 2.0f);
          draw_list->AddCircleFilled(ImVec2(center.x + 5, center.y - 5), 4.0f,
                                     IM_COL32(255, 255, 255, 255));
        }

        // Text Label
        ImVec2 textSize = ImGui::CalcTextSize(filename.c_str());
        float textX = pos.x + (thumbnailSize - textSize.x) * 0.5f;
        float textY = pos.y + thumbnailSize - textSize.y - 4.0f;
        if (textX < pos.x)
          textX = pos.x; // Keep within bounds

        // Truncate text if too long
        std::string displayTxt = filename;
        if (textSize.x > thumbnailSize) {
          displayTxt = filename.substr(0, 7) + "..";
          textSize = ImGui::CalcTextSize(displayTxt.c_str());
          textX = pos.x + (thumbnailSize - textSize.x) * 0.5f;
        }
        draw_list->AddText(
            ImVec2(textX, textY),
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.9f, 0.9f, 0.92f, 1.0f)),
            displayTxt.c_str());

        // Drag and drop for files (not dirs)
        if (!isDir &&
            ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
          std::string fullPath = path.string();
          ImGui::SetDragDropPayload("ASSET_PATH", fullPath.c_str(),
                                    fullPath.size() + 1);
          ImGui::Text("Drop %s into Scene", filename.c_str());
          ImGui::EndDragDropSource();
        }

        if (isModel && clicked && hovered) {
          ctx.events.publish(SpawnEntityRequestedEvent{path.string()});
        }

        ImGui::PopID();
      }
      ImGui::EndTable();
    }
  }
}

// =============================================================================
// Environment Settings
// =============================================================================
void EditorUI::drawEnvironment(EditorContext &ctx) {
  ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse;
  if (mLockLayout)
    wf |= ImGuiWindowFlags_NoMove;
  if (ImGui::Begin("World", &mShowEnvironment, wf)) {
    struct WorldSection {
      const char *name;
      const char *icon;
      ImWchar iconCode;
      const char *fallback;
    };
    const WorldSection sections[] = {
        {"Core", ICON_FA_COG, 0xf013, "C"},
        {"Sky", ICON_FA_SUN, 0xf185, "S"},
        {"Atmosphere", ICON_FA_CLOUD, 0xf0c2, "A"},
        {"Lighting", ICON_FA_LIGHTBULB, 0xf0eb, "L"},
        {"Post", ICON_FA_SLIDERS_H, 0xf1de, "P"},
        {"Terrain", ICON_FA_MOUNTAIN, 0xf6fc, "T"}};
    static int worldSection = 0;

    ImGui::BeginChild("WorldSidebar", ImVec2(42, 0), true);
    for (int i = 0; i < 6; ++i) {
      ImGui::PushID(i);
      const char *label =
          ctx.iconFontLoaded ? sections[i].icon : sections[i].fallback;
      ImVec2 start = ImGui::GetCursorScreenPos();
      ImVec2 size(ImGui::GetContentRegionAvail().x, 30.0f);
      bool pressed = ImGui::InvisibleButton("##WorldIcon", size);
      bool hovered = ImGui::IsItemHovered();
      bool selected = (worldSection == i);
      if (pressed) {
        worldSection = i;
      }
      ImU32 col = ImGui::ColorConvertFloat4ToU32(
          selected
              ? EditorTheme::kAccent
              : (hovered ? EditorTheme::kAccentHover : EditorTheme::kTextDim));
      ImDrawList *dl = ImGui::GetWindowDrawList();
      ImVec2 textSize = ImGui::CalcTextSize(label);
      ImVec2 textPos(start.x + (size.x - textSize.x) * 0.5f,
                     start.y + (size.y - textSize.y) * 0.5f);
      dl->AddText(textPos, col, label);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", sections[i].name);
      ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("WorldContent", ImVec2(0, 0), false);
    ImGui::SeparatorText(sections[worldSection].name);
    if (worldSection == 0) {
      ImGui::SeparatorText("Player");
      ImGui::DragFloat("Walk Speed", &ctx.walkStep, 0.001f, 0.0f, 1.0f);
      ImGui::DragFloat("Run Mult", &ctx.runMult, 0.1f, 1.0f, 10.0f);
      ImGui::DragFloat("Jump Force", &ctx.jumpStrength, 0.01f, 0.0f, 10.0f);
      ImGui::DragFloat("Gravity", &ctx.gravity, 0.001f, 0.0f, 1.0f);
      ImGui::Checkbox("Freeze Physics", &ctx.freezePhysics);
      ImGui::Checkbox("Creative Flight", &ctx.creativeFlight);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Play mode: disables player gravity. Space ascends, Shift "
            "descends.");
      }
      ImGui::SeparatorText("Viewmodel");
      ImGui::SliderInt("Active Slot", reinterpret_cast<int*>(&ctx.gameplay.activeSlot), 1, 2,
                       static_cast<int>(ctx.gameplay.activeSlot) == 1 ? "Axe" : "Torch");
      ImGui::Checkbox("Enable Axe", &ctx.gameplay.viewmodel.axeEnabled);
      if (ImGui::Button("Reset Axe Defaults")) {
        ctx.gameplay.viewmodel.axeOffset = glm::vec3(0.25f, -0.2f, 0.45f);
        ctx.gameplay.viewmodel.axeRotation = glm::vec3(0.0f, 0.0f, 0.0f);
        ctx.gameplay.viewmodel.axeScale = glm::vec3(0.6f);
      }
      ImGui::SameLine();
      if (ImGui::Button("Move Axe In Front")) {
        ctx.gameplay.viewmodel.axeOffset.z = std::max(0.2f, ctx.gameplay.viewmodel.axeOffset.z);
      }
      ImGui::DragFloat3("Axe Offset", &ctx.gameplay.viewmodel.axeOffset.x, 0.01f, -2.0f, 2.0f);
      ImGui::DragFloat3("Axe Rotation", &ctx.gameplay.viewmodel.axeRotation.x, 0.5f, -180.0f,
                        180.0f);
      ImGui::DragFloat3("Axe Scale", &ctx.gameplay.viewmodel.axeScale.x, 0.01f, 0.05f, 5.0f);
      ImGui::Separator();
      ImGui::Checkbox("Enable Torch", &ctx.gameplay.viewmodel.torchEnabled);
      if (ImGui::Button("Reset Torch Defaults")) {
        ctx.gameplay.viewmodel.torchOffset = glm::vec3(0.18f, -0.24f, 0.36f);
        ctx.gameplay.viewmodel.torchRotation = glm::vec3(18.0f, 0.0f, -10.0f);
        ctx.gameplay.viewmodel.torchScale = glm::vec3(0.07f, 0.58f, 0.07f);
      }
      ImGui::DragFloat3("Torch Offset", &ctx.gameplay.viewmodel.torchOffset.x, 0.01f, -2.0f, 2.0f);
      ImGui::DragFloat3("Torch Rotation", &ctx.gameplay.viewmodel.torchRotation.x, 0.5f, -180.0f,
                        180.0f);
      ImGui::DragFloat3("Torch Scale", &ctx.gameplay.viewmodel.torchScale.x, 0.01f, 0.02f, 5.0f);
      ImGui::Checkbox("Use Player Camera In Edit", &ctx.gameplay.viewmodel.usePlayerCameraInEdit);
      ImGui::TextDisabled("Tip: Axe Offset Z < 0 puts it behind the camera.");
      ImGui::SeparatorText("System");
      ImGui::Checkbox("Hot Reload", &ctx.hotReloadEnabled);
      ImGui::Checkbox("Auto Import", &ctx.autoProcessImportQueue);
      ImGui::SeparatorText("Audio");
      ImGui::Checkbox("Enable Audio", &ctx.audio.enabled);
      ImGui::SameLine();
      ImGui::Checkbox("Mute", &ctx.audio.mute);
      ImGui::SliderFloat("Master Volume", &ctx.audio.masterVolume, 0.0f, 1.5f,
                         "%.2f");
      ImGui::TextDisabled("Backend: %s",
                          ctx.audioBackendAvailable ? "Available" : "Unavailable");
      if (!ctx.audioStatus.empty())
        ImGui::TextWrapped("%s", ctx.audioStatus.c_str());

      auto drawAudioPathField = [&](const char *label, const char *tag,
                                    std::string &value) {
        char pathBuf[512] = {};
        std::snprintf(pathBuf, sizeof(pathBuf), "%s", value.c_str());
        if (ImGui::InputText(label, pathBuf, sizeof(pathBuf))) {
          value = pathBuf;
        }
        ImGui::SameLine();
        const std::string buttonId = std::string("Browse##") + tag;
        if (ImGui::Button(buttonId.c_str())) {
          mBrowsePath = tag;
          ImGui::OpenPopup("Select Terrain Asset");
        }
      };

      ImGui::Checkbox("Enable Ambient", &ctx.audio.ambientEnabled);
      drawAudioPathField("Ambient Track", "AudioAmbient",
                         ctx.audio.ambientPath);
      ImGui::SliderFloat("Ambient Volume", &ctx.audio.ambientVolume, 0.0f, 1.5f,
                         "%.2f");

      ImGui::Checkbox("Enable Footsteps", &ctx.audio.footstepsEnabled);
      ImGui::TextDisabled("Footstep clips auto-discover from assets/*.ogg");
      if (ImGui::Button("Play Test Footstep")) {
        mPendingConsoleCommands.push_back("audio_test_footstep");
      }
      ImGui::SliderFloat("Footstep Volume", &ctx.audio.footstepVolume, 0.0f,
                         1.5f, "%.2f");
      ImGui::SliderFloat("Walk Cadence", &ctx.audio.footstepWalkCadence, 0.10f, 0.80f,
                         "%.2fs");
      ImGui::SliderFloat("Run Cadence", &ctx.audio.footstepRunCadence, 0.08f, 0.60f,
                         "%.2fs");
    }
    if (worldSection == 1) {
      ImGui::Checkbox("Enable Day/Night", &ctx.dayNightEnabled);
      if (ctx.dayNightEnabled) {
        ImGui::SliderFloat("Time Of Day", &ctx.timeOfDay, 0.0f, 1.0f, "%.3f");
        ImGui::SliderFloat("Cycle Speed (per min)", &ctx.cycleSpeed, 0.0f, 1.0f,
                           "%.3f");
        ImGui::SeparatorText("Sky Colors");
        ImGui::ColorEdit3("Day Horizon", ctx.dayHorizon);
        ImGui::ColorEdit3("Day Top", ctx.dayTop);
        ImGui::ColorEdit3("Night Horizon", ctx.nightHorizon);
        ImGui::ColorEdit3("Night Top", ctx.nightTop);
        ImGui::SeparatorText("Lighting Sun Color Ramp");
        ImGui::ColorEdit3("Sun Day", &ctx.sunDayColor.x);
        ImGui::ColorEdit3("Sun Dusk", &ctx.sunDuskColor.x);
        ImGui::ColorEdit3("Sun Night", &ctx.sunNightColor.x);
        ImGui::SeparatorText("Visual Sun Color Ramp");
        ImGui::TextDisabled(
            "These tint only the sun disc/halo in the sky, not world lighting.");
        ImGui::ColorEdit3("Visual Sun Day", &ctx.visualSunDayColor.x);
        ImGui::ColorEdit3("Visual Sun Dusk", &ctx.visualSunDuskColor.x);
        ImGui::ColorEdit3("Visual Sun Night", &ctx.visualSunNightColor.x);
        ImGui::SeparatorText("Fireflies (Night)");
        ImGui::Checkbox("Enable Fireflies", &ctx.firefliesEnabled);
        ImGui::DragInt("Count", &ctx.fireflyCount, 1.0f, 0, 500);
        ImGui::DragFloat("Radius", &ctx.fireflyRadius, 0.5f, 5.0f, 80.0f);
        ImGui::DragFloat("Height Min", &ctx.fireflyHeightMin, 0.1f, 0.0f,
                         10.0f);
        ImGui::DragFloat("Height Max", &ctx.fireflyHeightMax, 0.1f, 0.1f,
                         15.0f);
        ImGui::DragFloat("Size", &ctx.fireflySize, 0.1f, 1.0f, 20.0f);
        ImGui::DragFloat("Intensity", &ctx.fireflyIntensity, 0.05f, 0.0f, 5.0f);
        ImGui::ColorEdit3("Color", &ctx.fireflyColor.x);
      } else {
        ImGui::ColorEdit3("Horizon", ctx.skyHorizon);
        ImGui::ColorEdit3("Top", ctx.skyTop);
      }
      ImGui::SeparatorText("Sun Direction");
      bool sunMoved = false;
      sunMoved |=
          ImGui::SliderFloat("Azimuth", &ctx.sun.sunAzimuth, 0.0f, 360.0f);
      if (!ctx.dayNightEnabled) {
        sunMoved |= ImGui::SliderFloat("Elevation", &ctx.sun.sunElevation,
                                       -90.0f, 90.0f);
      } else {
        ImGui::BeginDisabled();
        ImGui::SliderFloat("Elevation", &ctx.sun.sunElevation, -90.0f, 90.0f);
        ImGui::EndDisabled();
      }

      if (sunMoved) {
        float az = glm::radians(ctx.sun.sunAzimuth);
        float el = glm::radians(ctx.sun.sunElevation);
        // Convert spherical to cartesian direction (where +Y is up)
        ctx.sun.sunDir =
            glm::normalize(glm::vec3(std::cos(el) * std::sin(az), std::sin(el),
                                     std::cos(el) * std::cos(az)));
        // Since rays travel *from* the sun, we negate it
        ctx.sun.sunDir = -ctx.sun.sunDir;
      }

      if (!ctx.dayNightEnabled) {
        ImGui::ColorEdit3("Sun Light Color", &ctx.sun.sunColor.x);
        ImGui::ColorEdit3("Visual Sun Color", &ctx.visualSunColor.x);
        ImGui::TextDisabled(
            "Visual sun color changes the sky sun only. Sun light color changes lighting.");
      }
      ImGui::SeparatorText("Sun Light");
      ImGui::SliderFloat("Light Intensity", &ctx.sun.lightIntensity, 0.0f, 3.0f,
                         "%.2f");
      ImGui::SliderFloat("Ambient Strength", &ctx.sun.ambientStrength, 0.0f,
                         1.5f, "%.2f");
      ImGui::SeparatorText("Custom Sky Shape");
      if (ImGui::Button("Clear Noon Sky")) {
        ctx.disableHDR = true;
        ctx.useBlackHole = false;
        ctx.skyHorizon[0] = 0.55f;
        ctx.skyHorizon[1] = 0.72f;
        ctx.skyHorizon[2] = 0.95f;
        ctx.skyTop[0] = 0.22f;
        ctx.skyTop[1] = 0.42f;
        ctx.skyTop[2] = 0.82f;
        ctx.dayHorizon[0] = ctx.skyHorizon[0];
        ctx.dayHorizon[1] = ctx.skyHorizon[1];
        ctx.dayHorizon[2] = ctx.skyHorizon[2];
        ctx.dayTop[0] = ctx.skyTop[0];
        ctx.dayTop[1] = ctx.skyTop[1];
        ctx.dayTop[2] = ctx.skyTop[2];
        ctx.visualSunColor = glm::vec3(1.0f, 0.88f, 0.62f);
        ctx.visualSunDayColor = ctx.visualSunColor;
        ctx.skyAtmosphereStrength = 0.26f;
        ctx.skyGradientPower = 1.10f;
        ctx.skyHorizonGlow = 0.12f;
        ctx.skySunDiscIntensity = 14.0f;
        ctx.skySunHaloIntensity = 0.38f;
        ctx.skySunRaysIntensity = 0.08f;
        ctx.skySunHaloSize = 0.16f;
      }
      ImGui::SameLine();
      if (ImGui::Button("Golden Hour Sky")) {
        ctx.disableHDR = true;
        ctx.useBlackHole = false;
        ctx.skyHorizon[0] = 0.98f;
        ctx.skyHorizon[1] = 0.50f;
        ctx.skyHorizon[2] = 0.24f;
        ctx.skyTop[0] = 0.18f;
        ctx.skyTop[1] = 0.30f;
        ctx.skyTop[2] = 0.62f;
        ctx.dayHorizon[0] = ctx.skyHorizon[0];
        ctx.dayHorizon[1] = ctx.skyHorizon[1];
        ctx.dayHorizon[2] = ctx.skyHorizon[2];
        ctx.dayTop[0] = ctx.skyTop[0];
        ctx.dayTop[1] = ctx.skyTop[1];
        ctx.dayTop[2] = ctx.skyTop[2];
        ctx.visualSunColor = glm::vec3(1.0f, 0.46f, 0.16f);
        ctx.visualSunDayColor = ctx.visualSunColor;
        ctx.visualSunDuskColor = glm::vec3(1.0f, 0.36f, 0.12f);
        ctx.skyAtmosphereStrength = 0.34f;
        ctx.skyGradientPower = 1.45f;
        ctx.skyHorizonGlow = 0.28f;
        ctx.skySunDiscIntensity = 18.0f;
        ctx.skySunHaloIntensity = 0.62f;
        ctx.skySunRaysIntensity = 0.22f;
        ctx.skySunHaloSize = 0.24f;
      }
      ImGui::SameLine();
      if (ImGui::Button("Soft Fantasy Sky")) {
        ctx.disableHDR = true;
        ctx.useBlackHole = false;
        ctx.skyHorizon[0] = 0.74f;
        ctx.skyHorizon[1] = 0.82f;
        ctx.skyHorizon[2] = 0.96f;
        ctx.skyTop[0] = 0.34f;
        ctx.skyTop[1] = 0.48f;
        ctx.skyTop[2] = 0.88f;
        ctx.dayHorizon[0] = ctx.skyHorizon[0];
        ctx.dayHorizon[1] = ctx.skyHorizon[1];
        ctx.dayHorizon[2] = ctx.skyHorizon[2];
        ctx.dayTop[0] = ctx.skyTop[0];
        ctx.dayTop[1] = ctx.skyTop[1];
        ctx.dayTop[2] = ctx.skyTop[2];
        ctx.visualSunColor = glm::vec3(1.0f, 0.78f, 0.48f);
        ctx.visualSunDayColor = ctx.visualSunColor;
        ctx.skyAtmosphereStrength = 0.22f;
        ctx.skyGradientPower = 0.85f;
        ctx.skyHorizonGlow = 0.24f;
        ctx.skySunDiscIntensity = 11.0f;
        ctx.skySunHaloIntensity = 0.58f;
        ctx.skySunRaysIntensity = 0.14f;
        ctx.skySunHaloSize = 0.30f;
      }
      ImGui::SameLine();
      if (ImGui::Button("Black Hole Sky")) {
        ctx.disableHDR = true;
        ctx.useBlackHole = true;
        ctx.sky.skyCloudsEnabled = false;
        ctx.skyHorizon[0] = 0.06f;
        ctx.skyHorizon[1] = 0.04f;
        ctx.skyHorizon[2] = 0.10f;
        ctx.skyTop[0] = 0.00f;
        ctx.skyTop[1] = 0.00f;
        ctx.skyTop[2] = 0.03f;
        ctx.dayHorizon[0] = ctx.skyHorizon[0];
        ctx.dayHorizon[1] = ctx.skyHorizon[1];
        ctx.dayHorizon[2] = ctx.skyHorizon[2];
        ctx.dayTop[0] = ctx.skyTop[0];
        ctx.dayTop[1] = ctx.skyTop[1];
        ctx.dayTop[2] = ctx.skyTop[2];
        ctx.nightHorizon[0] = 0.02f;
        ctx.nightHorizon[1] = 0.01f;
        ctx.nightHorizon[2] = 0.04f;
        ctx.nightTop[0] = 0.00f;
        ctx.nightTop[1] = 0.00f;
        ctx.nightTop[2] = 0.02f;
        const BlackHolePresetDefaults bhDefaults =
            ctx.projectConfig.blackHoleDefaults.valid
                ? ctx.projectConfig.blackHoleDefaults
                : BlackHolePresetDefaults{};
        applyBlackHoleDefaults(ctx, bhDefaults);
        ctx.skyAtmosphereStrength = 0.04f;
        ctx.skyGradientPower = 0.92f;
        ctx.skyHorizonGlow = 0.02f;
      }
      ImGui::SliderFloat("Atmosphere Blend", &ctx.skyAtmosphereStrength, 0.0f,
                         1.0f, "%.2f");
      ImGui::SliderFloat("Gradient Curve", &ctx.skyGradientPower, 0.25f, 4.0f,
                         "%.2f");
      ImGui::SliderFloat("Horizon Glow", &ctx.skyHorizonGlow, 0.0f, 2.0f,
                         "%.2f");
      ImGui::SeparatorText("Celestial Body");
      int celestialBody = ctx.useBlackHole ? 1 : 0;
      if (ImGui::Combo("Type", &celestialBody, "Sun\0Black Hole\0")) {
        ctx.useBlackHole = (celestialBody == 1);
      }
      if (ctx.useBlackHole) {
        ImGui::TextDisabled(
            "Black hole position and size are independent from the lighting sun.");
        int placementMode = ctx.blackHoleWorldMode ? 1 : 0;
        if (ImGui::Combo("Placement", &placementMode,
                         "Sky Direction\0World Entity\0")) {
          ctx.blackHoleWorldMode = (placementMode == 1);
        }
        if (ctx.blackHoleWorldMode) {
          ImGui::DragFloat3("World Position", &ctx.blackHoleWorldPosition.x,
                            0.5f, -10000.0f, 10000.0f, "%.1f");
          ImGui::SliderFloat("World Radius", &ctx.blackHoleWorldRadius, 0.5f,
                             500.0f, "%.1f");
          ImGui::TextDisabled(
              "World Entity mode anchors the black hole to this world "
              "position. Size changes with camera distance.");
        } else {
          ImGui::SliderFloat("Black Hole Azimuth", &ctx.blackHoleAzimuth, 0.0f,
                             360.0f, "%.1f");
          ImGui::SliderFloat("Black Hole Elevation", &ctx.blackHoleElevation,
                             -90.0f, 90.0f, "%.1f");
        }
        ImGui::SliderFloat("View Pitch", &ctx.blackHoleViewPitchDeg, -89.0f,
                           89.0f, "%.1f deg");
        ImGui::SliderFloat("Event Horizon Size (deg)", &ctx.blackHoleSizeDeg,
                           0.10f, 20.0f, "%.2f");
        ImGui::SliderFloat("Disk Tilt", &ctx.blackHoleDiskTiltDeg, -180.0f,
                           180.0f, "%.1f deg");
        ImGui::SliderFloat("Disk Inclination",
                           &ctx.blackHoleDiskInclinationDeg, 0.0f, 88.0f,
                           "%.1f deg");
        ImGui::ColorEdit3("Accretion Color", &ctx.blackHoleColor.x);
        ImGui::SliderFloat("Inner Disk Radius",
                           &ctx.blackHoleInnerDiskRadius, 0.85f, 4.0f,
                           "%.2f");
        ImGui::SliderFloat("Outer Disk Radius",
                           &ctx.blackHoleOuterDiskRadius, 2.5f, 16.0f,
                           "%.2f");
        ImGui::SliderFloat("Disk Temperature",
                           &ctx.blackHoleDiskTemperature, 0.15f, 3.0f,
                           "%.2f");
        ImGui::SliderFloat("Disk Density", &ctx.blackHoleDiskDensity, 0.0f,
                           3.0f, "%.2f");
        ImGui::SliderFloat("Disk Spin", &ctx.blackHoleDiskSpinSpeed, 0.0f, 8.0f,
                           "%.2f");
        ImGui::SliderFloat("Disk Flow / Shear",
                           &ctx.blackHoleDiskFlowShear, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "Lower values make strips rotate in place. Higher values add "
              "more turbulent inward/outward accretion shear.");
        }
        ImGui::SliderFloat("Disk Turbulence", &ctx.blackHoleDiskTurbulence, 0.0f,
                           1.0f, "%.2f");
        ImGui::SliderFloat("Lensing Strength",
                           &ctx.blackHoleLensingStrength, 0.0f, 1.5f,
                           "%.2f");
        ImGui::SliderFloat("Photon Ring", &ctx.blackHolePhotonRingIntensity,
                           0.0f, 6.0f, "%.2f");
        ImGui::SliderFloat("Doppler Boost", &ctx.blackHoleDopplerBoost, 0.0f,
                           1.0f, "%.2f");
        ImGui::SliderFloat("Background Stars",
                           &ctx.blackHoleBackgroundStarIntensity, 0.0f, 4.0f,
                           "%.2f");
        ImGui::SliderFloat("Black Hole Exposure", &ctx.blackHoleExposure,
                           0.05f, 6.0f, "%.2f");
        ImGui::SliderFloat("Corona", &ctx.blackHoleCoronaIntensity, 0.0f, 2.0f,
                           "%.2f");
        ImGui::SliderFloat("Horizon Darkness", &ctx.blackHoleShadowStrength,
                           0.0f, 1.0f, "%.2f");
        ImGui::Combo("Quality", &ctx.blackHoleQuality,
                     "Performance\0High\0Ultra\0");
        ImGui::TextDisabled(
            "Use File > Save Current Settings As Defaults to keep these values "
            "across launches.");
      } else {
        ImGui::SliderFloat("Sun Size (deg)", &ctx.sun.sunSize, 0.05f, 3.0f,
                           "%.2f");
        ImGui::SliderFloat("Disc Intensity", &ctx.skySunDiscIntensity, 0.0f,
                           80.0f, "%.1f");
        ImGui::SliderFloat("Disc Softness", &ctx.skySunDiscSoftness, 0.0001f,
                           0.05f, "%.4f");
        ImGui::SliderFloat("Halo Intensity", &ctx.skySunHaloIntensity, 0.0f,
                           8.0f, "%.2f");
        ImGui::SliderFloat("Halo Size", &ctx.skySunHaloSize, 0.0f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Ray Intensity", &ctx.skySunRaysIntensity, 0.0f,
                           4.0f, "%.2f");
        ImGui::SliderFloat("Ray Sharpness", &ctx.skySunRaySharpness, 1.0f,
                           40.0f, "%.1f");
      }
      ImGui::SeparatorText("Sky Presentation");
      ImGui::Checkbox("Minimal Sky", &ctx.minimalSky);
      if (ctx.minimalSky) {
        ImGui::SliderFloat("Backdrop Blend", &ctx.skyBackdropBlend, 0.0f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Feature Visibility", &ctx.skyFeatureVisibility,
                           0.0f, 1.0f, "%.2f");
      }

      ImGui::SeparatorText("Background Type");
      bool useCustomSky = ctx.disableHDR;
      if (ImGui::Checkbox("Use Custom Procedural Sky", &useCustomSky)) {
        ctx.disableHDR = useCustomSky;
      }
      if (ctx.disableHDR) {
        ImGui::TextDisabled(
            "Custom procedural sky is active. HDRI reflections are disabled.");
      } else {
          if (ctx.skyHDRPath.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.25f, 1.0f),
                               "HDR pipeline is on, but no HDRI is selected yet.");
          } else {
            ImGui::TextDisabled(
                "HDRI sky is active. Manual day/night sky colors are ignored.");
          }
          char hdrPathBuf[512] = {};
          std::snprintf(hdrPathBuf, sizeof(hdrPathBuf), "%s",
                        ctx.skyHDRPath.c_str());
          if (ImGui::InputText("Sky HDRI (.hdr)", hdrPathBuf,
                               sizeof(hdrPathBuf))) {
            ctx.skyHDRPath = hdrPathBuf;
          }
          ImGui::SameLine();
          if (ImGui::Button("Browse##SkyHDR")) {
            mBrowsePath = "SkyHDR";
            ImGui::OpenPopup("Select Terrain Asset");
          }
      }
    }
    if (worldSection == 2) {
      bool cloudsEnabled = !ctx.disableClouds;
      if (ImGui::Checkbox("Enable All Clouds", &cloudsEnabled)) {
        ctx.disableClouds = !cloudsEnabled;
      }
      if (ctx.disableClouds) {
        ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.24f, 1.0f),
                           "Cloud sliders are inactive while this is off.");
      }
      ImGui::Separator();

      ImGui::BeginDisabled(ctx.disableClouds);
      ImGui::SeparatorText("Sky Clouds (Lightweight)");
      ImGui::Checkbox("Enable Sky Clouds", &ctx.sky.skyCloudsEnabled);
      if (ctx.sky.skyCloudsEnabled) {
        if (ImGui::Button("Scattered Cumulus##SkyClouds")) {
          ctx.sky.skyCloudScale = 2.0f;
          ctx.sky.skyCloudCoverage = 0.46f;
          ctx.sky.skyCloudDensity = 0.72f;
          ctx.sky.skyCloudSoftness = 0.24f;
          ctx.sky.skyCloudSpeed = 0.006f;
          ctx.sky.skyCloudColor = glm::vec3(0.96f, 0.97f, 1.0f);
        }
        ImGui::SameLine();
        if (ImGui::Button("Wispy High Clouds##SkyClouds")) {
          ctx.sky.skyCloudScale = 3.6f;
          ctx.sky.skyCloudCoverage = 0.35f;
          ctx.sky.skyCloudDensity = 0.42f;
          ctx.sky.skyCloudSoftness = 0.36f;
          ctx.sky.skyCloudSpeed = 0.010f;
          ctx.sky.skyCloudColor = glm::vec3(0.92f, 0.95f, 1.0f);
        }
        ImGui::SameLine();
        if (ImGui::Button("Storm Build-Up##SkyClouds")) {
          ctx.sky.skyCloudScale = 1.35f;
          ctx.sky.skyCloudCoverage = 0.72f;
          ctx.sky.skyCloudDensity = 1.05f;
          ctx.sky.skyCloudSoftness = 0.16f;
          ctx.sky.skyCloudSpeed = 0.004f;
          ctx.sky.skyCloudColor = glm::vec3(0.72f, 0.76f, 0.84f);
        }
        ImGui::SliderFloat("Cloud Scale##Sky", &ctx.sky.skyCloudScale, 0.5f,
                           6.0f, "%.2f");
        ImGui::SliderFloat("Coverage##Sky", &ctx.sky.skyCloudCoverage, 0.0f,
                           0.95f, "%.2f");
        ImGui::SliderFloat("Density##Sky", &ctx.sky.skyCloudDensity, 0.0f, 1.6f,
                           "%.2f");
        ImGui::SliderFloat("Softness##Sky", &ctx.sky.skyCloudSoftness, 0.02f,
                           0.45f, "%.2f");
        ImGui::SliderFloat("Speed##Sky", &ctx.sky.skyCloudSpeed, 0.0f, 0.05f,
                           "%.3f");
        ImGui::ColorEdit3("Cloud Color##Sky", &ctx.sky.skyCloudColor.x);
      }

      ImGui::SeparatorText("Local Volumetric Layer (Experimental)");
      ImGui::Checkbox("Enable Local Volumetric Clouds", &ctx.cloud.enabled);
      ImGui::TextDisabled(
          "Off by default: the sky cloud layer is now the main cloud renderer.");
      ImGui::BeginDisabled(!ctx.cloud.enabled);
      ImGui::SliderFloat("Cover", &ctx.cloud.cover, 0.0f, 1.0f);
      ImGui::SliderFloat("Density", &ctx.cloud.density, 0.0f, 2.0f);
      ImGui::SliderFloat("Opacity", &ctx.cloud.alpha, 0.0f, 0.8f, "%.2f");
      ImGui::SliderFloat("Softness##VolCloud", &ctx.cloud.softness, 0.02f,
                         0.5f, "%.2f");
      ImGui::SliderFloat("Height##VolCloud", &ctx.cloud.height, 5.0f, 180.0f,
                         "%.1f");
      ImGui::SliderFloat("Thickness##VolCloud", &ctx.cloud.thickness, 2.0f,
                         80.0f, "%.1f");
      ImGui::SliderFloat("Size##VolCloud", &ctx.cloud.size, 120.0f, 1200.0f,
                         "%.0f");
      ImGui::SliderFloat("Light Absorption##VolCloud",
                         &ctx.cloud.lightAbsorption, 0.2f, 3.0f, "%.2f");
      ImGui::SliderFloat("Silver Lining##VolCloud", &ctx.cloud.phaseG, 0.0f,
                         0.92f, "%.2f");
      ImGui::EndDisabled();
      ImGui::EndDisabled();
    }
    if (worldSection == 3) {
      ImGui::SeparatorText("Fog");
      ImGui::ColorEdit3("Fog Color", &ctx.fogColor.x);
      ImGui::SliderFloat("Fog Density", &ctx.fogDensity, 0.0f, 0.05f, "%.4f");
      ImGui::SliderFloat("Fog Height Falloff", &ctx.fogHeightFalloff, 0.0f,
                         0.12f, "%.3f");
      ImGui::TextDisabled(
          "Lower falloff keeps haze visible across tall mountains.");

      ImGui::SeparatorText("Aerial Perspective");
      if (ImGui::Button("Cinematic Distance Haze")) {
        ctx.fogDensity = 0.0012f;
        ctx.fogHeightFalloff = 0.014f;
        ctx.fogColor = glm::vec3(0.58f, 0.66f, 0.76f);
        ctx.aerialPerspectiveEnabled = true;
        ctx.aerialPerspectiveDensity = 0.00145f;
        ctx.aerialPerspectiveStart = 45.0f;
        ctx.aerialPerspectiveHeightFalloff = 0.0055f;
        ctx.aerialPerspectiveSkyBlend = 0.76f;
        ctx.aerialPerspectiveSunGlow = 0.42f;
        ctx.aerialPerspectiveDesaturation = 0.38f;
      }
      ImGui::SameLine();
      if (ImGui::Button("Clear Long View")) {
        ctx.fogDensity = 0.00055f;
        ctx.fogHeightFalloff = 0.018f;
        ctx.fogColor = glm::vec3(0.58f, 0.66f, 0.76f);
        ctx.aerialPerspectiveEnabled = true;
        ctx.aerialPerspectiveDensity = 0.00075f;
        ctx.aerialPerspectiveStart = 120.0f;
        ctx.aerialPerspectiveHeightFalloff = 0.0075f;
        ctx.aerialPerspectiveSkyBlend = 0.55f;
        ctx.aerialPerspectiveSunGlow = 0.22f;
        ctx.aerialPerspectiveDesaturation = 0.20f;
      }
      ImGui::SameLine();
      if (ImGui::Button("Misty Valley")) {
        ctx.fogDensity = 0.0024f;
        ctx.fogHeightFalloff = 0.006f;
        ctx.fogColor = glm::vec3(0.62f, 0.68f, 0.74f);
        ctx.aerialPerspectiveEnabled = true;
        ctx.aerialPerspectiveDensity = 0.00220f;
        ctx.aerialPerspectiveStart = 25.0f;
        ctx.aerialPerspectiveHeightFalloff = 0.0030f;
        ctx.aerialPerspectiveSkyBlend = 0.85f;
        ctx.aerialPerspectiveSunGlow = 0.55f;
        ctx.aerialPerspectiveDesaturation = 0.48f;
      }
      ImGui::Checkbox("Enable Aerial Perspective",
                      &ctx.aerialPerspectiveEnabled);
      ImGui::BeginDisabled(!ctx.aerialPerspectiveEnabled);
      ImGui::SliderFloat("Atmosphere Density", &ctx.aerialPerspectiveDensity,
                         0.0f, 0.008f, "%.5f");
      ImGui::SliderFloat("Start Distance", &ctx.aerialPerspectiveStart, 0.0f,
                         300.0f, "%.0f m");
      ImGui::SliderFloat("Altitude Falloff",
                         &ctx.aerialPerspectiveHeightFalloff, 0.0f, 0.04f,
                         "%.4f");
      ImGui::SliderFloat("Sky Color Blend", &ctx.aerialPerspectiveSkyBlend,
                         0.0f, 1.0f, "%.2f");
      ImGui::SliderFloat("Sun Glow", &ctx.aerialPerspectiveSunGlow, 0.0f,
                         1.5f, "%.2f");
      ImGui::SliderFloat("Far Desaturation",
                         &ctx.aerialPerspectiveDesaturation, 0.0f, 1.0f,
                         "%.2f");
      ImGui::TextDisabled(
          "Uses current sky/day-night colors, so distant terrain fades into the horizon instead of a flat fog tint.");
      ImGui::EndDisabled();

      ImGui::SeparatorText("Environment Ambient");
      if (ImGui::Button("Meadows Ambient")) {
        applyValheimAmbientPreset(ctx, 0);
      }
      ImGui::SameLine();
      if (ImGui::Button("Black Forest")) {
        applyValheimAmbientPreset(ctx, 1);
      }
      ImGui::SameLine();
      if (ImGui::Button("Swamp Mood")) {
        applyValheimAmbientPreset(ctx, 2);
      }
      ImGui::SameLine();
      if (ImGui::Button("Mountain Air")) {
        applyValheimAmbientPreset(ctx, 3);
      }
      ImGui::Checkbox("Hemisphere Ambient", &ctx.ambientHemisphereEnabled);
      ImGui::BeginDisabled(!ctx.ambientHemisphereEnabled);
      ImGui::SliderFloat("Master Ambient##EnvAmbient",
                         &ctx.sun.ambientStrength, 0.0f, 1.5f, "%.2f");
      ImGui::SliderFloat("Hemisphere Strength",
                         &ctx.ambientHemisphereIntensity, 0.0f, 2.5f,
                         "%.2f");
      ImGui::SliderFloat("Follow Sky Colors", &ctx.ambientSkyInfluence, 0.0f,
                         1.0f, "%.2f");
      ImGui::SliderFloat("Horizon Wrap", &ctx.ambientHorizonStrength, 0.0f,
                         1.5f, "%.2f");
      ImGui::SliderFloat("Terrain Ambient Boost", &ctx.ambientTerrainBoost,
                         0.5f, 2.5f, "%.2f");
      ImGui::ColorEdit3("Ambient Sky", &ctx.ambientSkyColor.x);
      ImGui::ColorEdit3("Ambient Horizon", &ctx.ambientHorizonColor.x);
      ImGui::ColorEdit3("Ambient Ground", &ctx.ambientGroundColor.x);
      ImGui::TextDisabled(
          "Sky follow blends these colors with the current custom sky/day-night profile.");
      ImGui::EndDisabled();

      ImGui::SeparatorText("Stylized Lighting");
      ImGui::TextDisabled("These are art-direction controls, not realism knobs.");
      ImGui::Checkbox("Enable Toon", &ctx.toonEnabled);
      ImGui::SliderInt("Toon Steps", &ctx.toonSteps, 2, 8);
      ImGui::SliderFloat("Toon Min Light", &ctx.toonMin, 0.0f, 0.4f, "%.2f");
      ImGui::SeparatorText("Shadow Bands");
      ImGui::Checkbox("Enable Shadow Bands", &ctx.shadowBandEnabled);
      ImGui::SliderInt("Shadow Steps", &ctx.shadowBandSteps, 2, 6);
      ImGui::SliderFloat("Shadow Softness", &ctx.shadowBandSoftness, 0.0f, 1.0f,
                         "%.2f");
      ImGui::SeparatorText("Ambient Ramp");
      ImGui::Checkbox("Enable Ambient Ramp", &ctx.ambientRampEnabled);
      ImGui::ColorEdit3("Ambient Top", &ctx.ambientRampTop.x);
      ImGui::ColorEdit3("Ambient Bottom", &ctx.ambientRampBottom.x);
      ImGui::SliderFloat("Ambient Strength", &ctx.ambientRampStrength, 0.0f,
                         1.5f, "%.2f");
      ImGui::SeparatorText("Rim Lighting");
      ImGui::Checkbox("Enable Rim", &ctx.rimEnabled);
      ImGui::ColorEdit3("Rim Color", &ctx.rimColor.x);
      ImGui::SliderFloat("Rim Strength", &ctx.rimStrength, 0.0f, 2.0f, "%.2f");
      ImGui::SliderFloat("Rim Power", &ctx.rimPower, 0.5f, 6.0f, "%.2f");
    }
    if (worldSection == 4) {
      ImGui::SeparatorText("Graphics Presets");
      if (ImGui::Button("Custom Sky Realistic")) {
        applyHdrRealisticGraphicsPreset(ctx);
      }
      ImGui::SameLine();
      if (ImGui::Button("Balanced Performance")) {
        applyBalancedPerformanceGraphicsPreset(ctx);
      }
      ImGui::TextDisabled(
          "Presets also fix hidden master toggles, not just post-process values.");

      ImGui::SeparatorText("Diagnostics");
      const bool hdrPipeline = !ctx.disableHDR;
      const bool hdrTextureActive = hdrPipeline && ctx.sky.hasHDRTexture();
      drawGraphicsStatusRow(
          "HDR pipeline", hdrPipeline,
          hdrPipeline ? (hdrTextureActive ? "HDRI active" : "Procedural HDR")
                      : "Off",
          hdrPipeline
              ? (hdrTextureActive ? "environment reflections available"
                                  : "no loaded HDRI, reflections disabled")
              : "sky/env lighting forced procedural");
      drawGraphicsStatusRow(
          "Cloud controls", !ctx.disableClouds,
          ctx.disableClouds ? "Master off" : "Active",
          ctx.disableClouds ? "cloud sliders intentionally do nothing" : "");
      drawGraphicsStatusRow(
          "Bloom", ctx.postProcessor.bloomIntensity > 0.001f,
          ctx.postProcessor.bloomIntensity > 0.001f ? "Active" : "Muted",
          ctx.postProcessor.bloomThreshold > 2.0f
              ? "high threshold can make it subtle"
              : "");
      drawGraphicsStatusRow(
          "SSAO", ctx.postProcessor.enableSSAO, 
          ctx.postProcessor.enableSSAO ? "Active" : "Off");
      drawGraphicsStatusRow(
          "Env ambient", ctx.ambientHemisphereEnabled,
          ctx.ambientHemisphereEnabled ? "Hemi active" : "Off",
          ctx.ambientHemisphereEnabled ? "sky/horizon/ground tinting world"
                                       : "");
      drawGraphicsStatusRow(
          "Volumetrics", ctx.postProcessor.enableVolumetricFog,
          ctx.postProcessor.enableVolumetricFog ? "Active" : "Off");
      drawGraphicsStatusRow(
          "Anti-aliasing",
          ctx.postProcessor.enableTAA || ctx.postProcessor.enableFXAA,
          ctx.postProcessor.enableTAA
              ? "TAA active"
              : (ctx.postProcessor.enableFXAA ? "FXAA active" : "Off"),
          ctx.postProcessor.enableTAA && ctx.postProcessor.enableFXAA
              ? "FXAA is bypassed while TAA is on"
              : "");
      const float sunHeight =
          ctx.dayNightEnabled
              ? std::sin(ctx.timeOfDay * 6.28318530718f - 1.57079632679f)
              : 1.0f;
      drawGraphicsStatusRow(
          "Day/Night overrides", ctx.dayNightEnabled,
          ctx.dayNightEnabled ? (sunHeight < 0.15f ? "Dusk/night active"
                                                   : "Cycle active")
                              : "Off",
          ctx.dayNightEnabled ? "dusk/night can override post values" : "");

      ImGui::SeparatorText("Camera / Tone Mapping");
      ImGui::SliderFloat("Scene Exposure", &ctx.exposure, 0.10f, 3.00f,
                         "%.2f");
      ImGui::SliderFloat("Display Gamma", &ctx.gamma, 1.60f, 2.80f, "%.2f");
      ImGui::SliderFloat("Brightness##Render", &ctx.postProcessor.brightness,
                         0.0f, 3.0f, "%.2f");
      ImGui::Checkbox("Enable Auto Exposure",
                      &ctx.postProcessor.enableAutoExposure);
      if (ctx.postProcessor.enableAutoExposure) {
        ImGui::SliderFloat("Auto Min", &ctx.postProcessor.autoExposureMin,
                           0.20f, 2.00f, "%.2f");
        ImGui::SliderFloat("Auto Max", &ctx.postProcessor.autoExposureMax,
                           0.30f, 4.00f, "%.2f");
        ImGui::SliderFloat("Auto Target", &ctx.postProcessor.autoExposureTarget,
                           0.05f, 1.00f, "%.2f");
        ImGui::SliderFloat("Adapt Speed", &ctx.postProcessor.autoExposureSpeed,
                           0.001f, 0.40f, "%.3f");
        ImGui::TextDisabled("Current auto exposure: %.2f",
                            ctx.postProcessor.autoExposureValue);
      }

      ImGui::Checkbox("Wireframe", &ctx.wireframe);
      ImGui::SeparatorText("Outline");
      ImGui::Checkbox("Enable Outline", &ctx.postProcessor.enableOutline);
      ImGui::ColorEdit3("Outline Color", &ctx.postProcessor.outlineColor.x);
      ImGui::SliderFloat("Outline Strength", &ctx.postProcessor.outlineStrength,
                         0.0f, 2.0f, "%.2f");
      ImGui::SliderFloat("Outline Thickness",
                         &ctx.postProcessor.outlineThickness, 0.5f, 3.0f,
                         "%.2f");
      ImGui::SliderFloat("Outline Threshold",
                         &ctx.postProcessor.outlineThreshold, 0.0002f, 0.01f,
                         "%.4f");
      ImGui::SeparatorText("Distance Tint");
      ImGui::Checkbox("Enable Distance Tint",
                      &ctx.postProcessor.enableDistanceTint);
      ImGui::ColorEdit3("Tint Color", &ctx.postProcessor.distanceTintColor.x);
      ImGui::SliderFloat("Tint Start", &ctx.postProcessor.distanceTintStart,
                         10.0f, 300.0f, "%.1f");
      ImGui::SliderFloat("Tint End", &ctx.postProcessor.distanceTintEnd, 20.0f,
                         600.0f, "%.1f");
      ImGui::SeparatorText("Color Grading");
      ImGui::Checkbox("Enable Color Grade",
                      &ctx.postProcessor.enableColorGrade);
      ImGui::SliderFloat("Saturation", &ctx.postProcessor.gradeSaturation, 0.0f,
                         2.0f, "%.2f");
      ImGui::SliderFloat("Contrast", &ctx.postProcessor.gradeContrast, 0.5f,
                         2.0f, "%.2f");
      ImGui::SliderFloat("Lift", &ctx.postProcessor.gradeLift, -0.3f, 0.3f,
                         "%.2f");
      ImGui::SliderFloat("Gamma", &ctx.postProcessor.gradeGamma, 0.6f, 1.6f,
                         "%.2f");
      ImGui::SliderFloat("Gain", &ctx.postProcessor.gradeGain, 0.5f, 2.0f,
                         "%.2f");
      ImGui::ColorEdit3("Grade Tint", &ctx.postProcessor.gradeTint.x);
      ImGui::SeparatorText("Palette Quantize");
      ImGui::Checkbox("Enable Palette",
                      &ctx.postProcessor.enablePaletteQuantize);
      ImGui::SliderInt("Palette Steps", &ctx.postProcessor.paletteSteps, 2, 16);
      ImGui::SeparatorText("Bloom");
      ImGui::SliderFloat("Threshold##Bloom", &ctx.postProcessor.bloomThreshold,
                         0.2f, 4.0f, "%.2f");
      ImGui::SliderFloat("Resolution Scale##Bloom",
                         &ctx.postProcessor.bloomScale, 0.25f, 1.0f, "%.2f");
      ImGui::SliderInt("Blur Iterations##Bloom",
                       &ctx.postProcessor.blurIterations, 1, 20);
      ImGui::SliderFloat("Intensity##Bloom", &ctx.postProcessor.bloomIntensity,
                         0.0f, 3.0f, "%.2f");

      ImGui::SeparatorText("Anti-Aliasing");
      ImGui::Checkbox("Enable TAA", &ctx.postProcessor.enableTAA);
      if (ctx.postProcessor.enableTAA) {
        ImGui::TextDisabled(
            "TAA uses a stable temporal resolve. FXAA is bypassed while TAA is on.");
        ImGui::SliderFloat("History Blend##TAA",
                           &ctx.postProcessor.taaHistoryBlend, 0.50f, 0.96f,
                           "%.2f");
        ImGui::SliderFloat("Jitter Scale##TAA",
                           &ctx.postProcessor.taaJitterScale, 0.25f, 1.5f,
                           "%.2f");
        ImGui::SliderFloat("Motion Reset##TAA",
                           &ctx.postProcessor.taaMotionReset, 0.05f, 2.0f,
                           "%.2f");
      }
      ImGui::Checkbox("Enable FXAA", &ctx.postProcessor.enableFXAA);
      if (ctx.postProcessor.enableFXAA) {
        ImGui::SliderFloat("Span Max##FXAA", &ctx.postProcessor.fxaaSpanMax,
                           2.0f, 16.0f, "%.1f");
        ImGui::SliderFloat("Reduce Min##FXAA", &ctx.postProcessor.fxaaReduceMin,
                           1.0f / 256.0f, 1.0f / 64.0f, "%.4f");
        ImGui::SliderFloat("Reduce Mul##FXAA", &ctx.postProcessor.fxaaReduceMul,
                           1.0f / 16.0f, 1.0f / 4.0f, "%.4f");
      }

      ImGui::SeparatorText("SSAO");
      ImGui::Checkbox("Enable SSAO", &ctx.postProcessor.enableSSAO);
      if (ctx.postProcessor.enableSSAO) {
        static const char *ssaoQualityLabels[] = {"Low", "Medium", "High",
                                                  "Ultra", "Custom"};
        const int prevSsaoQuality = ctx.postProcessor.ssaoQuality;
        bool qualityChanged =
            ImGui::Combo("Quality##SSAO", &ctx.postProcessor.ssaoQuality,
                         ssaoQualityLabels,
                         IM_ARRAYSIZE(ssaoQualityLabels));
        if (qualityChanged && ctx.postProcessor.ssaoQuality >= 0 &&
            ctx.postProcessor.ssaoQuality <= 3) {
          applySsaoQualityPreset(ctx.postProcessor, ctx.postProcessor.ssaoQuality);
        } else if (ctx.postProcessor.ssaoQuality < 0 ||
                   ctx.postProcessor.ssaoQuality > 4) {
          ctx.postProcessor.ssaoQuality = prevSsaoQuality;
        }

        bool ssaoManualChanged = false;
        ssaoManualChanged |=
            ImGui::SliderFloat("Radius##SSAO", &ctx.postProcessor.ssaoRadius,
                               0.1f, 2.0f, "%.2f");
        ssaoManualChanged |=
            ImGui::SliderFloat("Bias##SSAO", &ctx.postProcessor.ssaoBias, 0.001f,
                               0.10f, "%.3f");
        ssaoManualChanged |=
            ImGui::SliderFloat("Power##SSAO", &ctx.postProcessor.ssaoPower, 0.5f,
                               4.0f, "%.2f");
        ssaoManualChanged |= ImGui::SliderFloat(
            "Intensity##SSAO", &ctx.postProcessor.ssaoIntensity, 0.0f, 2.0f,
            "%.2f");
        if (ssaoManualChanged && !qualityChanged &&
            ctx.postProcessor.ssaoQuality != 4) {
          ctx.postProcessor.ssaoQuality = 4;
        }
      }

      ImGui::SeparatorText("Volumetrics");
      ImGui::Checkbox("Enable Volumetric Fog",
                      &ctx.postProcessor.enableVolumetricFog);
      if (ctx.postProcessor.enableVolumetricFog) {
        static const char *volQualityLabels[] = {"Low", "Medium", "High",
                                                 "Ultra", "Custom"};
        const int prevVolQuality = ctx.postProcessor.volumetricQuality;
        bool volQualityChanged =
            ImGui::Combo("Quality##Vol", &ctx.postProcessor.volumetricQuality,
                         volQualityLabels, IM_ARRAYSIZE(volQualityLabels));
        if (volQualityChanged && ctx.postProcessor.volumetricQuality >= 0 &&
            ctx.postProcessor.volumetricQuality <= 3) {
          applyVolumetricQualityPreset(ctx.postProcessor,
                                       ctx.postProcessor.volumetricQuality);
        } else if (ctx.postProcessor.volumetricQuality < 0 ||
                   ctx.postProcessor.volumetricQuality > 4) {
          ctx.postProcessor.volumetricQuality = prevVolQuality;
        }

        bool volManualChanged = false;
        volManualChanged |=
            ImGui::SliderFloat("Fog Density##Vol",
                               &ctx.postProcessor.volumetricFogDensity, 0.001f,
                               0.08f, "%.3f");
        volManualChanged |=
            ImGui::SliderFloat("Light Exposure##Vol",
                               &ctx.postProcessor.volumetricLightExposure, 0.0f,
                               1.0f, "%.2f");
        volManualChanged |=
            ImGui::SliderFloat("Light Weight##Vol",
                               &ctx.postProcessor.volumetricLightWeight, 0.01f,
                               0.25f, "%.3f");
        volManualChanged |=
            ImGui::SliderFloat("Light Decay##Vol",
                               &ctx.postProcessor.volumetricLightDecay, 0.80f,
                               0.999f, "%.3f");
        if (volManualChanged && !volQualityChanged &&
            ctx.postProcessor.volumetricQuality != 4) {
          ctx.postProcessor.volumetricQuality = 4;
        }
      }

      ImGui::SeparatorText("Performance");
      if (ctx.postProcessor.enableSSAO) {
        bool ssaoPerfChanged = false;
        ssaoPerfChanged |= ImGui::SliderFloat(
            "SSAO Res Scale##Perf", &ctx.postProcessor.ssaoScale, 0.25f, 1.0f,
            "%.2f");
        ssaoPerfChanged |=
            ImGui::Checkbox("Scale SSAO Radius",
                            &ctx.postProcessor.ssaoScaleRadius);
        ImGui::Checkbox("Full-Res On Flat Terrain",
                        &ctx.postProcessor.ssaoFullResTerrain);
        ssaoPerfChanged |= ImGui::SliderInt(
            "SSAO Samples##Perf", &ctx.postProcessor.ssaoSamples, 8, 64);
        if (ssaoPerfChanged && ctx.postProcessor.ssaoQuality != 4) {
          ctx.postProcessor.ssaoQuality = 4;
        }
      }
      if (ctx.postProcessor.enableVolumetricFog) {
        bool volPerfChanged = false;
        volPerfChanged |= ImGui::SliderFloat("Volumetric Res Scale##Perf",
                                             &ctx.postProcessor.volumetricScale,
                                             0.25f, 1.0f, "%.2f");
        volPerfChanged |= ImGui::SliderInt("Volumetric Samples##Perf",
                                           &ctx.postProcessor.volumetricSamples,
                                           8, 64);
        if (volPerfChanged && ctx.postProcessor.volumetricQuality != 4) {
          ctx.postProcessor.volumetricQuality = 4;
        }
      }

      ImGui::SeparatorText("Shadows");
      bool enableShadows = !ctx.disableShadows;
      if (ImGui::Checkbox("Enable Shadows", &enableShadows)) {
        ctx.disableShadows = !enableShadows;
      }
      if (!ctx.disableShadows) {
        if (ImGui::Button("Huge Terrain Shadows")) {
          ctx.enableCascadedShadows = true;
          ctx.shadowCascadeCount = 4;
          ctx.shadowMapResolution = 2048;
          ctx.shadowCascadeDistance = 760.0f;
          ctx.shadowCascadeLambda = 0.68f;
          ctx.shadowStrength = 1.15f;
          ctx.shadowNormalBias = 0.030f;
          ctx.shadowDepthBias = 0.0014f;
          ctx.shadowSoftness = 1.0f;
          ctx.shadowUpdateInterval = 1;
          ctx.shadowUpdateDistance = 0.35f;
          ctx.shadowUpdateAngle = 1.4f;
          ctx.shadowStaggeredUpdates = true;
          ctx.shadowCascadeCadence = 2;
          ctx.shadowCascadeDistanceScale = 2.0f;
          ctx.shadowCascadeAngleScale = 1.6f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Crisp Near Shadows")) {
          ctx.enableCascadedShadows = true;
          ctx.shadowCascadeCount = 4;
          ctx.shadowMapResolution = 4096;
          ctx.shadowCascadeDistance = 420.0f;
          ctx.shadowCascadeLambda = 0.78f;
          ctx.shadowStrength = 1.20f;
          ctx.shadowNormalBias = 0.024f;
          ctx.shadowDepthBias = 0.0012f;
          ctx.shadowSoftness = 0.75f;
          ctx.shadowUpdateInterval = 1;
          ctx.shadowStaggeredUpdates = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Performance Shadows")) {
          ctx.enableCascadedShadows = true;
          ctx.shadowCascadeCount = 3;
          ctx.shadowMapResolution = 1536;
          ctx.shadowCascadeDistance = 500.0f;
          ctx.shadowCascadeLambda = 0.58f;
          ctx.shadowStrength = 1.0f;
          ctx.shadowNormalBias = 0.040f;
          ctx.shadowDepthBias = 0.0020f;
          ctx.shadowSoftness = 1.25f;
          ctx.shadowUpdateInterval = 2;
          ctx.shadowStaggeredUpdates = true;
          ctx.shadowCascadeCadence = 2;
          ctx.shadowCascadeDistanceScale = 2.4f;
          ctx.shadowCascadeAngleScale = 1.8f;
        }

        ImGui::SliderFloat("Shadow Strength", &ctx.shadowStrength, 0.0f, 2.5f,
                           "%.2f");
        ImGui::Checkbox("Cascaded Shadows", &ctx.enableCascadedShadows);
        if (ctx.enableCascadedShadows) {
          ImGui::SliderInt("Cascade Count", &ctx.shadowCascadeCount, 2, 4);
          static const char *shadowResLabels[] = {"1024", "1536", "2048",
                                                  "4096"};
          int resIndex = ctx.shadowMapResolution >= 4096
                             ? 3
                             : ctx.shadowMapResolution >= 2048
                                   ? 2
                                   : ctx.shadowMapResolution >= 1536 ? 1 : 0;
          if (ImGui::Combo("Shadow Resolution", &resIndex, shadowResLabels,
                           IM_ARRAYSIZE(shadowResLabels))) {
            const int values[] = {1024, 1536, 2048, 4096};
            ctx.shadowMapResolution = values[resIndex];
          }
          ImGui::SliderFloat("Cascade Distance",
                             &ctx.shadowCascadeDistance, 80.0f, 1600.0f,
                             "%.0f");
          ImGui::SliderFloat("Split Lambda", &ctx.shadowCascadeLambda, 0.0f,
                             1.0f, "%.2f");
          ImGui::SliderFloat("Normal Bias", &ctx.shadowNormalBias, 0.0f,
                             0.12f, "%.4f");
          ImGui::SliderFloat("Depth Bias", &ctx.shadowDepthBias, 0.0f,
                             0.01f, "%.4f");
          ImGui::SliderFloat("Softness", &ctx.shadowSoftness, 0.25f, 3.0f,
                             "%.2f");
          ImGui::Checkbox("Show Cascade Debug", &ctx.showShadowCascades);
          ImGui::TextDisabled(
              "CSM keeps near shadows sharp while still covering far terrain.");
        } else {
          ImGui::SliderFloat("Shadow Far Plane", &ctx.shadowFarPlane, 40.0f,
                             900.0f, "%.0f");
          ImGui::TextDisabled(
              "Single-map mode is a fallback; large ranges will look softer.");
        }
        ImGui::Checkbox("Shadow Camera Culling", &ctx.shadowCameraCulling);
        ImGui::SliderInt("Shadow Update Interval", &ctx.shadowUpdateInterval, 1,
                         8);
        ImGui::SliderFloat("Shadow Update Distance", &ctx.shadowUpdateDistance,
                           0.0f, 5.0f, "%.2f");
        ImGui::SliderFloat("Shadow Update Angle", &ctx.shadowUpdateAngle, 0.0f,
                           10.0f, "%.1f deg");
        if (ctx.enableCascadedShadows) {
          ImGui::Checkbox("Stagger Cascade Updates",
                          &ctx.shadowStaggeredUpdates);
          if (ctx.shadowStaggeredUpdates) {
            ImGui::SliderInt("Cascade Cadence Base",
                             &ctx.shadowCascadeCadence, 1, 4);
            ImGui::SliderFloat("Cascade Distance Scale",
                               &ctx.shadowCascadeDistanceScale, 1.0f, 4.0f,
                               "%.2f");
            ImGui::SliderFloat("Cascade Angle Scale",
                               &ctx.shadowCascadeAngleScale, 1.0f, 4.0f,
                               "%.2f");
            ImGui::TextDisabled(
                "Near cascades stay fresh while far cascades refresh less often.");
          }
        }
      }
    }
    if (worldSection == 5) {
      if (ImGui::Checkbox("Enable Terrain", &ctx.terrainSettings.enabled)) {
        if (ctx.terrainSettings.enabled) {
          applyTerrainGeneratorDefaults(ctx.terrainSettings);
          ctx.terrainSystem.init(ctx.terrainSettings, ctx.scene, &ctx.assets);
        } else {
          ctx.terrainSystem.shutdown();
        }
      }
      if (ctx.terrainSettings.enabled) {
        ImGui::Separator();
        bool needsRegen = false;
        bool needsApply = false;

        int seed = (int)ctx.terrainSettings.seed;
        if (ImGui::InputInt("Seed", &seed)) {
          ctx.terrainSettings.seed = (uint32_t)seed;
          needsRegen = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Random")) {
          ctx.terrainSettings.seed = (uint32_t)rand();
          needsRegen = true;
        }
        if (ImGui::SliderFloat("Height", &ctx.terrainSettings.heightScale, 1.0f,
                               80.0f))
          needsRegen = true;
        if (ImGui::SliderFloat("Frequency", &ctx.terrainSettings.noiseFrequency,
                               0.001f, 0.1f))
          needsRegen = true;
        if (ImGui::SliderFloat("Landscape Scale",
                               &ctx.terrainSettings.landscapeScale, 0.5f,
                               4.0f, "%.2f"))
          needsRegen = true;
        if (ImGui::SliderFloat("Macro Strength",
                               &ctx.terrainSettings.macroStrength, 0.0f, 2.5f,
                               "%.2f"))
          needsRegen = true;
        if (ImGui::SliderFloat("Mountain Span",
                               &ctx.terrainSettings.mountainSpan, 0.5f, 2.5f,
                               "%.2f"))
          needsRegen = true;
        if (ImGui::SliderFloat("Valley Span", &ctx.terrainSettings.valleySpan,
                               0.5f, 2.5f, "%.2f"))
          needsRegen = true;
        if (ImGui::SliderInt("View Dist", &ctx.terrainSettings.viewDistance, 1,
                             24))
          needsRegen = true;
        if (ImGui::SliderInt("Chunk Jobs",
                             &ctx.terrainSettings.maxConcurrentChunkJobs, 1,
                             64))
          needsRegen = true;
        if (ImGui::SliderInt("Chunk Loads/Update",
                             &ctx.terrainSettings.maxChunkLoadsPerUpdate, 1,
                             128))
          needsRegen = true;
        ImGui::SliderInt("Chunk Uploads/Frame",
                         &ctx.terrainSettings.maxChunkUploadsPerFrame, 1, 16);
        if (ImGui::SliderInt("Collision Radius",
                             &ctx.terrainSettings.collisionChunkRadius, 0, 3))
          needsRegen = true;

        ImGui::SeparatorText("Terrain Performance");
        if (ImGui::Button("Huge Terrain 60 FPS")) {
          ctx.terrainSettings.useGpuTerrain = true;
          ctx.terrainSettings.enableHorizonCulling = true;
          ctx.terrainSettings.horizonCullingSectors = 256;
          ctx.terrainSettings.viewDistance = 18;
          ctx.terrainSettings.terrainGpuPageCapacity = 1536;
          ctx.terrainSettings.terrainWorkerThreads = 4;
          ctx.terrainSettings.maxCompletedChunksPerFrame = 4;
          ctx.terrainSettings.maxGpuUploadBytesPerFrame = 8 * 1024 * 1024;
          ctx.terrainSettings.collisionUpdatesPerFrame = 2;
          ctx.terrainSettings.collisionChunkRadius = 1;
          ctx.terrainSettings.meshVegetationDistance = 220;
          ctx.terrainSettings.impostorVegetationDistance = 1200;
          ctx.terrainSettings.vegetationShadowDistance = 260;
          ctx.terrainSettings.materialQuality =
              TerrainMaterialQuality::Balanced;
          needsRegen = true;
        }
        if (ImGui::Checkbox("GPU Terrain",
                            &ctx.terrainSettings.useGpuTerrain))
          needsRegen = true;
        if (ImGui::Checkbox("Horizon Culling",
                            &ctx.terrainSettings.enableHorizonCulling))
          needsApply = true;
        if (ImGui::SliderInt("Horizon Sectors",
                             &ctx.terrainSettings.horizonCullingSectors, 32,
                             2048))
          needsApply = true;
        if (ImGui::SliderInt("GPU Page Capacity",
                             &ctx.terrainSettings.terrainGpuPageCapacity, 128,
                             4096))
          needsRegen = true;
        if (ImGui::SliderInt("Terrain Workers",
                             &ctx.terrainSettings.terrainWorkerThreads, 1, 16))
          needsApply = true;
        if (ImGui::SliderInt("Completed Chunks/Frame",
                             &ctx.terrainSettings.maxCompletedChunksPerFrame,
                             1, 16))
          needsApply = true;
        int uploadMb =
            std::max(1, ctx.terrainSettings.maxGpuUploadBytesPerFrame /
                            (1024 * 1024));
        if (ImGui::SliderInt("GPU Upload MB/Frame", &uploadMb, 1, 64)) {
          ctx.terrainSettings.maxGpuUploadBytesPerFrame =
              uploadMb * 1024 * 1024;
          needsApply = true;
        }
        if (ImGui::SliderInt("Collision Updates/Frame",
                             &ctx.terrainSettings.collisionUpdatesPerFrame, 0,
                             8))
          needsApply = true;
        if (ImGui::SliderInt("Mesh Vegetation Distance",
                             &ctx.terrainSettings.meshVegetationDistance, 40,
                             800))
          needsRegen = true;
        if (ImGui::SliderInt("Impostor Vegetation Distance",
                             &ctx.terrainSettings.impostorVegetationDistance,
                             200, 3000))
          needsRegen = true;
        if (ImGui::SliderInt("Vegetation Shadow Distance",
                             &ctx.terrainSettings.vegetationShadowDistance, 0,
                             800))
          needsRegen = true;
        const char *terrainQualityItems[] = {"Ultra", "High", "Balanced",
                                             "Performance"};
        int terrainQuality = (int)ctx.terrainSettings.materialQuality;
        if (ImGui::Combo("Terrain Material Quality", &terrainQuality,
                         terrainQualityItems, IM_ARRAYSIZE(terrainQualityItems))) {
          ctx.terrainSettings.materialQuality =
              (TerrainMaterialQuality)std::clamp(terrainQuality, 0, 3);
          needsApply = true;
        }
        ImGui::TextDisabled(
            "GPU mode draws shared LOD grids by bucket instead of one OBJ mesh per chunk.");

        if (ImGui::SliderInt("Octaves", &ctx.terrainSettings.octaves, 1, 10))
          needsRegen = true;
        if (ImGui::Checkbox("Ridge Noise", &ctx.terrainSettings.useRidgeNoise))
          needsRegen = true;
        if (ImGui::Checkbox("Single Biome",
                            &ctx.terrainSettings.singleBiomeOnly))
          needsRegen = true;
        ImGui::TextDisabled(
            "Push scale first, then tune span to shape one dominant massif.");
        ImGui::TextDisabled(
            "Streaming caps and nearby-only collision keep huge terrains stable.");

        ImGui::Separator();
        ImGui::Text("Biomes");
        if (ImGui::SliderFloat("Biome Scale", &ctx.terrainSettings.biomeScale,
                               0.001f, 0.02f, "%.4f"))
          needsRegen = true;
        if (ImGui::SliderFloat("Tree Density", &ctx.terrainSettings.treeDensity,
                               0.0f, 1.0f))
          needsRegen = true;
        if (ImGui::SliderInt("Interactive Tree Radius",
                             &ctx.terrainSettings.interactiveTreeChunkRadius,
                             0, 3))
          needsRegen = true;
        if (ImGui::SliderFloat("Interactive Tree Ratio",
                               &ctx.terrainSettings.interactiveTreeRatio, 0.0f,
                               1.0f, "%.2f"))
          needsRegen = true;
        if (ImGui::SliderInt("Interactive Trees Per Chunk",
                             &ctx.terrainSettings.maxInteractiveTreesPerChunk,
                             0, 128))
          needsRegen = true;
        if (ImGui::SliderFloat("Sea Level", &ctx.terrainSettings.seaLevel,
                               -10.0f, 0.0f))
          needsRegen = true;
        if (ImGui::SliderFloat("Rock Density", &ctx.terrainSettings.rockDensity,
                               0.0f, 1.0f))
          needsRegen = true;
        if (ImGui::SliderFloat("Rock Size", &ctx.terrainSettings.rockScale,
                               0.2f, 3.0f))
          needsRegen = true;
        if (ImGui::SliderFloat("Grass Density",
                               &ctx.terrainSettings.grassDensity, 0.0f, 1.0f))
          needsRegen = true;
        if (ImGui::SliderFloat("Grass Size", &ctx.terrainSettings.grassScale,
                               0.2f, 3.0f))
          needsRegen = true;

        if (ImGui::Checkbox("Flip Custom Grass",
                            &ctx.terrainSettings.flipCustomGrass))
          needsRegen = true;
        if (ImGui::Checkbox("Spawn Vegetation",
                            &ctx.terrainSettings.spawnVegetation))
          needsRegen = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("Spawn Rocks", &ctx.terrainSettings.spawnRocks))
          needsRegen = true;
        if (ImGui::Checkbox("Spawn Water", &ctx.terrainSettings.spawnWater))
          needsRegen = true;
        ImGui::TextDisabled(
            "Visual trees stay dense; only a nearby subset gets physics/chopping.");

          ImGui::Separator();
          ImGui::Text("Brush Tools");
          ImGui::Checkbox("Enable Brush", &ctx.terrainBrush.enabled);
          if (ctx.terrainBrush.enabled) {
            const char *modes[] = {"Raise Height", "Lower Height",
                                   "Add Vegetation", "Remove Vegetation"};
            ImGui::Combo("Brush Mode", &ctx.terrainBrush.mode, modes,
                         IM_ARRAYSIZE(modes));
            ImGui::SliderFloat("Brush Radius", &ctx.terrainBrush.radius, 1.0f,
                               30.0f, "%.1f");
            ImGui::SliderFloat("Brush Strength",
                               &ctx.terrainBrush.strength, 0.1f, 20.0f, "%.2f");
            if (ctx.terrainBrush.mode >= 2) {
              const char *targets[] = {"Tree", "Rock", "Grass"};
              ImGui::Combo("Brush Target", &ctx.terrainBrush.target, targets,
                           IM_ARRAYSIZE(targets));
              ImGui::SliderInt("Scatter Count",
                               &ctx.terrainBrush.scatterCount, 1, 30);
            }
            ImGui::TextDisabled(
                "Tip: Hold LMB on terrain to paint. Brush uses loaded chunks.");
          }

          ImGui::Separator();
          ImGui::Text("Terrain Material");
        ImGui::Checkbox("Enable Custom Material",
                        &ctx.terrainMaterial.enableCustom);
        if (ctx.terrainMaterial.enableCustom) {
          ImGui::TextDisabled(
              "Default mode uses procedural biome material. Enable Flat Green "
              "only for a stylized/simple preview.");
          if (ImGui::Button("Reset Procedural Material Defaults")) {
            ctx.terrainMaterial = TerrainMaterialSettings{};
          }
          ImGui::SliderFloat("Macro Scale##TerrainMat",
                             &ctx.terrainMaterial.macroScale, 0.01f, 0.20f,
                             "%.3f");
          ImGui::SliderFloat("Detail Scale##TerrainMat",
                             &ctx.terrainMaterial.detailScale, 0.25f, 3.0f,
                             "%.2f");
          ImGui::SliderFloat("Normal Detail##TerrainMat",
                             &ctx.terrainMaterial.normalDetailScale, 0.5f, 4.0f,
                             "%.2f");
          ImGui::SliderFloat("Normal Strength##TerrainMat",
                             &ctx.terrainMaterial.normalStrength, 0.0f, 2.0f,
                             "%.2f");
          ImGui::SliderFloat("Macro Variation##TerrainMat",
                             &ctx.terrainMaterial.macroVariationStrength, 0.0f,
                             0.6f, "%.2f");
          ImGui::SliderFloat("Cliff Desat##TerrainMat",
                             &ctx.terrainMaterial.cliffDesatStrength, 0.0f,
                             0.8f, "%.2f");

          ImGui::Text("Blend Ranges");
          ImGui::SliderFloat("Cliff Start##TerrainMat",
                             &ctx.terrainMaterial.cliffStart, 0.0f, 1.0f,
                             "%.2f");
          ImGui::SliderFloat("Cliff End##TerrainMat",
                             &ctx.terrainMaterial.cliffEnd, 0.01f, 1.0f,
                             "%.2f");
          ImGui::SliderFloat("Snow Start##TerrainMat",
                             &ctx.terrainMaterial.snowStartHeight, -20.0f,
                             60.0f, "%.1f");
          ImGui::SliderFloat("Snow End##TerrainMat",
                             &ctx.terrainMaterial.snowEndHeight, -20.0f, 80.0f,
                             "%.1f");
          ImGui::SliderFloat("Low Start##TerrainMat",
                             &ctx.terrainMaterial.lowStartHeight, -20.0f, 20.0f,
                             "%.1f");
          ImGui::SliderFloat("Low End##TerrainMat",
                             &ctx.terrainMaterial.lowEndHeight, -20.0f, 40.0f,
                             "%.1f");

          if (ctx.terrainMaterial.cliffEnd <= ctx.terrainMaterial.cliffStart) {
            ctx.terrainMaterial.cliffEnd =
                ctx.terrainMaterial.cliffStart + 0.01f;
          }
          if (ctx.terrainMaterial.snowEndHeight <=
              ctx.terrainMaterial.snowStartHeight) {
            ctx.terrainMaterial.snowEndHeight =
                ctx.terrainMaterial.snowStartHeight + 0.1f;
          }
          if (ctx.terrainMaterial.lowEndHeight <=
              ctx.terrainMaterial.lowStartHeight) {
            ctx.terrainMaterial.lowEndHeight =
                ctx.terrainMaterial.lowStartHeight + 0.1f;
          }

          ImGui::Text("Layer Colors");
          ImGui::ColorEdit3("Grass A##TerrainMat",
                            &ctx.terrainMaterial.grassA.x);
          ImGui::ColorEdit3("Grass B##TerrainMat",
                            &ctx.terrainMaterial.grassB.x);
          ImGui::ColorEdit3("Dirt A##TerrainMat", &ctx.terrainMaterial.dirtA.x);
          ImGui::ColorEdit3("Dirt B##TerrainMat", &ctx.terrainMaterial.dirtB.x);
          ImGui::ColorEdit3("Rock A##TerrainMat", &ctx.terrainMaterial.rockA.x);
          ImGui::ColorEdit3("Rock B##TerrainMat", &ctx.terrainMaterial.rockB.x);
          ImGui::ColorEdit3("Sand A##TerrainMat", &ctx.terrainMaterial.sandA.x);
          ImGui::ColorEdit3("Sand B##TerrainMat", &ctx.terrainMaterial.sandB.x);
          ImGui::ColorEdit3("Snow A##TerrainMat", &ctx.terrainMaterial.snowA.x);
          ImGui::ColorEdit3("Snow B##TerrainMat", &ctx.terrainMaterial.snowB.x);

          ImGui::Text("Layer Roughness");
          ImGui::SliderFloat("Grass Roughness##TerrainMat",
                             &ctx.terrainMaterial.roughGrass, 0.0f, 1.0f,
                             "%.2f");
          ImGui::SliderFloat("Dirt Roughness##TerrainMat",
                             &ctx.terrainMaterial.roughDirt, 0.0f, 1.0f,
                             "%.2f");
          ImGui::SliderFloat("Rock Roughness##TerrainMat",
                             &ctx.terrainMaterial.roughRock, 0.0f, 1.0f,
                             "%.2f");
          ImGui::SliderFloat("Sand Roughness##TerrainMat",
                             &ctx.terrainMaterial.roughSand, 0.0f, 1.0f,
                             "%.2f");
          ImGui::SliderFloat("Snow Roughness##TerrainMat",
                             &ctx.terrainMaterial.roughSnow, 0.0f, 1.0f,
                             "%.2f");

          ImGui::SeparatorText("Terrain Layer Materials");
          ImGui::TextDisabled(
              "Grass Albedo is the top/green layer. Dirt Albedo is the "
              "second/slope layer.");
          ImGui::TextDisabled(
              "This mode replaces the legacy Ground Material overlay.");
          if (ImGui::Checkbox("Use Layer Textures##TerrainMat",
                              &ctx.terrainMaterial.useLayerTextures) &&
              ctx.terrainMaterial.useLayerTextures) {
            ctx.terrainMaterial.useGroundTextures = false;
            ctx.terrainMaterial.groundFullOverride = false;
            ctx.terrainMaterial.flatGreenEnabled = false;
          }
          if (ctx.terrainMaterial.useLayerTextures) {
            auto drawLayerPathField = [&](const char *label, const char *tag,
                                          std::string &value) {
              char pathBuf[512] = {};
              std::snprintf(pathBuf, sizeof(pathBuf), "%s", value.c_str());
              if (ImGui::InputText(label, pathBuf, sizeof(pathBuf))) {
                value = pathBuf;
              }
              ImGui::SameLine();
              const std::string buttonId = std::string("Browse##") + tag;
              if (ImGui::Button(buttonId.c_str())) {
                mBrowsePath = tag;
                ImGui::OpenPopup("Select Terrain Asset");
              }
            };
            drawLayerPathField("Grass Albedo##TerrainMat",
                               "TerrainGrassAlbedo",
                               ctx.terrainMaterial.grassAlbedoPath);
            drawLayerPathField("Grass Normal##TerrainMat",
                               "TerrainGrassNormal",
                               ctx.terrainMaterial.grassNormalPath);
            drawLayerPathField("Grass Roughness##TerrainMat",
                               "TerrainGrassRoughness",
                               ctx.terrainMaterial.grassRoughnessPath);
            drawLayerPathField("Dirt Albedo##TerrainMat", "TerrainDirtAlbedo",
                               ctx.terrainMaterial.dirtAlbedoPath);
            drawLayerPathField("Dirt Normal##TerrainMat", "TerrainDirtNormal",
                               ctx.terrainMaterial.dirtNormalPath);
            drawLayerPathField("Dirt Roughness##TerrainMat",
                               "TerrainDirtRoughness",
                               ctx.terrainMaterial.dirtRoughnessPath);
            drawTextureStatusRow("Grass Albedo Preview",
                                 ctx.terrainMaterial.grassAlbedoPath,
                                 TextureUsage::Color, false);
            drawTextureStatusRow("Grass Normal Preview",
                                 ctx.terrainMaterial.grassNormalPath,
                                 TextureUsage::Data, false);
            drawTextureStatusRow("Grass Roughness Preview",
                                 ctx.terrainMaterial.grassRoughnessPath,
                                 TextureUsage::Data, false);
            drawTextureStatusRow("Dirt Albedo Preview",
                                 ctx.terrainMaterial.dirtAlbedoPath,
                                 TextureUsage::Color, false);
            drawTextureStatusRow("Dirt Normal Preview",
                                 ctx.terrainMaterial.dirtNormalPath,
                                 TextureUsage::Data, false);
            drawTextureStatusRow("Dirt Roughness Preview",
                                 ctx.terrainMaterial.dirtRoughnessPath,
                                 TextureUsage::Data, false);
            ImGui::SliderFloat("Layer Texture Tiling##TerrainMat",
                               &ctx.terrainMaterial.layerTextureTiling, 0.02f,
                               2.0f, "%.3f");
            ImGui::SliderFloat("Layer Texture Strength##TerrainMat",
                               &ctx.terrainMaterial.layerTextureStrength,
                               0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Layer Normal Strength##TerrainMat",
                               &ctx.terrainMaterial.layerNormalStrength, 0.0f,
                               2.0f, "%.2f");
            ImGui::SliderFloat("Layer Roughness Strength##TerrainMat",
                               &ctx.terrainMaterial.layerRoughnessStrength,
                               0.0f, 1.0f, "%.2f");
          }

          ImGui::SeparatorText("Ground Material (Legacy Overlay)");
          if (ctx.terrainMaterial.useLayerTextures) {
            ImGui::TextDisabled(
                "Disabled while Terrain Layer Materials are active, so grass "
                "and dirt textures are not overwritten.");
          } else {
          ImGui::Checkbox("Use Ground Textures##TerrainMat",
                          &ctx.terrainMaterial.useGroundTextures);
          if (ctx.terrainMaterial.useGroundTextures) {
            auto pathExists = [](const std::string &path) {
              return !path.empty() && std::filesystem::exists(path);
            };
            auto drawPathField = [&](const char *label, const char *tag,
                                     std::string &value) {
              char pathBuf[512] = {};
              std::snprintf(pathBuf, sizeof(pathBuf), "%s", value.c_str());
              if (ImGui::InputText(label, pathBuf, sizeof(pathBuf))) {
                value = pathBuf;
              }
              ImGui::SameLine();
              const std::string buttonId = std::string("Browse##") + tag;
              if (ImGui::Button(buttonId.c_str())) {
                mBrowsePath = tag;
                ImGui::OpenPopup("Select Terrain Asset");
              }
            };
            drawPathField("Ground Albedo##TerrainMat", "TerrainGroundAlbedo",
                          ctx.terrainMaterial.groundAlbedoPath);
            drawPathField("Ground Normal##TerrainMat", "TerrainGroundNormal",
                          ctx.terrainMaterial.groundNormalPath);
            drawPathField("Ground Roughness##TerrainMat",
                          "TerrainGroundRoughness",
                          ctx.terrainMaterial.groundRoughnessPath);
            drawPathField("Ground Height##TerrainMat", "TerrainGroundHeight",
                          ctx.terrainMaterial.groundHeightPath);
            ImGui::Spacing();
            drawTextureStatusRow("Ground Albedo Preview",
                                 ctx.terrainMaterial.groundAlbedoPath,
                                 TextureUsage::Color, true);
            drawTextureStatusRow("Ground Normal Preview",
                                 ctx.terrainMaterial.groundNormalPath,
                                 TextureUsage::Data, false);
            drawTextureStatusRow("Ground Roughness Preview",
                                 ctx.terrainMaterial.groundRoughnessPath,
                                 TextureUsage::Data, false);
            drawTextureStatusRow("Ground Height Preview",
                                 ctx.terrainMaterial.groundHeightPath,
                                 TextureUsage::Data, false);
            if (!pathExists(ctx.terrainMaterial.groundAlbedoPath)) {
              ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                                 "Ground albedo is required and is currently missing.");
            }
            if (!ctx.terrainMaterial.groundNormalPath.empty() &&
                !pathExists(ctx.terrainMaterial.groundNormalPath)) {
              ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                                 "Ground normal path does not exist.");
            } else if (pathUsesLossyImage(ctx.terrainMaterial.groundNormalPath)) {
              ImGui::TextColored(
                  ImVec4(1.0f, 0.70f, 0.25f, 1.0f),
                  "Ground normal uses JPG. PNG is strongly recommended for normal maps.");
            }
            if (!ctx.terrainMaterial.groundRoughnessPath.empty() &&
                !pathExists(ctx.terrainMaterial.groundRoughnessPath)) {
              ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                                 "Ground roughness path does not exist.");
            } else if (pathUsesLossyImage(ctx.terrainMaterial.groundRoughnessPath)) {
              ImGui::TextColored(
                  ImVec4(1.0f, 0.70f, 0.25f, 1.0f),
                  "Ground roughness uses JPG. PNG is recommended for data textures.");
            }
            if (!ctx.terrainMaterial.groundHeightPath.empty() &&
                !pathExists(ctx.terrainMaterial.groundHeightPath)) {
              ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                                 "Ground height path does not exist.");
            } else if (pathUsesLossyImage(ctx.terrainMaterial.groundHeightPath)) {
              ImGui::TextColored(
                  ImVec4(1.0f, 0.70f, 0.25f, 1.0f),
                  "Ground height uses JPG. PNG is recommended for height data.");
            }
            if (pathUsesLossyImage(ctx.terrainMaterial.groundAlbedoPath)) {
              ImGui::TextColored(
                  ImVec4(0.85f, 0.85f, 0.55f, 1.0f),
                  "Ground albedo uses JPG. This is okay, but PNG keeps more detail.");
            }
            ImGui::SliderFloat("Ground Tiling##TerrainMat",
                               &ctx.terrainMaterial.groundTiling, 0.02f, 2.0f,
                               "%.3f");
            ImGui::SliderFloat("Ground Blend##TerrainMat",
                               &ctx.terrainMaterial.groundBlendStrength, 0.0f,
                               1.5f, "%.2f");
            ImGui::SliderFloat("Ground Roughness##TerrainMat",
                               &ctx.terrainMaterial.groundRoughness, 0.0f,
                               1.0f, "%.2f");
            ImGui::SliderFloat("Ground Height Strength##TerrainMat",
                               &ctx.terrainMaterial.groundHeightStrength,
                               0.0f, 1.5f, "%.2f");
            ImGui::Checkbox("Pseudo Height Fallback##TerrainMat",
                            &ctx.terrainMaterial.groundPseudoHeightEnabled);
            if (ctx.terrainMaterial.groundPseudoHeightEnabled) {
              const char *pseudoHeightSources[] = {"Albedo Luma", "Normal Map"};
              ImGui::Combo("Pseudo Height Source##TerrainMat",
                           &ctx.terrainMaterial.groundPseudoHeightSource,
                           pseudoHeightSources,
                           IM_ARRAYSIZE(pseudoHeightSources));
              if (ctx.terrainMaterial.groundPseudoHeightSource == 1 &&
                  ctx.terrainMaterial.groundNormalPath.empty()) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.65f, 0.25f, 1.0f),
                    "Normal-based pseudo height needs a ground normal texture.");
              }
              ImGui::SliderFloat("Pseudo Height Contrast##TerrainMat",
                                 &ctx.terrainMaterial.groundPseudoHeightContrast,
                                 0.25f, 4.0f, "%.2f");
              ImGui::SliderFloat("Pseudo Height Bias##TerrainMat",
                                 &ctx.terrainMaterial.groundPseudoHeightBias,
                                 -1.0f, 1.0f, "%.2f");
              if (!ctx.terrainMaterial.groundHeightPath.empty()) {
                ImGui::TextColored(
                    ImVec4(0.75f, 0.9f, 1.0f, 1.0f),
                    "A real ground height map still takes priority when present.");
              }
            }
            ImGui::Checkbox("Ground Grade##TerrainMat",
                            &ctx.terrainMaterial.groundGradeEnabled);
            if (ctx.terrainMaterial.groundGradeEnabled) {
              ImGui::SliderFloat("Ground Saturation##TerrainMat",
                                 &ctx.terrainMaterial.groundGradeSaturation,
                                 0.0f, 2.0f, "%.2f");
              ImGui::SliderFloat("Ground Contrast##TerrainMat",
                                 &ctx.terrainMaterial.groundGradeContrast,
                                 0.5f, 2.0f, "%.2f");
              ImGui::SliderFloat("Ground Gamma##TerrainMat",
                                 &ctx.terrainMaterial.groundGradeGamma,
                                 0.5f, 2.0f, "%.2f");
              ImGui::ColorEdit3("Ground Tint##TerrainMat",
                                &ctx.terrainMaterial.groundGradeTint.x);
            }
            ImGui::SliderFloat("Ground Brightness##TerrainMat",
                               &ctx.terrainMaterial.groundBrightness, 0.25f,
                               3.0f, "%.2f");
            ImGui::SliderFloat("Ground Variation##TerrainMat",
                               &ctx.terrainMaterial.groundVariationStrength,
                               0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Ground Variation Scale##TerrainMat",
                               &ctx.terrainMaterial.groundVariationScale,
                               0.005f, 0.12f, "%.3f");
            ImGui::Checkbox("Ground Overrides All Layers##TerrainMat",
                            &ctx.terrainMaterial.groundFullOverride);
            if (ctx.terrainMaterial.groundFullOverride) {
              ImGui::TextColored(
                  ImVec4(0.75f, 0.9f, 1.0f, 1.0f),
                  "Full override mode ignores procedural grass/dirt/rock/sand/snow colors.");
            }
          }
          }

          ImGui::SeparatorText("Terrain Sun Glint");
          ImGui::TextDisabled(
              "Directional highlight only. It does not move the real sun or "
              "change shadows.");
          ImGui::Checkbox("Sun Glint##TerrainMat",
                          &ctx.terrainMaterial.sunGlintEnabled);
          if (ctx.terrainMaterial.sunGlintEnabled) {
            ImGui::Checkbox("Use Scene Sun Direction##TerrainMat",
                            &ctx.terrainMaterial.sunGlintUseSceneSun);
            ImGui::TextDisabled(
                ctx.terrainMaterial.sunGlintUseSceneSun
                    ? "Glint now follows the actual sun direction used by the sky and shadows."
                    : "Manual glint direction is active for art-directed highlights.");
            ImGui::BeginDisabled(ctx.terrainMaterial.sunGlintUseSceneSun);
            if (ImGui::Button("Randomize Glint Direction##TerrainMat")) {
              const float t = static_cast<float>(ImGui::GetTime());
              ctx.terrainMaterial.sunGlintDirectionAzimuth =
                  std::fmod(t * 137.50777f + 41.0f, 360.0f);
              ctx.terrainMaterial.sunGlintDirectionElevation =
                  14.0f + std::fmod(t * 31.0f + 9.0f, 30.0f);
            }
            ImGui::SliderFloat("Glint Azimuth##TerrainMat",
                               &ctx.terrainMaterial.sunGlintDirectionAzimuth,
                               0.0f, 360.0f, "%.1f deg");
            ImGui::SliderFloat("Glint Elevation##TerrainMat",
                               &ctx.terrainMaterial.sunGlintDirectionElevation,
                               3.0f, 65.0f, "%.1f deg");
            ImGui::EndDisabled();
            ImGui::SliderFloat("Glint Intensity##TerrainMat",
                               &ctx.terrainMaterial.sunGlintIntensity, 0.0f,
                               4.0f, "%.2f");
            ImGui::SliderFloat("Glint Sharpness##TerrainMat",
                               &ctx.terrainMaterial.sunGlintSharpness, 8.0f,
                               180.0f, "%.1f");
            ImGui::SliderFloat("Glint Band Width##TerrainMat",
                               &ctx.terrainMaterial.sunGlintBandWidth, 0.03f,
                               0.75f, "%.2f");
            ImGui::SliderFloat("Glint Patch Scale##TerrainMat",
                               &ctx.terrainMaterial.sunGlintMaskScale, 0.002f,
                               0.08f, "%.3f");
            ImGui::SliderFloat("Glint Patchiness##TerrainMat",
                               &ctx.terrainMaterial.sunGlintMaskStrength, 0.0f,
                               1.0f, "%.2f");
            ImGui::SliderFloat("Base Spec Cut##TerrainMat",
                               &ctx.terrainMaterial.sunGlintBaseSpecular, 0.0f,
                               0.5f, "%.2f");
          }

          ImGui::SeparatorText("Stylized Override");
          ImGui::Checkbox("Flat Green Terrain (Simple Preview)##TerrainMat",
                          &ctx.terrainMaterial.flatGreenEnabled);
          if (ctx.terrainMaterial.flatGreenEnabled &&
              ctx.terrainMaterial.useGroundTextures) {
            ImGui::TextColored(
                ImVec4(0.95f, 0.75f, 0.25f, 1.0f),
                "Ground textures take priority over Flat Green during rendering.");
          }
          if (ctx.terrainMaterial.flatGreenEnabled) {
            ImGui::ColorEdit3("Flat Green Color##TerrainMat",
                              &ctx.terrainMaterial.flatGreenColor.x);
          }
        }

        ImGui::Separator();
        ImGui::Text("Custom Terrain Models (.obj/.fbx)");
        ImGui::TextDisabled(
            "Ground terrain remains procedural. Use Grass/Ground Cover for "
            "custom ground clutter.");

        auto drawTerrainModelField = [&](const char *label, const char *tag,
                                         std::string &value, bool &regenFlag) {
          char pathBuf[512] = {};
          std::snprintf(pathBuf, sizeof(pathBuf), "%s", value.c_str());
          if (ImGui::InputText(label, pathBuf, sizeof(pathBuf))) {
            value = pathBuf;
            regenFlag = true;
          }
          ImGui::SameLine();
          const std::string buttonId = std::string("Browse##") + tag;
          if (ImGui::Button(buttonId.c_str())) {
            mBrowsePath = tag;
            ImGui::OpenPopup("Select Terrain Asset");
          }
        };

        drawTerrainModelField("Tree Model", "TerrainTree",
                              ctx.terrainSettings.customTreeModelPath,
                              needsRegen);
        drawTerrainModelField("Rock Model", "TerrainRock",
                              ctx.terrainSettings.customRockModelPath,
                              needsRegen);
        drawTerrainModelField("Grass/Ground Cover Model", "TerrainGrass",
                              ctx.terrainSettings.customGrassModelPath,
                              needsRegen);
        drawTerrainModelField("Flower Model", "TerrainFlower",
                              ctx.terrainSettings.customFlowerModelPath,
                              needsRegen);
        drawTerrainModelField("Cactus Model", "TerrainCactus",
                              ctx.terrainSettings.customCactusModelPath,
                              needsRegen);
        drawTerrainModelField("Dead Tree Model", "TerrainDeadTree",
                              ctx.terrainSettings.customDeadTreeModelPath,
                              needsRegen);

        ImGui::Separator();

        if (needsRegen) {
          ctx.terrainSystem.applySettings(ctx.terrainSettings);
          ctx.terrainSystem.regenerate();
        } else if (needsApply) {
          ctx.terrainSystem.applySettings(ctx.terrainSettings);
        }
        ImGui::SameLine();
        if (ImGui::Button("Focus Terrain")) {
          ctx.editorCamera.focusOn(glm::vec3(0.0f, 0.0f, 0.0f));
          ctx.editorCamera.setDistanceInstant(80.0f);
          ctx.editorCamera.pitch = 35.0f;
        }
        ImGui::Separator();
        ImGui::Text("Stats");
        auto &ts = ctx.terrainSystem.stats();
        ImGui::Text("Chunks: %d | Trees: %d | Rocks: %d | Water: %d",
                    ts.loadedChunks, ts.totalTreeEntities, ts.totalRockEntities,
                    ts.totalWaterPlanes);
        ImGui::Text("Verts: %d | Tris: %d", ts.verticesGenerated,
                    ts.trianglesGenerated);
        ImGui::Text("GPU Terrain: %s | Pages: %d | Draw Calls: %d",
                    ts.gpuTerrainActive ? "On" : "Off",
                    ts.terrainGpuPagesUsed, ts.terrainDrawCalls);
        ImGui::Text("Terrain GPU Uploads: %d | Skipped: %d | %.2f KB",
                    ts.terrainGpuInstanceUploads,
                    ts.terrainGpuInstanceUploadSkips,
                    ts.terrainGpuInstanceUploadBytes / 1024.0f);
        ImGui::Text("Visible: %d | Frustum Culled: %d | Horizon Culled: %d",
                    ts.visibleChunks, ts.frustumCulledChunks,
                    ts.horizonCulledChunks);
        ImGui::Text("Pending Jobs: %d | Pending Uploads: %d | Collision: %d",
                    ts.pendingJobs, ts.pendingUploads, ts.collisionBodies);
      }
    }
    if (ImGui::BeginPopupModal("Select Terrain Asset", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("Browse Assets Directory:");
      ImGui::InputText("##Search", mAssetSearch, sizeof(mAssetSearch));

      ImGui::BeginChild("AssetList", ImVec2(400, 300), true);

      const bool browsingTerrainTexture =
          (mBrowsePath == "TerrainGroundAlbedo" ||
           mBrowsePath == "TerrainGroundNormal" ||
           mBrowsePath == "TerrainGroundRoughness" ||
           mBrowsePath == "TerrainGroundHeight" ||
           mBrowsePath == "TerrainGrassAlbedo" ||
           mBrowsePath == "TerrainGrassNormal" ||
           mBrowsePath == "TerrainGrassRoughness" ||
           mBrowsePath == "TerrainDirtAlbedo" ||
           mBrowsePath == "TerrainDirtNormal" ||
           mBrowsePath == "TerrainDirtRoughness");
      const bool browsingSkyHDR = (mBrowsePath == "SkyHDR");
      const bool browsingAudio =
          (mBrowsePath == "AudioAmbient" || mBrowsePath == "AudioFootsteps");

      std::vector<std::string> exts =
          browsingAudio
              ? std::vector<std::string>{".ogg", ".wav", ".mp3", ".flac"}
              : browsingSkyHDR
              ? std::vector<std::string>{".hdr"}
              : browsingTerrainTexture
              ? std::vector<std::string>{".png", ".jpg", ".jpeg", ".tga",
                                         ".bmp"}
              : std::vector<std::string>{".obj", ".fbx"};

      std::vector<std::filesystem::path> assetRoots;
      if (std::filesystem::exists("Assets"))
        assetRoots.emplace_back("Assets");
      if (std::filesystem::exists("assets"))
        assetRoots.emplace_back("assets");

      for (const auto &root : assetRoots) {
        for (auto &p : std::filesystem::recursive_directory_iterator(root)) {
          if (!p.is_regular_file())
            continue;

          std::string ext = p.path().extension().string();
          for (auto &c : ext)
            c = (char)std::tolower((unsigned char)c);

          bool match = false;
          for (const auto &validExt : exts) {
            if (ext == validExt) {
              match = true;
              break;
            }
          }
          if (!match)
            continue;

          std::string pathStr = p.path().string();
          if (mAssetSearch[0] != '\0' &&
              pathStr.find(mAssetSearch) == std::string::npos) {
            continue;
          }

          if (ImGui::Selectable(pathStr.c_str())) {
            if (mBrowsePath == "TerrainTree") {
              ctx.terrainSettings.customTreeModelPath = pathStr;
            } else if (mBrowsePath == "TerrainRock") {
              ctx.terrainSettings.customRockModelPath = pathStr;
            } else if (mBrowsePath == "TerrainGrass") {
              ctx.terrainSettings.customGrassModelPath = pathStr;
            } else if (mBrowsePath == "TerrainFlower") {
              ctx.terrainSettings.customFlowerModelPath = pathStr;
            } else if (mBrowsePath == "TerrainCactus") {
              ctx.terrainSettings.customCactusModelPath = pathStr;
            } else if (mBrowsePath == "TerrainDeadTree") {
              ctx.terrainSettings.customDeadTreeModelPath = pathStr;
            } else if (mBrowsePath == "TerrainGroundAlbedo") {
              ctx.terrainMaterial.groundAlbedoPath = pathStr;
            } else if (mBrowsePath == "TerrainGroundNormal") {
              ctx.terrainMaterial.groundNormalPath = pathStr;
            } else if (mBrowsePath == "TerrainGroundRoughness") {
              ctx.terrainMaterial.groundRoughnessPath = pathStr;
            } else if (mBrowsePath == "TerrainGroundHeight") {
              ctx.terrainMaterial.groundHeightPath = pathStr;
            } else if (mBrowsePath == "TerrainGrassAlbedo") {
              ctx.terrainMaterial.grassAlbedoPath = pathStr;
            } else if (mBrowsePath == "TerrainGrassNormal") {
              ctx.terrainMaterial.grassNormalPath = pathStr;
            } else if (mBrowsePath == "TerrainGrassRoughness") {
              ctx.terrainMaterial.grassRoughnessPath = pathStr;
            } else if (mBrowsePath == "TerrainDirtAlbedo") {
              ctx.terrainMaterial.dirtAlbedoPath = pathStr;
            } else if (mBrowsePath == "TerrainDirtNormal") {
              ctx.terrainMaterial.dirtNormalPath = pathStr;
            } else if (mBrowsePath == "TerrainDirtRoughness") {
              ctx.terrainMaterial.dirtRoughnessPath = pathStr;
            } else if (mBrowsePath == "SkyHDR") {
              ctx.skyHDRPath = pathStr;
            } else if (mBrowsePath == "AudioAmbient") {
              ctx.audio.ambientPath = pathStr;
            } else if (mBrowsePath == "AudioFootsteps") {
              ctx.audio.footstepPath = pathStr;
            }

            if (!browsingTerrainTexture && !browsingAudio &&
                mBrowsePath.rfind("Terrain", 0) == 0) {
              ctx.terrainSystem.applySettings(ctx.terrainSettings);
              ctx.terrainSystem.regenerate();
            }
            ImGui::CloseCurrentPopup();
          }
        }
      }
      ImGui::EndChild();

      if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
    ImGui::EndChild();
  }
  ImGui::End();
}

// =============================================================================
// Log Console
// =============================================================================
void EditorUI::drawLog(EditorContext &ctx) {
  // ── Toolbar ──
  if (ImGui::Button("Clear")) {
    // Logger doesn't support clear — just scroll to bottom
  }
  ImGui::SameLine();
  ImGui::Checkbox("Auto-scroll", &mConsoleAutoScroll);
  ImGui::SameLine();

  // Level filter toggles
  ImGui::PushStyleColor(ImGuiCol_Button, mFilterInfo
                                             ? ImVec4(0.2f, 0.5f, 0.8f, 1)
                                             : ImVec4(0.2f, 0.2f, 0.2f, 1));
  if (ImGui::SmallButton("Info"))
    mFilterInfo = !mFilterInfo;
  ImGui::PopStyleColor();
  ImGui::SameLine();

  ImGui::PushStyleColor(ImGuiCol_Button, mFilterWarn
                                             ? ImVec4(0.9f, 0.8f, 0.2f, 1)
                                             : ImVec4(0.2f, 0.2f, 0.2f, 1));
  if (ImGui::SmallButton("Warn"))
    mFilterWarn = !mFilterWarn;
  ImGui::PopStyleColor();
  ImGui::SameLine();

  ImGui::PushStyleColor(ImGuiCol_Button, mFilterError
                                             ? ImVec4(0.9f, 0.3f, 0.3f, 1)
                                             : ImVec4(0.2f, 0.2f, 0.2f, 1));
  if (ImGui::SmallButton("Error"))
    mFilterError = !mFilterError;
  ImGui::PopStyleColor();

  ImGui::SameLine();
  ImGui::SetNextItemWidth(200);
  ImGui::InputText("##ConsoleSearch", mConsoleSearch, sizeof(mConsoleSearch));
  ImGui::SameLine();
  ImGui::TextDisabled("Search");

  ImGui::Separator();

  // ── Log entries ──
  ImVec2 avail = ImGui::GetContentRegionAvail();
  float footerHeight = ImGui::GetFrameHeightWithSpacing() * 2.2f;
  float logHeight = std::max(0.0f, avail.y - footerHeight);
  ImGui::BeginChild("LogEntries", ImVec2(0, logHeight), false,
                    ImGuiWindowFlags_HorizontalScrollbar);
  const auto entries = Logger::instance().recentEntries(200);
  for (const auto &e : entries) {
    // Level filter
    if (e.level == Logger::Level::Info && !mFilterInfo)
      continue;
    if (e.level == Logger::Level::Warn && !mFilterWarn)
      continue;
    if (e.level == Logger::Level::Error && !mFilterError)
      continue;
    if (e.level == Logger::Level::Fatal && !mFilterError)
      continue;

    // Search filter
    if (mConsoleSearch[0] != '\0') {
      if (e.message.find(mConsoleSearch) == std::string::npos &&
          e.category.find(mConsoleSearch) == std::string::npos)
        continue;
    }

    ImVec4 color = ImVec4(0.75f, 0.75f, 0.78f, 1.0f); // Default dim text
    const char *levelTag = "[INFO]";
    if (e.level == Logger::Level::Warn) {
      color = ImVec4(0.95f, 0.78f, 0.2f, 1);
      levelTag = "[WARN]";
    }
    if (e.level == Logger::Level::Error) {
      color = ImVec4(0.92f, 0.3f, 0.3f, 1);
      levelTag = "[ERR] ";
    }
    if (e.level == Logger::Level::Fatal) {
      color = ImVec4(1.0f, 0.15f, 0.15f, 1);
      levelTag = "[FATAL]";
    }

    ImGui::TextColored(color, "%s [%s] %s", levelTag, e.category.c_str(),
                       e.message.c_str());
  }

  if (mConsoleAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10)
    ImGui::SetScrollHereY(1.0f);

  ImGui::EndChild();

  ImGui::Separator();
  ImGui::TextDisabled("Command");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-80.0f);
  ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue |
                                   ImGuiInputTextFlags_CallbackHistory;
  bool submitted =
      ImGui::InputText("##ConsoleInput", mConsoleInput, sizeof(mConsoleInput),
                       inputFlags, &EditorUI::consoleInputCallback, this);
  ImGui::SameLine();
  if (ImGui::Button("Run")) {
    submitted = true;
  }
  if (submitted) {
    std::string cmd = mConsoleInput;
    cmd.erase(0, cmd.find_first_not_of(" \t\r\n"));
    cmd.erase(cmd.find_last_not_of(" \t\r\n") + 1);
    if (!cmd.empty()) {
      mPendingConsoleCommands.push_back(cmd);
      mConsoleHistory.push_back(cmd);
      mConsoleHistoryPos = -1;
    }
    mConsoleInput[0] = '\0';
    ImGui::SetKeyboardFocusHere(-1);
  }
  ImGui::TextDisabled(
      "Examples: help, spawn Assets/tree.obj, set time 0.7, teleport 0 6 0");
}

// =============================================================================
// Statistics
// =============================================================================
void EditorUI::drawStats(EditorContext &ctx) {
  ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse;
  if (mLockLayout)
    wf |= ImGuiWindowFlags_NoMove;
  if (ImGui::Begin("Statistics", &mShowStats, wf)) {
    const FramePerformanceSnapshot &perf = ctx.performance;
    const float fps = perf.fps;
    const float ms = perf.frameMs;

    // ── FPS History Graph ──
    mFpsHistory[mFpsHistoryIdx] = fps;
    mFpsHistoryIdx = (mFpsHistoryIdx + 1) % kFpsHistorySize;

    char overlay[32];
    std::snprintf(overlay, sizeof(overlay), "%.1f FPS", fps);
    ImGui::PlotLines("##FPS", mFpsHistory, kFpsHistorySize, mFpsHistoryIdx,
                     overlay, 0.0f, 240.0f, ImVec2(0, 50));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Stats Table ──
    if (ImGui::BeginTable("StatsTable", 2, ImGuiTableFlags_SizingStretchProp)) {
      auto row = [](const char *label, const char *fmt, auto val) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", label);
        ImGui::TableNextColumn();
        char buf[64];
        std::snprintf(buf, sizeof(buf), fmt, val);
        ImGui::Text("%s", buf);
      };

      row("Frame Time", "%.2f ms", ms);
      row("Entities", "%d", ctx.entityCount);
      row("Particles", "%d", ctx.particleCount);
      row("Drawn", "%d", perf.visibleDrawn);
      row("Culled", "%d", perf.visibleCulled);
      row("Draw Calls (Main)", "%d", perf.drawCallsMain);
      row("Draw Calls (Shadow)", "%d", perf.drawCallsShadow);
      row("Instanced Calls (Main)", "%d", perf.instancedDrawCallsMain);
      row("Instanced Calls (Shadow)", "%d", perf.instancedDrawCallsShadow);
      row("Instanced Uploads (Main)", "%d", perf.instancedUploadsMain);
      row("Instanced Uploads (Shadow)", "%d", perf.instancedUploadsShadow);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted("Instanced Clusters (Main)");
      ImGui::TableNextColumn();
      ImGui::Text("%d / %d", perf.instancedClustersVisibleMain,
                  perf.instancedClustersTestedMain);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted("Instanced Clusters (Shadow)");
      ImGui::TableNextColumn();
      ImGui::Text("%d / %d", perf.instancedClustersVisibleShadow,
                  perf.instancedClustersTestedShadow);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted("Shadow Cascades Updated");
      ImGui::TableNextColumn();
      ImGui::Text("%d / %d", perf.shadowCascadesUpdated,
                  std::max(perf.shadowCascadeCount, 1));
      row("Shadow Distance Culls", "%d", perf.shadowDistanceCulled);
      row("Shadow Small-Caster Culls", "%d", perf.shadowSmallCasterCulled);
      row("Program Binds", "%d", perf.glProgramBinds);
      row("Texture Binds", "%d", perf.glTextureBinds);
      row("VAO Binds", "%d", perf.glVaoBinds);
      row("GL State Changes", "%d", perf.glStateChanges);

      const AssetStats as = ctx.assets.stats();
      row("OBJ Assets", "%u", as.objLive);
      row("Runtime OBJ", "%u", as.objRuntimeLive);
      row("GLTF Assets", "%u", as.gltfLive);
      row("FBX Assets", "%u", as.ufbxLive);
      row("Shaders", "%u", as.shaderLive);
      row("Import Queued", "%u", as.importQueued);
      row("Import Failed", "%u", as.importFailed);

      ImGui::EndTable();
    }
  }
  ImGui::End();
}

// =============================================================================
// Profiler
// =============================================================================
void EditorUI::drawProfiler(EditorContext &ctx) {
  ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse;
  if (mLockLayout)
    wf |= ImGuiWindowFlags_NoMove;
  if (ImGui::Begin("Profiler", &mShowProfiler, wf)) {
    const FramePerformanceSnapshot &perf = ctx.performance;
    ImGui::Text("Frame: %.2f ms (%.1f FPS)", perf.frameMs, perf.fps);
    ImGui::Text("GPU Frame: %.2f ms", perf.gpuFrameMs);
    ImGui::Text("GPU Shadow: %.2f ms", perf.gpuShadowMs);
    ImGui::Text("GPU Main: %.2f ms", perf.gpuMainMs);
    ImGui::Text("GPU Main Split: sky %.2f | terrain %.2f | scene %.2f | post %.2f",
                perf.gpuMainSkyMs, perf.gpuMainTerrainMs,
                perf.gpuMainSceneMs, perf.gpuMainPostMs);

    ImVec4 bottleneckColor(0.65f, 0.65f, 0.65f, 1.0f);
    switch (perf.bottleneck) {
    case PerformanceBottleneck::CpuDriver:
      bottleneckColor = ImVec4(1.0f, 0.68f, 0.25f, 1.0f);
      break;
    case PerformanceBottleneck::Gpu:
      bottleneckColor = ImVec4(0.35f, 0.85f, 0.45f, 1.0f);
      break;
    case PerformanceBottleneck::Mixed:
      bottleneckColor = ImVec4(0.9f, 0.78f, 0.35f, 1.0f);
      break;
    case PerformanceBottleneck::WithinBudget:
      bottleneckColor = ImVec4(0.45f, 0.75f, 1.0f, 1.0f);
      break;
    case PerformanceBottleneck::Collecting:
    default:
      break;
    }

    ImGui::TextColored(bottleneckColor, "Likely bottleneck: %s",
                       performanceBottleneckLabel(perf.bottleneck));
    if (perf.gpuTimerReady) {
      ImGui::Text("CPU/GPU gap: %.2f ms", std::max(0.0f, perf.cpuGpuGapMs));
    }
    ImGui::Text("Draw calls: main %d | instanced %d | terrain %d",
                perf.drawCallsMain, perf.instancedDrawCallsMain,
                perf.terrainDrawCalls);
    ImGui::Text("Terrain chunks: visible %d | pages %d | uploads %d/%d",
                perf.terrainVisibleChunks, perf.terrainGpuPagesUsed,
                perf.terrainUploads, perf.terrainUploadSkips);
    ImGui::Text("Terrain upload bytes/frame: %.2f KB",
                perf.terrainUploadBytes / 1024.0f);
    ImGui::Text("Instanced uploads/frame: main %d | shadow %d",
                perf.instancedUploadsMain, perf.instancedUploadsShadow);
    ImGui::Text("Instanced clusters: main %d/%d | shadow %d/%d",
                perf.instancedClustersVisibleMain,
                perf.instancedClustersTestedMain,
                perf.instancedClustersVisibleShadow,
                perf.instancedClustersTestedShadow);
    ImGui::Text("Shadow cascades updated: %d/%d (%s)",
                perf.shadowCascadesUpdated,
                std::max(perf.shadowCascadeCount, 1),
                perf.shadowCascadeStaggered ? "staggered" : "full");
    ImGui::Text("Shadow culls: distance %d | small casters %d",
                perf.shadowDistanceCulled, perf.shadowSmallCasterCulled);
    ImGui::Text("Submission backend: %s", perf.submissionBackendLabel.c_str());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!ctx.cpuSamples || ctx.cpuSamples->empty()) {
      ImGui::TextDisabled("No CPU samples yet.");
    } else {
      std::vector<FrameProfiler::Sample> samples = *ctx.cpuSamples;
      std::sort(samples.begin(), samples.end(),
                [](const auto &a, const auto &b) { return a.ms > b.ms; });

      if (ImGui::BeginTable("ProfilerTable", 3,
                            ImGuiTableFlags_SizingStretchProp |
                                ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("System");
        ImGui::TableSetupColumn("ms");
        ImGui::TableSetupColumn("% Frame");
        ImGui::TableHeadersRow();

        for (const auto &s : samples) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(s.name.c_str());
          ImGui::TableNextColumn();
          ImGui::Text("%.3f", s.ms);
          ImGui::TableNextColumn();
          const float pct =
              perf.frameMs > 0.0f ? (float)(s.ms / perf.frameMs * 100.0) : 0.0f;
          ImGui::Text("%.1f%%", pct);
        }

        ImGui::EndTable();
      }
    }
  }
  ImGui::End();
}

// =============================================================================
// Component Inspector — Enhanced with Add/Remove, Color XYZ, Reset, all
// editors
// =============================================================================

// Helper: Color-coded XYZ DragFloat3 (Red/Green/Blue for X/Y/Z like Unreal)
static bool DragFloat3Colored(const char *label, float *v, float speed = 0.1f,
                              float vMin = 0.0f, float vMax = 0.0f) {
  bool edited = false;
  ImGui::PushID(label);

  float fullWidth = ImGui::CalcItemWidth();
  float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
  float fieldW = (fullWidth - spacing * 2.0f) / 3.0f;

  // X — Red
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.45f, 0.12f, 0.12f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                        ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                        ImVec4(0.65f, 0.18f, 0.18f, 1.0f));
  ImGui::SetNextItemWidth(fieldW);
  edited |= ImGui::DragFloat("##X", &v[0], speed, vMin, vMax, "X: %.2f");
  ImGui::PopStyleColor(3);

  ImGui::SameLine(0, spacing);

  // Y — Green
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.40f, 0.12f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                        ImVec4(0.15f, 0.50f, 0.15f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                        ImVec4(0.18f, 0.60f, 0.18f, 1.0f));
  ImGui::SetNextItemWidth(fieldW);
  edited |= ImGui::DragFloat("##Y", &v[1], speed, vMin, vMax, "Y: %.2f");
  ImGui::PopStyleColor(3);

  ImGui::SameLine(0, spacing);

  // Z — Blue
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.12f, 0.45f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                        ImVec4(0.15f, 0.15f, 0.55f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                        ImVec4(0.18f, 0.18f, 0.65f, 1.0f));
  ImGui::SetNextItemWidth(fieldW);
  edited |= ImGui::DragFloat("##Z", &v[2], speed, vMin, vMax, "Z: %.2f");
  ImGui::PopStyleColor(3);

  ImGui::SameLine(0, spacing);
  ImGui::TextDisabled("%s", label);

  ImGui::PopID();
  return edited;
}

// Helper: Component header with right-click Remove + Reset button
static bool ComponentHeader(const char *label, bool *open, bool canRemove,
                            bool *wantsRemove, bool *wantsReset,
                            ImGuiTreeNodeFlags extraFlags = 0) {
  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen |
                             ImGuiTreeNodeFlags_Framed |
                             ImGuiTreeNodeFlags_AllowOverlap | extraFlags;

  // Reserve space for reset button
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.18f, 0.22f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                        ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
  *open = ImGui::CollapsingHeader(label, flags);
  ImGui::PopStyleColor(2);

  // Reset button on the same line
  ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
  ImGui::PushID(label);
  if (ImGui::SmallButton("R")) {
    *wantsReset = true;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Reset to defaults");
  ImGui::PopID();

  // Right-click context menu
  if (canRemove && ImGui::BeginPopupContextItem(label)) {
    if (ImGui::MenuItem("Remove Component")) {
      *wantsRemove = true;
    }
    ImGui::EndPopup();
  }

  return *open;
}

static bool DrawTextureSlotEditor(const char *label, std::string &path,
                                  GLuint &outTex) {
  bool edited = false;
  ImGui::PushID(label);

  char pathBuf[512];
  std::snprintf(pathBuf, sizeof(pathBuf), "%s", path.c_str());

  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 112.0f);
  if (ImGui::InputText("##Path", pathBuf, sizeof(pathBuf))) {
    path = pathBuf;
    outTex = path.empty() ? 0 : LoadTexture2DCached(path);
    edited = true;
  }

  ImGui::SameLine();
  if (ImGui::Button("Browse")) {
    const char *patterns[] = {"*.png", "*.jpg", "*.jpeg", "*.tga", "*.bmp"};
    const char *picked =
        tinyfd_openFileDialog("Select Texture", path.c_str(),
                              (int)(sizeof(patterns) / sizeof(patterns[0])),
                              patterns, "Texture Files", 0);
    if (picked) {
      path = picked;
      outTex = LoadTexture2DCached(path);
      edited = true;
    }
  }

  ImGui::SameLine();
  if (ImGui::Button("Clear")) {
    path.clear();
    outTex = 0;
    edited = true;
  }

  ImGui::TextDisabled("%s", label);
  if (!path.empty())
    ImGui::TextWrapped("%s", path.c_str());

  ImGui::PopID();
  return edited;
}

bool EditorUI::drawInspector(EditorContext &ctx) {
  bool edited = false;
  ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse;
  if (mLockLayout)
    wf |= ImGuiWindowFlags_NoMove;
  if (ImGui::Begin("Inspector", &mShowInspector, wf)) {
    uint32_t selectedEntityId = ctx.selection.selectedEntityId;
    auto &reg = ctx.scene.registry();
    auto &s = ctx.selection;

    // Evaluate Rename Popup eagerly before any scrolling
    if (s.renaming && selectedEntityId != 0) {
      ImGui::OpenPopup("RenameEntityPopup");
    }

    if (ImGui::BeginPopupModal("RenameEntityPopup", &s.renaming,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::InputText("Name", s.renameBuf, 128);
      if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        if (reg.has<NameComponent>(selectedEntityId)) {
          reg.get<NameComponent>(selectedEntityId).name = s.renameBuf;
          edited = true;
        }
        s.renaming = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        s.renaming = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }

    if (selectedEntityId == 0) {
      ImGui::TextDisabled("No entity selected.");
    } else {
      // ── Entity Header ──────────────────────────────────────────────────
      ImGui::Text("Entity %u", selectedEntityId);
      ImGui::SameLine();
      if (ImGui::Button("Rename")) {
        s.renaming = true;
        std::string name = "Entity " + std::to_string(selectedEntityId);
        if (reg.has<NameComponent>(selectedEntityId))
          name = reg.get<NameComponent>(selectedEntityId).name;
        std::snprintf(s.renameBuf, 128, "%s", name.c_str());
      }
      ImGui::SameLine();
      if (ImGui::Button("Delete")) {
        ctx.events.publish(DeleteEntityRequestedEvent{selectedEntityId});
      }

      ImGui::Separator();

      // ── Name ───────────────────────────────────────────────────────────
      if (reg.has<NameComponent>(selectedEntityId)) {
        auto &nc = reg.get<NameComponent>(selectedEntityId);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", nc.name.c_str());
        if (ImGui::InputText("Name", buf, sizeof(buf))) {
          nc.name = buf;
          edited = true;
        }
      }

      // ── Transform ──────────────────────────────────────────────────────
      if (reg.has<TransformComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Transform", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<TransformComponent>(selectedEntityId);
        } else {
          if (wantReset) {
            reg.get<TransformComponent>(selectedEntityId) =
                TransformComponent{};
            edited = true;
          }
          if (open) {
            auto &tr = reg.get<TransformComponent>(selectedEntityId);
            edited |= DragFloat3Colored("Position", &tr.position.x, 0.1f);
            if (DragFloat3Colored("Rotation", &tr.rotation.x, 0.5f)) {
              normalizeEulerDeg(tr.rotation);
              edited = true;
            }
            edited |=
                DragFloat3Colored("Scale", &tr.scale.x, 0.01f, 0.01f, 100.0f);
          }
        }
      }

      // ── Mesh ───────────────────────────────────────────────────────────
      if (reg.has<MeshComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Mesh", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<MeshComponent>(selectedEntityId);
        } else if (open) {
          auto &mc = reg.get<MeshComponent>(selectedEntityId);
          ImGui::Checkbox("Visible", &mc.visible);
          ImGui::Checkbox("Casts Shadow", &mc.castsShadow);

          if (mc.type == MeshComponent::AssetType::OBJ && mc.objModel) {
            ImGui::Text("Model: OBJ (%zu submeshes)",
                        mc.objModel->submeshCount());
            if (ImGui::TreeNode("Submeshes")) {
              ImGui::Checkbox("Edit selected part", &s.editObjPart);
              auto names = mc.objModel->objectNames();
              if (names.empty()) {
                ImGui::Text("No named parts.");
              } else {
                if (ImGui::BeginCombo("Select Part",
                                      s.selectedObjPartName.c_str())) {
                  for (const auto &n : names) {
                    bool is_selected = (s.selectedObjPartName == n);
                    if (ImGui::Selectable(n.c_str(), is_selected))
                      s.selectedObjPartName = n;
                    if (is_selected)
                      ImGui::SetItemDefaultFocus();
                  }
                  ImGui::EndCombo();
                }
              }
              ImGui::TreePop();
            }
          } else if (mc.type == MeshComponent::AssetType::GLTF &&
                     mc.gltfModel) {
            ImGui::Text("Model: GLTF (%zu submeshes)",
                        mc.gltfModel->submeshCount());
          } else if (mc.type == MeshComponent::AssetType::FBX && mc.ufbxModel) {
            ImGui::Text("Model: True FBX (%zu submeshes)",
                        mc.ufbxModel->submeshCount());
          } else {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Model: null");
          }
        }
      }

      // ── Material Override ───────────────────────────────────────────────
      if (reg.has<MaterialOverrideComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Material Override", &open, true, &wantRemove,
                        &wantReset);
        if (wantRemove) {
          reg.removeComponent<MaterialOverrideComponent>(selectedEntityId);
        } else {
          auto &mo = reg.get<MaterialOverrideComponent>(selectedEntityId);
          if (wantReset) {
            mo = MaterialOverrideComponent{};
            mo.material.id =
                "EntityOverride_" + std::to_string(selectedEntityId);
            edited = true;
          }
          if (open) {
            edited |= ImGui::Checkbox("Enabled##MaterialOverride", &mo.enabled);
            edited |= ImGui::ColorEdit4("Base Color", &mo.material.baseColor.x);
            edited |= ImGui::SliderFloat("Roughness", &mo.material.roughness,
                                         0.0f, 1.0f);
            edited |= ImGui::SliderFloat("Metallic", &mo.material.metallic,
                                         0.0f, 1.0f);
            edited |= ImGui::SliderFloat("AO", &mo.material.ao, 0.0f, 1.0f);

            static const char *channelLabels[] = {"R", "G", "B", "A"};
            edited |=
                ImGui::Combo("Roughness Channel", &mo.material.roughnessChannel,
                             channelLabels, 4);
            edited |=
                ImGui::Combo("Metallic Channel", &mo.material.metallicChannel,
                             channelLabels, 4);
            edited |= ImGui::Combo("AO Channel", &mo.material.aoChannel,
                                   channelLabels, 4);

            ImGui::SeparatorText("Texture Maps");
            edited |= DrawTextureSlotEditor("Albedo", mo.albedoPath,
                                            mo.material.texDiffuse);
            edited |= DrawTextureSlotEditor("Normal", mo.normalPath,
                                            mo.material.texNormal);
            edited |= DrawTextureSlotEditor("Roughness", mo.roughnessPath,
                                            mo.material.texRoughness);
            edited |= DrawTextureSlotEditor("Metallic", mo.metallicPath,
                                            mo.material.texMetallic);
            edited |= DrawTextureSlotEditor("Ambient Occlusion", mo.aoPath,
                                            mo.material.texAO);
          }
        }
      }

      // ── Rigidbody ────────────────────────────────────────────────────────
      if (reg.has<RigidbodyComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Rigidbody", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<RigidbodyComponent>(selectedEntityId);
        } else {
          if (wantReset) {
            reg.get<RigidbodyComponent>(selectedEntityId) =
                RigidbodyComponent{};
            edited = true;
          }
          if (open) {
            auto &rb = reg.get<RigidbodyComponent>(selectedEntityId);
            const char *typeItems[] = {"Static", "Kinematic", "Dynamic"};
            int currentType = (int)rb.type;
            if (ImGui::Combo("Type", &currentType, typeItems,
                             IM_ARRAYSIZE(typeItems))) {
              rb.type = (RigidbodyComponent::Type)currentType;
              edited = true;
            }
            edited |= ImGui::DragFloat("Mass", &rb.mass, 0.1f, 0.0f, 1000.0f);
            edited |=
                ImGui::DragFloat("Friction", &rb.friction, 0.05f, 0.0f, 1.0f);
            edited |= ImGui::DragFloat("Restitution", &rb.restitution, 0.05f,
                                       0.0f, 1.0f);
          }
        }
      }

      // ── Collider ─────────────────────────────────────────────────────────
      if (reg.has<ColliderComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Collider", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<ColliderComponent>(selectedEntityId);
        } else {
          if (wantReset) {
            reg.get<ColliderComponent>(selectedEntityId) = ColliderComponent{};
            edited = true;
          }
          if (open) {
            auto &col = reg.get<ColliderComponent>(selectedEntityId);
            const char *shapeItems[] = {"Box", "Sphere", "Capsule"};
            int currentShape = (int)col.shape;
            if (ImGui::Combo("Shape", &currentShape, shapeItems,
                             IM_ARRAYSIZE(shapeItems))) {
              col.shape = (ColliderComponent::Shape)currentShape;
              edited = true;
            }
            edited |=
                DragFloat3Colored("Center Offset", &col.offset.x, 0.05f);
            if (col.shape == ColliderComponent::Shape::Box) {
              edited |=
                  DragFloat3Colored("Dimensions", &col.dimensions.x, 0.1f);
            } else if (col.shape == ColliderComponent::Shape::Sphere) {
              edited |= ImGui::DragFloat("Radius", &col.dimensions.x, 0.1f);
            } else if (col.shape == ColliderComponent::Shape::Capsule) {
              edited |= ImGui::DragFloat("Height", &col.dimensions.y, 0.1f);
              edited |= ImGui::DragFloat("Radius", &col.dimensions.x, 0.1f);
            }
            ImGui::Spacing();
            if (ImGui::Button(s.editColliderBounds ? "Stop Editing Bounds"
                                                   : "Edit Bounds")) {
              s.editColliderBounds = !s.editColliderBounds;
              if (s.editColliderBounds) {
                // Ensure gizmo mode is set to scale when editing colliders to
                // make it intuitive
                s.gizmoOp = ImGuizmo::SCALE;
                s.gizmoMode = ImGuizmo::LOCAL;
              }
            }
          }
        }
      }

      if (reg.has<SpaceshipComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Spaceship", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<SpaceshipComponent>(selectedEntityId);
        } else {
          if (wantReset) {
            reg.get<SpaceshipComponent>(selectedEntityId) =
                SpaceshipComponent{};
            edited = true;
          }
          if (open) {
            auto &ship = reg.get<SpaceshipComponent>(selectedEntityId);
            edited |= ImGui::Checkbox("Enabled##Spaceship", &ship.enabled);
            ImGui::SeparatorText("Mass / Thrust");
            edited |= ImGui::DragFloat("Dry Mass (kg)", &ship.dryMassKg, 25.0f,
                                       1.0f, 1000000.0f, "%.0f");
            edited |= ImGui::DragFloat("Fuel Mass (kg)", &ship.fuelMassKg,
                                       25.0f, 0.0f, 1000000.0f, "%.0f");
            edited |= ImGui::DragFloat("Main Thrust (N)", &ship.mainThrustN,
                                       250.0f, 0.0f, 5000000.0f, "%.0f");
            edited |= ImGui::DragFloat("Boost Multiplier",
                                       &ship.boostMultiplier, 0.05f, 1.0f,
                                       10.0f, "%.2f");
            edited |= ImGui::DragFloat("Max Speed", &ship.maxSpeed, 1.0f, 1.0f,
                                       5000.0f, "%.1f");

            ImGui::SeparatorText("Flight Feel");
            edited |= ImGui::DragFloat("Turn Rate", &ship.turnRateDeg, 1.0f,
                                       0.0f, 720.0f, "%.1f deg/s");
            edited |= ImGui::DragFloat("Turn Responsiveness",
                                       &ship.turnResponsiveness, 0.1f, 0.0f,
                                       40.0f, "%.2f");
            edited |= ImGui::DragFloat("Bank Angle", &ship.bankAngleDeg, 0.5f,
                                       0.0f, 89.0f, "%.1f deg");
            edited |= ImGui::DragFloat("Bank Responsiveness",
                                       &ship.bankResponsiveness, 0.1f, 0.0f,
                                       40.0f, "%.2f");
            edited |= ImGui::DragFloat("Legacy Damping", &ship.damping, 0.001f,
                                       0.0f, 0.999f, "%.3f");
            edited |= ImGui::DragFloat("Idle Drag", &ship.idleDrag, 0.02f,
                                       0.0f, 10.0f, "%.2f");
            edited |= ImGui::DragFloat("Brake Drag", &ship.brakeDrag, 0.02f,
                                       0.0f, 20.0f, "%.2f");

            ImGui::SeparatorText("Runtime State");
            ImGui::Text("Throttle: %.2f", ship.throttle);
            ImGui::Text("Speed: %.2f", glm::length(ship.velocity));
            ImGui::Text("Yaw Rate: %.2f deg/s", ship.angularVelocity.y);
            if (ImGui::Button("Clear Velocity")) {
              ship.velocity = glm::vec3(0.0f);
              ship.angularVelocity = glm::vec3(0.0f);
              ship.throttle = 0.0f;
              edited = true;
            }
          }
        }
      }

      // ── Bounds ─────────────────────────────────────────────────────────
      // Destructible
      if (reg.has<DestructibleComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Destructible", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<DestructibleComponent>(selectedEntityId);
        } else {
          if (wantReset) {
            reg.get<DestructibleComponent>(selectedEntityId) =
                DestructibleComponent{};
            edited = true;
          }
          if (open) {
            auto &d = reg.get<DestructibleComponent>(selectedEntityId);
            edited |= ImGui::Checkbox("Enabled##Destructible", &d.enabled);
            edited |=
                ImGui::DragFloat("Health", &d.health, 1.0f, 1.0f, 10000.0f);
            edited |= ImGui::SliderInt("Shard Count", &d.shardCount, 1, 64);
            edited |=
                ImGui::DragFloat("Shard Scale", &d.shardScale, 0.02f, 0.05f,
                                 4.0f);
            edited |= ImGui::DragFloat("Explosion Force", &d.explosionForce,
                                       0.25f, 0.0f, 250.0f);
            edited |= ImGui::DragFloat("Upward Impulse", &d.upwardImpulse,
                                       0.1f, 0.0f, 100.0f);
            edited |= ImGui::Checkbox("Hide Original", &d.hideOriginal);
            ImGui::Text("State: %s", d.fractured ? "Fractured" : "Ready");

            if (!reg.has<RigidbodyComponent>(selectedEntityId) ||
                !reg.has<ColliderComponent>(selectedEntityId)) {
              ImGui::TextWrapped(
                  "Needs Rigidbody + Collider to be hit in Play mode.");
              if (ImGui::Button("Add Hit Physics")) {
                if (!reg.has<RigidbodyComponent>(selectedEntityId)) {
                  auto &rb = reg.emplace<RigidbodyComponent>(selectedEntityId);
                  rb.type = RigidbodyComponent::Type::Static;
                  rb.mass = 0.0f;
                }
                if (!reg.has<ColliderComponent>(selectedEntityId)) {
                  auto &col = reg.emplace<ColliderComponent>(selectedEntityId);
                  autoFitBoxColliderFromMesh(reg, selectedEntityId, col);
                }
                edited = true;
              }
            }

            if (ImGui::Button("Fracture Now")) {
              glm::vec3 hitPos(0.0f);
              if (reg.has<TransformComponent>(selectedEntityId))
                hitPos = reg.get<TransformComponent>(selectedEntityId).position;
              DestructionSystem::fractureEntity(
                  ctx.scene, ctx.assets, selectedEntityId, hitPos,
                  glm::normalize(glm::vec3(0.65f, 0.85f, 0.35f)), nullptr);
              edited = true;
            }
            if (d.fractured) {
              ImGui::SameLine();
              if (ImGui::Button("Reset Source")) {
                d.fractured = false;
                if (reg.has<MeshComponent>(selectedEntityId))
                  reg.get<MeshComponent>(selectedEntityId).visible = true;
                if (reg.has<LifecycleComponent>(selectedEntityId))
                  reg.get<LifecycleComponent>(selectedEntityId).state =
                      EntityLifecycleState::Alive;
                edited = true;
              }
            }
          }
        }
      }

      if (reg.has<BoundsComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Bounds", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<BoundsComponent>(selectedEntityId);
        } else {
          if (wantReset) {
            reg.get<BoundsComponent>(selectedEntityId) = BoundsComponent{};
            edited = true;
          }
          if (open) {
            auto &bc = reg.get<BoundsComponent>(selectedEntityId);
            edited |= ImGui::DragFloat("Radius##Bounds", &bc.radius, 0.1f,
                                       0.01f, 1000.0f);
          }
        }
      }

      // ── LOD ────────────────────────────────────────────────────────────
      if (reg.has<LODComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("LOD", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<LODComponent>(selectedEntityId);
        } else {
          if (wantReset) {
            reg.get<LODComponent>(selectedEntityId) = LODComponent{};
            edited = true;
          }
          if (open) {
            auto &lod = reg.get<LODComponent>(selectedEntityId);
            edited |= ImGui::DragFloat("Min Distance", &lod.minDistance, 1.0f,
                                       0.0f, lod.maxDistance);
            edited |= ImGui::DragFloat("Max Distance", &lod.maxDistance, 1.0f,
                                       lod.minDistance, 100000.0f);
          }
        }
      }

      // ── Lifecycle ──────────────────────────────────────────────────────
      if (reg.has<LifecycleComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Lifecycle", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<LifecycleComponent>(selectedEntityId);
        } else {
          if (wantReset) {
            reg.get<LifecycleComponent>(selectedEntityId) =
                LifecycleComponent{};
            edited = true;
          }
          if (open) {
            auto &lc = reg.get<LifecycleComponent>(selectedEntityId);
            const char *states[] = {"Alive", "Disabled", "PendingDestroy"};
            int stateIdx = static_cast<int>(lc.state);
            if (ImGui::Combo("State##LC", &stateIdx, states, 3)) {
              lc.state = static_cast<EntityLifecycleState>(stateIdx);
              edited = true;
            }
          }
        }
      }

      // ── Hierarchy ──────────────────────────────────────────────────────
      if (reg.has<HierarchyComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Hierarchy", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<HierarchyComponent>(selectedEntityId);
        } else {
          if (open) {
            auto &h = reg.get<HierarchyComponent>(selectedEntityId);
            int parent = static_cast<int>(h.parent);
            if (ImGui::DragInt("Parent ID", &parent, 1, 0, 100000)) {
              if (parent == 0) {
                ctx.scene.clearParent(selectedEntityId);
              } else {
                ctx.scene.setParent(selectedEntityId,
                                    static_cast<uint32_t>(parent));
              }
              edited = true;
            }
            ImGui::Text("Children: %d", (int)h.children.size());
            if (!h.children.empty() && ImGui::TreeNode("Children List")) {
              for (uint32_t child : h.children) {
                ImGui::BulletText("Entity %u", child);
              }
              ImGui::TreePop();
            }
          }
        }
      }

      // ── Camera ─────────────────────────────────────────────────────────
      if (reg.has<CameraComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Camera", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<CameraComponent>(selectedEntityId);
        } else {
          if (wantReset) {
            reg.get<CameraComponent>(selectedEntityId) = CameraComponent{};
            edited = true;
          }
          if (open) {
            auto &cam = reg.get<CameraComponent>(selectedEntityId);
            edited |=
                ImGui::DragFloat("FOV##Cam", &cam.fov, 0.5f, 10.0f, 170.0f);
            edited |= ImGui::DragFloat("Yaw##Cam", &cam.yaw, 0.5f);
            edited |=
                ImGui::DragFloat("Pitch##Cam", &cam.pitch, 0.5f, -89.0f, 89.0f);
            ImGui::Checkbox("Is Primary", &cam.isPrimary);
            ImGui::Text("Front: (%.2f, %.2f, %.2f)", cam.front.x, cam.front.y,
                        cam.front.z);
          }
        }
      }

      // ── Script ──────────────────────────────────────────────────────────
      if (reg.has<ScriptComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Script", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<ScriptComponent>(selectedEntityId);
        } else {
          if (wantReset) {
            auto &sc = reg.get<ScriptComponent>(selectedEntityId);
            sc.scriptPath.clear();
            sc.initialized = false;
            sc.envRef = -1;
            edited = true;
          }
          if (open) {
            auto &sc = reg.get<ScriptComponent>(selectedEntityId);

            // ── Script path input ──
            char pathBuf[256];
            std::snprintf(pathBuf, sizeof(pathBuf), "%s",
                          sc.scriptPath.c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70);
            if (ImGui::InputText("##ScriptPath", pathBuf, sizeof(pathBuf))) {
              sc.scriptPath = pathBuf;
              sc.initialized = false; // Reload on next frame
              edited = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse##Script")) {
              ImGui::OpenPopup("ScriptBrowserPopup");
            }

            // ── Script browser popup ──
            if (ImGui::BeginPopup("ScriptBrowserPopup")) {
              ImGui::Text("Lua Scripts:");
              ImGui::Separator();
              namespace fs = std::filesystem;
              std::string searchDir = "scripts";
              if (fs::exists(searchDir) && fs::is_directory(searchDir)) {
                for (auto const &entry : fs::directory_iterator(searchDir)) {
                  if (!entry.is_regular_file())
                    continue;
                  std::string ext = entry.path().extension().string();
                  if (ext != ".lua")
                    continue;
                  std::string name = entry.path().filename().string();
                  if (ImGui::Selectable(name.c_str())) {
                    sc.scriptPath = entry.path().string();
                    sc.initialized = false;
                    edited = true;
                  }
                }
              } else {
                ImGui::TextDisabled("No 'scripts/' directory found.");
              }
              ImGui::EndPopup();
            }

            // ── Status ──
            if (sc.scriptPath.empty()) {
              ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1),
                                 "No script attached");
            } else if (sc.initialized) {
              ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1), "Running");
            } else {
              ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.2f, 1), "Pending...");
            }

            // ── Open editor button ──
            if (!sc.scriptPath.empty()) {
              if (ImGui::Button("Edit Script")) {
                mShowScriptEditor = true;
                // Force reload in the editor window
                mScriptEditorPath = sc.scriptPath;
                mScriptEditorDirty = false;
                std::ifstream in(sc.scriptPath);
                if (in.good()) {
                  std::string content((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
                  std::snprintf(mScriptEditorBuf, sizeof(mScriptEditorBuf),
                                "%s", content.c_str());
                } else {
                  mScriptEditorBuf[0] = '\0';
                }
              }
            }
          }
        }
      }

      // ── Add Component Button ───────────────────────────────────────────
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      float buttonWidth = ImGui::GetContentRegionAvail().x;
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.15f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            ImVec4(0.20f, 0.45f, 0.20f, 1.0f));
      if (ImGui::Button("+ Add Component", ImVec2(buttonWidth, 28))) {
        ImGui::OpenPopup("AddComponentPopup");
      }
      ImGui::PopStyleColor(2);

      if (ImGui::BeginPopup("AddComponentPopup")) {
        if (!reg.has<TransformComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Transform"))
            reg.emplace<TransformComponent>(selectedEntityId);
        }
        if (!reg.has<MeshComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Mesh"))
            reg.emplace<MeshComponent>(selectedEntityId);
        }
        if (!reg.has<MaterialOverrideComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Material Override")) {
            auto &mo = reg.emplace<MaterialOverrideComponent>(selectedEntityId);
            mo.material.id =
                "EntityOverride_" + std::to_string(selectedEntityId);
          }
        }
        if (!reg.has<RigidbodyComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Rigidbody"))
            reg.emplace<RigidbodyComponent>(selectedEntityId);
        }
        if (!reg.has<ColliderComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Collider")) {
            auto &col = reg.emplace<ColliderComponent>(selectedEntityId);
            autoFitBoxColliderFromMesh(reg, selectedEntityId, col);
          }
        }
        if (!reg.has<SpaceshipComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Spaceship")) {
            auto &ship = reg.emplace<SpaceshipComponent>(selectedEntityId);
            ship.centerOfMass = glm::vec3(0.0f, -0.35f, 0.0f);
            if (!reg.has<RigidbodyComponent>(selectedEntityId)) {
              auto &rb = reg.emplace<RigidbodyComponent>(selectedEntityId);
              rb.type = RigidbodyComponent::Type::Kinematic;
              rb.mass = ship.dryMassKg + ship.fuelMassKg;
            }
          }
        }
        if (!reg.has<DestructibleComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Destructible")) {
            reg.emplace<DestructibleComponent>(selectedEntityId);
            if (!reg.has<RigidbodyComponent>(selectedEntityId)) {
              auto &rb = reg.emplace<RigidbodyComponent>(selectedEntityId);
              rb.type = RigidbodyComponent::Type::Static;
              rb.mass = 0.0f;
            }
            if (!reg.has<ColliderComponent>(selectedEntityId)) {
              auto &col = reg.emplace<ColliderComponent>(selectedEntityId);
              autoFitBoxColliderFromMesh(reg, selectedEntityId, col);
            }
          }
        }
        if (!reg.has<BoundsComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Bounds"))
            reg.emplace<BoundsComponent>(selectedEntityId);
        }
        if (!reg.has<LODComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("LOD"))
            reg.emplace<LODComponent>(selectedEntityId);
        }
        if (!reg.has<LifecycleComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Lifecycle"))
            reg.emplace<LifecycleComponent>(selectedEntityId);
        }
        if (!reg.has<HierarchyComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Hierarchy"))
            reg.emplace<HierarchyComponent>(selectedEntityId);
        }
        if (!reg.has<CameraComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Camera"))
            reg.emplace<CameraComponent>(selectedEntityId);
        }
        if (!reg.has<NameComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Name"))
            reg.emplace<NameComponent>(selectedEntityId, "Unnamed");
        }
        if (!reg.has<ScriptComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Script"))
            reg.emplace<ScriptComponent>(selectedEntityId);
        }
        ImGui::EndPopup();
      }
    }
  }
  ImGui::End();

  return edited;
}

// =============================================================================
// Gizmo & Outliner
// =============================================================================
bool EditorUI::drawGizmo(bool uiMode, const glm::mat4 &view,
                         const glm::mat4 &projection, Scene &scene, SunFX &sun,
                         EventBus &events, SelectionState &s,
                         glm::vec3 &cameraPos) {
  bool edited = false;
  if (!uiMode)
    return false;

  ImGuizmo::BeginFrame();
  ImGuizmo::SetOrthographic(false);
  ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());

  ImGuiIO &io = ImGui::GetIO();
  ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);

  auto &reg = scene.registry();

  // --- Helpers ---
  auto passFilter = [&](const char *name) -> bool {
    if (s.outlinerFilter[0] == 0)
      return true;
    std::string a = name ? name : "";
    std::string b = s.outlinerFilter;
    for (auto &c : a)
      c = (char)tolower((unsigned char)c);
    for (auto &c : b)
      c = (char)tolower((unsigned char)c);
    return a.find(b) != std::string::npos;
  };

  auto isSelected = [&](uint32_t id) {
    for (uint32_t v : s.selectedEntities)
      if (v == id)
        return true;
    return false;
  };

  auto addToSelection = [&](uint32_t id) {
    if (!isSelected(id))
      s.selectedEntities.push_back(id);
  };

  auto removeFromSelection = [&](uint32_t id) {
    for (int i = 0; i < (int)s.selectedEntities.size(); ++i) {
      if (s.selectedEntities[i] == id) {
        s.selectedEntities.erase(s.selectedEntities.begin() + i);
        break;
      }
    }
  };

  auto clearSelection = [&]() {
    s.selectedEntities.clear();
    s.selectedEntityId = 0;
    s.lastClickedEntity = 0;
    s.editObjPart = false;
    s.editColliderBounds = false;
    s.selectedObjPartName.clear();
    s.renaming = false;
  };

  // --- Gizmo Tools Window ---
  ImGui::Begin("Gizmo");
  if (ImGui::RadioButton("Translate", s.gizmoOp == ImGuizmo::TRANSLATE))
    s.gizmoOp = ImGuizmo::TRANSLATE;
  ImGui::SameLine();
  if (ImGui::RadioButton("Rotate", s.gizmoOp == ImGuizmo::ROTATE))
    s.gizmoOp = ImGuizmo::ROTATE;
  ImGui::SameLine();
  if (ImGui::RadioButton("Scale", s.gizmoOp == ImGuizmo::SCALE))
    s.gizmoOp = ImGuizmo::SCALE;
  ImGui::Separator();

  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload *payload =
            ImGui::AcceptDragDropPayload("ASSET_PATH")) {
      const char *path = static_cast<const char *>(payload->Data);
      if (path && path[0] != '\0')
        events.publish(SpawnEntityRequestedEvent{path});
    }
    ImGui::EndDragDropTarget();
  }
  ImGui::DragFloat("Focus distance", &s.focusDistance, 0.25f, 1.0f, 200.0f);
  ImGui::End();

  // Gizmo Logic Helper
  bool hasPrimaryEntity = (s.selectedEntityId != 0 &&
                           reg.has<TransformComponent>(s.selectedEntityId));

  bool sunSelected = (s.selectedEntityId == 0 && !s.selectedEntities.empty() &&
                      s.selectedEntities[0] == 0);

  const bool viewModelSelected =
      hasPrimaryEntity && reg.has<MeshComponent>(s.selectedEntityId) &&
      reg.get<MeshComponent>(s.selectedEntityId).isViewModel;

  if (viewModelSelected && s.gizmoOp == ImGuizmo::ROTATE) {
    s.gizmoOp = ImGuizmo::TRANSLATE;
  }

  OBJModel *modelPtr = nullptr;
  if (hasPrimaryEntity && reg.has<MeshComponent>(s.selectedEntityId)) {
    modelPtr = reg.get<MeshComponent>(s.selectedEntityId).objModel;
  }

  // Gizmo Logic
  if (s.selectedEntityId == 0) {
    // The Sun is now infinitely far away, and controlled via azimuthal angles
    // in Editor Settings menus.
    return edited;
  }

  if (!hasPrimaryEntity)
    return edited;

  auto &tr = reg.get<TransformComponent>(s.selectedEntityId);
  glm::mat4 M_entity = tr.getMatrix();
  bool canEditPart =
      (s.editObjPart && modelPtr && !s.selectedObjPartName.empty());

  if (!canEditPart) {
    glm::mat4 model = M_entity;
    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection),
                         (ImGuizmo::OPERATION)s.gizmoOp,
                         (ImGuizmo::MODE)s.gizmoMode, glm::value_ptr(model));
    if (ImGuizmo::IsUsing()) {
      glm::vec3 t(0.0f), r(0.0f), sc(1.0f);
      if (!decomposeTRSYXZ(model, t, r, sc)) {
        float tf[3], rf[3], scf[3];
        ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(model), tf, rf,
                                              scf);
        t = {tf[0], tf[1], tf[2]};
        r = {rf[0], rf[1], rf[2]};
        sc = {scf[0], scf[1], scf[2]};
      }

      if (s.editColliderBounds &&
          reg.has<ColliderComponent>(s.selectedEntityId)) {
        auto &col = reg.get<ColliderComponent>(s.selectedEntityId);
        // We only use the scale for colliders
        if (col.shape == ColliderComponent::Shape::Box) {
          col.dimensions = sc;
        } else if (col.shape == ColliderComponent::Shape::Sphere) {
          col.dimensions.x = sc.x;
        } else if (col.shape == ColliderComponent::Shape::Capsule) {
          col.dimensions.x = sc.x; // radius
          col.dimensions.y = sc.y; // height
        }
      } else {
        tr.position = t;
        if (!viewModelSelected) {
          tr.rotation = r;
        }
        tr.scale = sc;
      }
      edited = true;
    }
  } else {
    glm::vec3 lp(0.0f), lr(0.0f), ls(1.0f);
    (void)modelPtr->getObjectLocalTRS(s.selectedObjPartName, lp, lr, ls);
    glm::mat4 M_partLocal = buildTRS(lp, lr, ls);
    glm::mat4 model = M_entity * M_partLocal;
    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection),
                         (ImGuizmo::OPERATION)s.gizmoOp,
                         (ImGuizmo::MODE)s.gizmoMode, glm::value_ptr(model));
    if (ImGuizmo::IsUsing()) {
      glm::mat4 newLocal = glm::inverse(M_entity) * model;
      glm::vec3 t(0.0f), r(0.0f), sc(1.0f);
      if (!decomposeTRSYXZ(newLocal, t, r, sc)) {
        float tf[3], rf[3], scf[3];
        ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(newLocal), tf, rf,
                                              scf);
        t = {tf[0], tf[1], tf[2]};
        r = {rf[0], rf[1], rf[2]};
        sc = {scf[0], scf[1], scf[2]};
      }
      modelPtr->setObjectLocalTRS(s.selectedObjPartName, t, r, sc);
      edited = true;
    }
  }
  return edited;
}

// =============================================================================
// Script Editor
// =============================================================================
void EditorUI::drawScriptEditor(EditorContext &ctx) {
  if (!mShowScriptEditor)
    return;

  std::string windowTitle =
      "Script Editor - " +
      (mScriptEditorPath.empty() ? "Untitled" : mScriptEditorPath) +
      "###ScriptEditorWindow";

  if (ImGui::Begin(windowTitle.c_str(), &mShowScriptEditor)) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.08f, 0.08f, 0.12f, 1.0f));
    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts.Size > 1
                        ? ImGui::GetIO().Fonts->Fonts[1]
                        : nullptr);
    if (ImGui::InputTextMultiline(
            "##ScriptEditorText", mScriptEditorBuf, sizeof(mScriptEditorBuf),
            ImVec2(-1, -ImGui::GetTextLineHeightWithSpacing() * 2),
            ImGuiInputTextFlags_AllowTabInput)) {
      mScriptEditorDirty = true;
    }
    ImGui::PopFont();
    ImGui::PopStyleColor();

    if (mScriptEditorDirty) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.2f, 1.0f));
      if (ImGui::Button("Save & Reload")) {
        std::ofstream out(mScriptEditorPath);
        if (out.good()) {
          out << mScriptEditorBuf;
          out.close();
          mScriptEditorDirty = false;
          // Force reload of script component
          for (auto eid : ctx.scene.registry().view<ScriptComponent>()) {
            auto &sc = ctx.scene.registry().get<ScriptComponent>(eid);
            if (sc.scriptPath == mScriptEditorPath) {
              sc.initialized = false;
            }
          }
        }
      }
      ImGui::PopStyleColor();
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.1f, 1), "Unsaved changes");
    }
  }
  ImGui::End();
}
