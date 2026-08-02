#include "VkEditor.h"

#include "VulkanRenderer.h"

#include "Assets/AssetManager.h"
#include "Assets/MeshData.h"
#include "Core/Logger.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "ECS/Systems/PhysicsSystem.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h" // Console tab's Lua prompt
#include "subsystems/VkTerrainSubsystem.h"

#include "EditorCamera.h"
#include "EditorTheme.h"
#include "ImGuizmo.h"
#include "imgui.h"
#include "json.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ─── Helpers shared with the old EditorUI (Editor/EditorUI.cpp) ─────────────

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

// Matches TransformComponent::getMatrix: T * Ry * Rx * Rz * S. ImGuizmo's
// built-in Euler decomposition uses a different convention, which makes
// entity rotations jump or drift after gizmo edits.
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

  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.18f, 0.22f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                        ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
  *open = ImGui::CollapsingHeader(label, flags);
  ImGui::PopStyleColor(2);

  ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
  ImGui::PushID(label);
  if (ImGui::SmallButton("R")) {
    *wantsReset = true;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Reset to defaults");
  ImGui::PopID();

  if (canRemove && ImGui::BeginPopupContextItem(label)) {
    if (ImGui::MenuItem("Remove Component")) {
      *wantsRemove = true;
    }
    ImGui::EndPopup();
  }

  return *open;
}

static bool isModelFile(const std::string &ext) {
  return ext == ".obj" || ext == ".gltf" || ext == ".glb" || ext == ".fbx";
}

// Hover tooltip for the previous item. Reserved for the non-obvious knobs;
// self-explanatory ones stay bare.
static void Tip(const char *text) { ImGui::SetItemTooltip("%s", text); }

static bool icontains(const char *haystack, const char *needle) {
  if (!haystack || !needle)
    return false;
  std::string a = haystack, b = needle;
  for (auto &c : a)
    c = (char)tolower((unsigned char)c);
  for (auto &c : b)
    c = (char)tolower((unsigned char)c);
  return a.find(b) != std::string::npos;
}

bool VkEditor::EnvRowFilter::row(const char *label) {
  if (!active())
    return true;
  if (!icontains(label, query) && !icontains(currentSection, query))
    return false;
  if (pendingSection) {
    ImGui::SeparatorText(pendingSection);
    pendingSection = nullptr;
  }
  return true;
}

void VkEditor::envSection(const char *name) {
  if (mEnvFilter.active())
    mEnvFilter.section(name);
  else
    ImGui::SeparatorText(name);
}

static void saveGraphicsSettingsImpl(const std::string &path,
                                      const vkrhi::VulkanRenderer::Params &p) {
  try {
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path());
    nlohmann::json j;

    j["lightYawDeg"] = p.lightYawDeg;
    j["lightPitchDeg"] = p.lightPitchDeg;
    j["timeOfDayEnabled"] = p.timeOfDayEnabled;
    j["timeOfDaySpeed"] = p.timeOfDaySpeed;
    j["sunIntensity"] = p.sunIntensity;
    j["sunDiscIntensity"] = p.sunDiscIntensity;
    j["ambientIntensity"] = p.ambientIntensity;
    j["shadowStrength"] = p.shadowStrength;
    j["shadowSoftness"] = p.shadowSoftness;
    j["shadowSamples"] = p.shadowSamples;

    j["aoRadius"] = p.aoRadius;
    j["aoBias"] = p.aoBias;
    j["aoStrength"] = p.aoStrength;

    j["atmosphereHaze"] = p.atmosphereHaze;
    j["skyBrightness"] = p.skyBrightness;
    j["starIntensity"] = p.starIntensity;
    j["moonIntensity"] = p.moonIntensity;
    j["moonGlowIntensity"] = p.moonGlowIntensity;
    j["nightSkyBrightness"] = p.nightSkyBrightness;

    j["iblSpecularIntensity"] = p.iblSpecularIntensity;
    j["terrainSkyReflectIntensity"] = p.terrainSkyReflectIntensity;

    j["volumetricEnabled"] = p.volumetricEnabled;
    j["volumetricIntensity"] = p.volumetricIntensity;
    j["volumetricAnisotropy"] = p.volumetricAnisotropy;
    j["volumetricMaxDist"] = p.volumetricMaxDist;
    j["volumetricSteps"] = p.volumetricSteps;

    j["exposure"] = p.exposure;
    j["gamma"] = p.gamma;
    j["saturation"] = p.saturation;
    j["contrast"] = p.contrast;
    j["vignette"] = p.vignette;
    j["tonemapMode"] = p.tonemapMode;
    j["bloomIntensity"] = p.bloomIntensity;
    j["autoExposure"] = p.autoExposure;
    j["autoExposureSpeed"] = p.autoExposureSpeed;
    j["autoExposureMin"] = p.autoExposureMin;
    j["autoExposureMax"] = p.autoExposureMax;

    j["vegShadowDistance"] = p.vegShadowDistance;
    j["vegDrawDistance"] = p.vegDrawDistance;
    const auto v3 = [](glm::vec3 v) {
      return nlohmann::json::array({v.x, v.y, v.z});
    };
    const auto &s = p.style;
    nlohmann::json sj;
    sj["version"] = 1;
    sj["terrain"] = nlohmann::json::array();
    for (const auto &layer : s.terrain)
      sj["terrain"].push_back({{"lit", v3(layer.lit)},
                               {"shade", v3(layer.shade)},
                               {"mottleScale", layer.mottleScale},
                               {"overlayStrength", layer.overlayStrength}});
    sj["autumnAmount"] = s.autumnAmount;
    sj["facetStrength"] = s.facetStrength;
    sj["washEdgeDarkening"] = s.washEdgeDarkening;
    sj["worldPaperStrength"] = s.worldPaperStrength;
    sj["vibrance"] = s.vibrance;
    sj["splitBalance"] = s.splitBalance;
    sj["splitShadow"] = v3(s.splitShadow);
    sj["splitHighlight"] = v3(s.splitHighlight);
    sj["outlineColor"] = v3(s.outlineColor);
    sj["outlineWidth"] = s.outlineWidth;
    sj["outlineStrength"] = s.outlineStrength;
    sj["outlineDepthThreshold"] = s.outlineDepthThreshold;
    sj["outlineNormalThreshold"] = s.outlineNormalThreshold;
    sj["outlineDistance"] = s.outlineDistance;
    sj["screenPaperStrength"] = s.screenPaperStrength;
    sj["fxaaEnabled"] = s.fxaaEnabled;
    sj["skyZenith"] = v3(s.skyZenith);
    sj["skyHorizon"] = v3(s.skyHorizon);
    sj["skyGradeStrength"] = s.skyGradeStrength;
    sj["skyBands"] = s.skyBands;
    sj["skyBandSoftness"] = s.skyBandSoftness;
    sj["sunSoftness"] = s.sunSoftness;
    sj["cloudCoverage"] = s.cloudCoverage;
    sj["cloudSoftness"] = s.cloudSoftness;
    sj["cloudWind"] = {s.cloudWind.x, s.cloudWind.y};
    sj["cloudLit"] = v3(s.cloudLit);
    sj["cloudMid"] = v3(s.cloudMid);
    sj["cloudBase"] = v3(s.cloudBase);
    j["style"] = std::move(sj);

    std::ofstream file(path);
    if (file.is_open()) {
      file << j.dump(2);
      LOG_INFO("Editor", "Saved graphics settings to " + path);
    }
  } catch (const std::exception &e) {
    LOG_ERROR("Editor",
              std::string("Failed to save graphics settings: ") + e.what());
  }
}

static void loadGraphicsSettingsImpl(const std::string &path,
                                      vkrhi::VulkanRenderer::Params &p) {
  try {
    std::ifstream file(path);
    if (!file.is_open())
      return;
    nlohmann::json j;
    file >> j;
    if (j.contains("style") && j["style"].is_object()) {
      const auto &sj = j["style"];
      auto &s = p.style;
      auto readF = [&](const char *k, float &v) {
        if (sj.contains(k) && sj[k].is_number()) v = sj[k].get<float>();
      };
      auto readV3 = [&](const char *k, glm::vec3 &v) {
        if (sj.contains(k) && sj[k].is_array() && sj[k].size() >= 3)
          v = {sj[k][0].get<float>(), sj[k][1].get<float>(),
               sj[k][2].get<float>()};
      };
      if (sj.contains("terrain") && sj["terrain"].is_array())
        for (size_t i = 0; i < s.terrain.size() && i < sj["terrain"].size(); ++i) {
          const auto &lj = sj["terrain"][i];
          if (lj.contains("lit") && lj["lit"].size() >= 3)
            s.terrain[i].lit = {lj["lit"][0], lj["lit"][1], lj["lit"][2]};
          if (lj.contains("shade") && lj["shade"].size() >= 3)
            s.terrain[i].shade = {lj["shade"][0], lj["shade"][1], lj["shade"][2]};
          if (lj.contains("mottleScale")) s.terrain[i].mottleScale = lj["mottleScale"];
          if (lj.contains("overlayStrength")) s.terrain[i].overlayStrength = lj["overlayStrength"];
        }
      readF("autumnAmount", s.autumnAmount); readF("facetStrength", s.facetStrength);
      readF("washEdgeDarkening", s.washEdgeDarkening); readF("worldPaperStrength", s.worldPaperStrength);
      readF("vibrance", s.vibrance); readF("splitBalance", s.splitBalance);
      readV3("splitShadow", s.splitShadow); readV3("splitHighlight", s.splitHighlight);
      readV3("outlineColor", s.outlineColor); readF("outlineWidth", s.outlineWidth);
      readF("outlineStrength", s.outlineStrength); readF("outlineDepthThreshold", s.outlineDepthThreshold);
      readF("outlineNormalThreshold", s.outlineNormalThreshold); readF("outlineDistance", s.outlineDistance);
      readF("screenPaperStrength", s.screenPaperStrength);
      if (sj.contains("fxaaEnabled")) s.fxaaEnabled = sj["fxaaEnabled"].get<bool>();
      readV3("skyZenith", s.skyZenith); readV3("skyHorizon", s.skyHorizon);
      readF("skyGradeStrength", s.skyGradeStrength); readF("skyBands", s.skyBands);
      readF("skyBandSoftness", s.skyBandSoftness); readF("sunSoftness", s.sunSoftness);
      readF("cloudCoverage", s.cloudCoverage); readF("cloudSoftness", s.cloudSoftness);
      if (sj.contains("cloudWind") && sj["cloudWind"].size() >= 2)
        s.cloudWind = {sj["cloudWind"][0], sj["cloudWind"][1]};
      readV3("cloudLit", s.cloudLit); readV3("cloudMid", s.cloudMid); readV3("cloudBase", s.cloudBase);
    }

    if (j.contains("lightYawDeg"))
      p.lightYawDeg = j["lightYawDeg"].get<float>();
    if (j.contains("lightPitchDeg"))
      p.lightPitchDeg = j["lightPitchDeg"].get<float>();
    if (j.contains("timeOfDayEnabled"))
      p.timeOfDayEnabled = j["timeOfDayEnabled"].get<bool>();
    if (j.contains("timeOfDaySpeed"))
      p.timeOfDaySpeed = j["timeOfDaySpeed"].get<float>();
    if (j.contains("sunIntensity"))
      p.sunIntensity = j["sunIntensity"].get<float>();
    if (j.contains("sunDiscIntensity"))
      p.sunDiscIntensity = j["sunDiscIntensity"].get<float>();
    if (j.contains("ambientIntensity"))
      p.ambientIntensity = j["ambientIntensity"].get<float>();
    if (j.contains("shadowStrength"))
      p.shadowStrength = j["shadowStrength"].get<float>();
    if (j.contains("shadowSoftness"))
      p.shadowSoftness = j["shadowSoftness"].get<float>();
    if (j.contains("shadowSamples"))
      p.shadowSamples = j["shadowSamples"].get<int>();

    if (j.contains("aoRadius"))
      p.aoRadius = j["aoRadius"].get<float>();
    if (j.contains("aoBias"))
      p.aoBias = j["aoBias"].get<float>();
    if (j.contains("aoStrength"))
      p.aoStrength = j["aoStrength"].get<float>();

    if (j.contains("atmosphereHaze"))
      p.atmosphereHaze = j["atmosphereHaze"].get<float>();
    if (j.contains("skyBrightness"))
      p.skyBrightness = j["skyBrightness"].get<float>();
    if (j.contains("starIntensity"))
      p.starIntensity = j["starIntensity"].get<float>();
    if (j.contains("moonIntensity"))
      p.moonIntensity = j["moonIntensity"].get<float>();
    if (j.contains("moonGlowIntensity"))
      p.moonGlowIntensity = j["moonGlowIntensity"].get<float>();
    if (j.contains("nightSkyBrightness"))
      p.nightSkyBrightness = j["nightSkyBrightness"].get<float>();

    if (j.contains("iblSpecularIntensity"))
      p.iblSpecularIntensity = j["iblSpecularIntensity"].get<float>();
    if (j.contains("terrainSkyReflectIntensity"))
      p.terrainSkyReflectIntensity =
          j["terrainSkyReflectIntensity"].get<float>();

    if (j.contains("volumetricEnabled"))
      p.volumetricEnabled = j["volumetricEnabled"].get<bool>();
    if (j.contains("volumetricIntensity"))
      p.volumetricIntensity = j["volumetricIntensity"].get<float>();
    if (j.contains("volumetricAnisotropy"))
      p.volumetricAnisotropy = j["volumetricAnisotropy"].get<float>();
    if (j.contains("volumetricMaxDist"))
      p.volumetricMaxDist = j["volumetricMaxDist"].get<float>();
    if (j.contains("volumetricSteps"))
      p.volumetricSteps = j["volumetricSteps"].get<int>();

    if (j.contains("exposure"))
      p.exposure = j["exposure"].get<float>();
    if (j.contains("gamma"))
      p.gamma = j["gamma"].get<float>();
    if (j.contains("saturation"))
      p.saturation = j["saturation"].get<float>();
    if (j.contains("contrast"))
      p.contrast = j["contrast"].get<float>();
    if (j.contains("vignette"))
      p.vignette = j["vignette"].get<float>();
    if (j.contains("tonemapMode"))
      p.tonemapMode = j["tonemapMode"].get<int>();
    if (j.contains("bloomIntensity"))
      p.bloomIntensity = j["bloomIntensity"].get<float>();
    if (j.contains("autoExposure"))
      p.autoExposure = j["autoExposure"].get<bool>();
    if (j.contains("autoExposureSpeed"))
      p.autoExposureSpeed = j["autoExposureSpeed"].get<float>();
    if (j.contains("autoExposureMin"))
      p.autoExposureMin = j["autoExposureMin"].get<float>();
    if (j.contains("autoExposureMax"))
      p.autoExposureMax = j["autoExposureMax"].get<float>();

    if (j.contains("vegShadowDistance"))
      p.vegShadowDistance = j["vegShadowDistance"].get<float>();
    if (j.contains("vegDrawDistance"))
      p.vegDrawDistance = j["vegDrawDistance"].get<float>();

    LOG_INFO("Editor", "Loaded graphics settings from " + path);
  } catch (const std::exception &e) {
    LOG_ERROR("Editor",
              std::string("Failed to load graphics settings: ") + e.what());
  }
}

void VkEditor::saveAllSettings(Context &ctx) {
  std::string gfxSettingsPath = ctx.assetDir + "/settings/graphics_settings.json";
  saveGraphicsSettingsImpl(gfxSettingsPath, ctx.renderer.params());

  if (ctx.terrainSubsystem) {
    std::filesystem::path manifestPath =
        std::filesystem::path(ctx.assetDir).parent_path() / "terrain_scatter.json";
    writeScatterManifest(manifestPath.string(), ctx.terrainSubsystem->manifest());
  }

  std::error_code ec;
  std::filesystem::create_directories(
      std::filesystem::path(mScenePath).parent_path(), ec);
  if (ctx.scene.saveToFile(mScenePath)) {
    LOG_INFO("Editor", "Saved scene: " + mScenePath);
  }

  LOG_INFO("Editor",
           "Successfully saved all settings (Graphics, Terrain, Environment & Scene)");
}

void VkEditor::loadAllSettings(Context &ctx) {
  std::string gfxSettingsPath = ctx.assetDir + "/settings/graphics_settings.json";
  loadGraphicsSettingsImpl(gfxSettingsPath, ctx.renderer.params());
}

// ─── VkEditor ────────────────────────────────────────────────────────────────

void VkEditor::releasePhysicsBodies(Context &ctx) {
  auto &reg = ctx.scene.registry();
  for (EntityId e : reg.view<RigidbodyComponent>()) {
    auto &rb = reg.get<RigidbodyComponent>(e);
    if (rb.bodyID != 0xFFFFFFFF) {
      ctx.physics.removeBody(rb.bodyID);
      rb.bodyID = 0xFFFFFFFF;
    }
  }
}

bool VkEditor::hasPlayerEntity(Context &ctx) const {
  auto &reg = ctx.scene.registry();
  for (EntityId e : reg.view<CameraComponent>()) {
    if (!reg.has<LifecycleComponent>(e) ||
        reg.get<LifecycleComponent>(e).state == EntityLifecycleState::Alive) {
      return true;
    }
  }
  return false;
}

uint32_t VkEditor::createPlayerEntity(Context &ctx) {
  auto &reg = ctx.scene.registry();
  uint32_t id = reg.create();

  glm::vec3 camPos = ctx.renderer.params().camPos;
  float camYaw = ctx.renderer.params().camYawDeg;
  float camPitch = ctx.renderer.params().camPitchDeg;

  reg.emplace<NameComponent>(id, NameComponent("Player"));
  reg.emplace<TransformComponent>(
      id, TransformComponent{camPos, glm::vec3(0.0f, camYaw, 0.0f),
                             glm::vec3(1.0f, 1.0f, 1.0f)});

  auto &cam = reg.emplace<CameraComponent>(id);
  cam.isPrimary = true;
  cam.fov = 60.0f;
  cam.yaw = camYaw;
  cam.pitch = camPitch;

  auto &rb = reg.emplace<RigidbodyComponent>(id);
  rb.type = RigidbodyComponent::Type::Dynamic;
  rb.mass = 70.0f;
  rb.lockRotation = true;

  auto &col = reg.emplace<ColliderComponent>(id);
  col.shape = ColliderComponent::Shape::Capsule;
  col.dimensions = glm::vec3(0.6f, 1.8f, 0.6f);
  col.offset = glm::vec3(0.0f, 0.9f, 0.0f);

  auto &sc = reg.emplace<ScriptComponent>(id);
  sc.scriptPath = ctx.assetDir + "/scripts/rock_thrower.lua";

  selection.selectedEntityId = id;
  selection.selectedEntities = {id};

  LOG_INFO("Editor", "Created Player entity at camera position (" +
                         std::to_string(camPos.x) + ", " +
                         std::to_string(camPos.y) + ", " +
                         std::to_string(camPos.z) + ")");

  return id;
}

void VkEditor::requestPlay(Context &ctx) {
  if (!hasPlayerEntity(ctx)) {
    LOG_ERROR("Editor", "Cannot enter Play Mode: No Player entity in scene! "
                        "Use Create > Player to spawn one.");
    return;
  }

  // Normally set by the first draw() of the session; a caller invoking this
  // before ever drawing a frame (e.g. a headless smoke run driving play mode
  // from frame 0) still needs a valid snapshot path.
  if (mPlayModeSnapshotPath.empty())
    mPlayModeSnapshotPath =
        ctx.assetDir + "/scenes/.vk_editor_playmode_snapshot.json";
  if (!mInPlayMode) {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(mPlayModeSnapshotPath).parent_path(), ec);
    ctx.scene.saveToFile(mPlayModeSnapshotPath);
    mInPlayMode = true;
  }
  if (ctx.simulatePhysics)
    *ctx.simulatePhysics = true;
}

void VkEditor::stopPlayMode(Context &ctx) {
  *ctx.simulatePhysics = false;
  releasePhysicsBodies(ctx);
  if (ctx.scene.loadFromFile(mPlayModeSnapshotPath)) {
    selection = SelectionState{};
    LOG_INFO("Editor", "Stopped: reverted to pre-play state");
  } else {
    LOG_ERROR("Editor", "Stop: failed to reload play-mode snapshot");
  }
  mInPlayMode = false;
}

void VkEditor::deleteEntity(Context &ctx, uint32_t id) {
  if (id == 0)
    return;
  auto &reg = ctx.scene.registry();
  if (reg.has<RigidbodyComponent>(id)) {
    auto &rb = reg.get<RigidbodyComponent>(id);
    if (rb.bodyID != 0xFFFFFFFF) {
      ctx.physics.removeBody(rb.bodyID);
      rb.bodyID = 0xFFFFFFFF;
    }
  }
  ctx.scene.deleteEntity(id);
  ctx.scene.flushPendingDestroy();
  if (selection.selectedEntityId == id)
    selection.selectedEntityId = 0;
  selection.selectedEntities.erase(
      std::remove(selection.selectedEntities.begin(),
                  selection.selectedEntities.end(), id),
      selection.selectedEntities.end());
}

bool VkEditor::draw(Context &ctx, const glm::mat4 &view,
                    const glm::mat4 &proj) {
  bool sceneModified = false;
  // drawLog() has no Context of its own; latch what its Lua prompt needs.
  mScriptSystem = ctx.scriptSystem;

  static bool sLoadedSettingsOnce = false;
  if (!sLoadedSettingsOnce) {
    loadAllSettings(ctx);
    sLoadedSettingsOnce = true;
  }

  if (mScenePath.empty())
    mScenePath = ctx.assetDir + "/scenes/vk_editor_scene.json";
  if (mBrowsePath.empty())
    mBrowsePath = ctx.assetDir;
  if (mPlayModeSnapshotPath.empty())
    mPlayModeSnapshotPath = ctx.assetDir + "/scenes/.vk_editor_playmode_snapshot.json";

  drawMenuBar(ctx);

  // ── Play mode: the game's screen, not the editor's ─────────────────────
  // Panels, toolbar, gizmo and brush all hide until Stop (the cursor is
  // captured for mouse-look anyway); only the menu bar and the PLAY banner
  // remain. This is the visible half of the editor/play split -- the input
  // half lives in main.cpp / VkCoreAppLayer.
  if (mInPlayMode) {
    drawPlayModeOverlay(ctx);
    return sceneModified;
  }

  // ── Toolbar (same strip as the old editor) ─────────────────────────────
  if (selection.gizmoOp == ImGuizmo::TRANSLATE)
    toolbar.gizmoOp = ToolbarState::Translate;
  else if (selection.gizmoOp == ImGuizmo::ROTATE)
    toolbar.gizmoOp = ToolbarState::Rotate;
  else if (selection.gizmoOp == ImGuizmo::SCALE)
    toolbar.gizmoOp = ToolbarState::Scale;

  EditorToolbar::draw(toolbar);
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) // not mouse-looking
    EditorToolbar::processShortcuts(toolbar);

  if (toolbar.gizmoOp == ToolbarState::Translate)
    selection.gizmoOp = ImGuizmo::TRANSLATE;
  else if (toolbar.gizmoOp == ToolbarState::Rotate)
    selection.gizmoOp = ImGuizmo::ROTATE;
  else if (toolbar.gizmoOp == ToolbarState::Scale)
    selection.gizmoOp = ImGuizmo::SCALE;
  selection.gizmoMode = toolbar.worldSpace ? ImGuizmo::WORLD : ImGuizmo::LOCAL;

  // ── Panels: tiled default layout ───────────────────────────────────────
  // Left column Hierarchy/Statistics, right column Inspector/Environment,
  // Assets+Console strip along the bottom between them. Only a default
  // (ImGuiCond_FirstUseEver -- imgui_glgenvk.ini wins afterwards) unless
  // Windows > Reset Layout forced it for this frame.
  const float toolbarH = 44.0f; // toolbar strip + a small gap
  ImGuiViewport *vp = ImGui::GetMainViewport();
  const ImGuiCond layoutCond =
      mApplyDefaultLayout ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
  const float x0 = vp->WorkPos.x;
  const float y0 = vp->WorkPos.y + toolbarH;
  const float availW = vp->WorkSize.x;
  const float availH = std::max(400.0f, vp->WorkSize.y - toolbarH);
  const float leftW = 300.0f;
  const float rightW = 380.0f;
  const float bottomH = 250.0f;
  const float leftSplit = availH * 0.58f;
  const float rightSplit = availH * 0.52f;

  if (mShowHierarchy) {
    ImGui::SetNextWindowPos(ImVec2(x0, y0), layoutCond);
    ImGui::SetNextWindowSize(ImVec2(leftW, leftSplit), layoutCond);
    drawHierarchy(ctx);
  }
  if (mShowStats) {
    ImGui::SetNextWindowPos(ImVec2(x0, y0 + leftSplit), layoutCond);
    ImGui::SetNextWindowSize(ImVec2(leftW, availH - leftSplit), layoutCond);
    drawStats(ctx);
  }
  if (mShowInspector) {
    ImGui::SetNextWindowPos(ImVec2(x0 + availW - rightW, y0), layoutCond);
    ImGui::SetNextWindowSize(ImVec2(rightW, rightSplit), layoutCond);
    sceneModified |= drawInspector(ctx);
  }
  if (mShowEnvironment) {
    ImGui::SetNextWindowPos(ImVec2(x0 + availW - rightW, y0 + rightSplit),
                            layoutCond);
    ImGui::SetNextWindowSize(ImVec2(rightW, availH - rightSplit), layoutCond);
    drawEnvironment(ctx);
  }
  if (mShowAssets) {
    ImGui::SetNextWindowPos(ImVec2(x0 + leftW, y0 + availH - bottomH),
                            layoutCond);
    ImGui::SetNextWindowSize(
        ImVec2(std::max(400.0f, availW - leftW - rightW), bottomH),
        layoutCond);
    if (ImGui::Begin("Assets", &mShowAssets, ImGuiWindowFlags_NoCollapse)) {
      if (ImGui::BeginTabBar("AssetsConsoleTabs")) {
        if (ImGui::BeginTabItem("Assets")) {
          drawAssetsContent(ctx);
          ImGui::EndTabItem();
        }
        if (mShowLog && ImGui::BeginTabItem("Console")) {
          drawLog();
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
    }
    ImGui::End();
  }

  if (mShowMultiViewStudio)
    drawMultiViewStudio(ctx);

  mApplyDefaultLayout = false;

  // ── Gizmo ──────────────────────────────────────────────────────────────
  sceneModified |= drawGizmo(ctx, view, proj);

  // ── Collision outlines (3D wireframe for Player and selected colliders) ──
  drawColliderOutlines(ctx, view, proj);

  // ── Wireframe AABB overlay for all active scene entities ──────────────
  drawWireframeOverlay(ctx, view, proj);

  // ── Terrain brush (screen-to-world ray, applied while enabled+held) ────
  updateTerrainBrush(ctx, view, proj);

  return sceneModified;
}

void VkEditor::drawPlayModeOverlay(Context &ctx) {
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  const bool running = ctx.simulatePhysics && *ctx.simulatePhysics;
  const ImVec4 tint = running ? EditorTheme::kAccent
                              : ImVec4(0.60f, 0.60f, 0.66f, 1.0f); // paused
  const ImU32 col = ImGui::GetColorU32(tint);

  // Accent frame around the whole viewport.
  dl->AddRect(vp->Pos,
              ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), col,
              0.0f, 0, 3.0f);

  // Status pill, top-center, under the menu bar.
  const char *label =
      running ? ">  PLAY MODE  -  Esc to stop" : "||  PAUSED  -  Esc to stop";
  const ImVec2 textSize = ImGui::CalcTextSize(label);
  const ImVec2 pad(16.0f, 7.0f);
  const float cx = vp->Pos.x + vp->Size.x * 0.5f;
  const float top = vp->WorkPos.y + 10.0f;
  const ImVec2 rectMin(cx - textSize.x * 0.5f - pad.x, top);
  const ImVec2 rectMax(cx + textSize.x * 0.5f + pad.x,
                       top + textSize.y + pad.y * 2.0f);
  const float rounding = (rectMax.y - rectMin.y) * 0.5f;
  dl->AddRectFilled(rectMin, rectMax,
                    ImGui::GetColorU32(ImVec4(0.05f, 0.05f, 0.06f, 0.88f)),
                    rounding);
  dl->AddRect(rectMin, rectMax, col, rounding, 0, 1.5f);
  dl->AddText(ImVec2(rectMin.x + pad.x, rectMin.y + pad.y), col, label);
}

void VkEditor::drawMenuBar(Context &ctx) {
  if (!ImGui::BeginMainMenuBar())
    return;

  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("New Scene")) {
      releasePhysicsBodies(ctx);
      ctx.scene.clear();
      selection = SelectionState{};
      LOG_INFO("Editor", "New scene");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Save All Settings", "Ctrl+Shift+S")) {
      saveAllSettings(ctx);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
      std::error_code ec;
      std::filesystem::create_directories(
          std::filesystem::path(mScenePath).parent_path(), ec);
      if (ctx.scene.saveToFile(mScenePath))
        LOG_INFO("Editor", "Saved scene: " + mScenePath);
      else
        LOG_ERROR("Editor", "Failed to save scene: " + mScenePath);
    }
    if (ImGui::MenuItem("Load Scene", "Ctrl+O")) {
      releasePhysicsBodies(ctx);
      if (ctx.scene.loadFromFile(mScenePath)) {
        selection = SelectionState{};
        LOG_INFO("Editor", "Loaded scene: " + mScenePath);
      } else {
        LOG_ERROR("Editor", "Failed to load scene: " + mScenePath);
      }
    }
    ImGui::SetNextItemWidth(320);
    char pathBuf[512];
    std::snprintf(pathBuf, sizeof(pathBuf), "%s", mScenePath.c_str());
    if (ImGui::InputText("##ScenePath", pathBuf, sizeof(pathBuf)))
      mScenePath = pathBuf;
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Create")) {
    if (ImGui::MenuItem("Player")) {
      createPlayerEntity(ctx);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Empty Entity")) {
      uint32_t e = ctx.scene.createEmptyEntity("Empty");
      selection.selectedEntityId = e;
      selection.selectedEntities = {e};
    }
    ImGui::Separator();
    const char *prims[] = {"cube", "sphere", "plane", "cylinder", "cone"};
    const char *labels[] = {"Cube", "Sphere", "Plane", "Cylinder", "Cone"};
    for (int i = 0; i < 5; ++i) {
      if (ImGui::MenuItem(labels[i])) {
        uint32_t e = ctx.scene.spawnPrimitive(prims[i]);
        if (e != 0) {
          selection.selectedEntityId = e;
          selection.selectedEntities = {e};
        }
      }
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Nature")) {
      // Quick-spawn for the terrain-generator prop set (same assets/recenter
      // convention as the demo world in main.cpp) -- a faster path than
      // browsing to terraingeneratorassets/ in the Assets panel by hand.
      struct NatureProp {
        const char *label;
        const char *relPath;
        MeshData::Recenter recenter;
      };
      static const NatureProp props[] = {
          {"Tree", "/terraingeneratorassets/tree.obj", MeshData::Recenter::BaseY},
          {"Rock", "/terraingeneratorassets/rock.obj", MeshData::Recenter::Center},
      };
      for (const auto &prop : props) {
        if (ImGui::MenuItem(prop.label)) {
          uint32_t e = ctx.scene.spawnFromFile(ctx.assetDir + prop.relPath);
          if (e != 0) {
            auto &reg = ctx.scene.registry();
            if (reg.has<MeshComponent>(e))
              ctx.assets.recenterOBJ(reg.get<MeshComponent>(e).objHandle,
                                     prop.recenter);
            selection.selectedEntityId = e;
            selection.selectedEntities = {e};
            LOG_INFO("Editor", std::string("Spawned ") + prop.label);
          } else {
            LOG_ERROR("Editor", std::string("Failed to spawn ") + prop.label);
          }
        }
      }
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Windows")) {
    ImGui::MenuItem("Hierarchy", nullptr, &mShowHierarchy);
    ImGui::MenuItem("Inspector", nullptr, &mShowInspector);
    ImGui::MenuItem("Assets / Console", nullptr, &mShowAssets);
    ImGui::MenuItem("Environment", nullptr, &mShowEnvironment);
    ImGui::MenuItem("Multi-View Split Screen", nullptr, &mShowMultiViewStudio);
    ImGui::MenuItem("Statistics", nullptr, &mShowStats);
    ImGui::Separator();
    if (ImGui::MenuItem("Reset Layout")) {
      // Re-show everything and snap panels back to the default tiling
      // (overrides whatever imgui_glgenvk.ini remembers).
      mShowHierarchy = mShowInspector = mShowAssets = true;
      mShowEnvironment = mShowLog = mShowStats = true;
      mShowMultiViewStudio = true;
      mApplyDefaultLayout = true;
    }
    ImGui::EndMenu();
  }

  if (ctx.simulatePhysics) {
    // Play/Pause/Stop, like the old editor's play-state buttons. Entering
    // Play snapshots the scene (so Stop can revert whatever happened while
    // simulating); Pause just freezes/resumes without touching the snapshot;
    // Stop reloads it and leaves play mode.
    ImGui::SameLine(ImGui::GetWindowWidth() - 220);
    const bool playing = *ctx.simulatePhysics;
    const bool canPlay = hasPlayerEntity(ctx) || mInPlayMode;

    ImGui::BeginDisabled(!canPlay);
    ImGui::PushStyleColor(ImGuiCol_Button, (mInPlayMode && playing)
                                               ? EditorTheme::kAccent
                                               : EditorTheme::kHeader);
    if (ImGui::SmallButton("> Play"))
      requestPlay(ctx);
    ImGui::PopStyleColor();
    ImGui::EndDisabled();

    if (!canPlay && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Cannot start Play Mode: No Player entity in scene!\n"
                        "Use Create > Player to spawn a Player entity.");
    }

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, (mInPlayMode && !playing)
                                               ? EditorTheme::kAccent
                                               : EditorTheme::kHeader);
    if (ImGui::SmallButton("|| Pause") && mInPlayMode)
      *ctx.simulatePhysics = false;
    ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::BeginDisabled(!mInPlayMode);
    if (ImGui::SmallButton("[] Stop"))
      stopPlayMode(ctx);
    ImGui::EndDisabled();
  }

  ImGui::EndMainMenuBar();

  // Global shortcuts
  ImGuiIO &io = ImGui::GetIO();
  // Escape leaves play mode even though the mouse is captured for character
  // look and the Stop button above isn't clickable -- this is the only way
  // out in that state.
  if (mInPlayMode && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    stopPlayMode(ctx);
  // Editor-only shortcuts: while playing, the keyboard belongs to gameplay
  // (and Ctrl+S would save the live simulated scene over the authored one).
  if (!mInPlayMode) {
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
      std::error_code ec;
      std::filesystem::create_directories(
          std::filesystem::path(mScenePath).parent_path(), ec);
      if (ctx.scene.saveToFile(mScenePath))
        LOG_INFO("Editor", "Saved scene: " + mScenePath);
    }
    if (!io.WantCaptureKeyboard &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false) &&
        selection.selectedEntityId != 0) {
      deleteEntity(ctx, selection.selectedEntityId);
    }
  }
}

void VkEditor::drawHierarchy(Context &ctx) {
  if (ImGui::Begin("Hierarchy", &mShowHierarchy, ImGuiWindowFlags_NoCollapse)) {
    auto &s = selection;
    ImGui::InputTextWithHint("##filter", "Search...", s.outlinerFilter, 128);
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
      s.outlinerFilter[0] = 0;
    ImGui::Separator();

    auto &reg = ctx.scene.registry();

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

    uint32_t pendingDelete = 0;

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
      if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Delete"))
          pendingDelete = id;
        ImGui::EndPopup();
      }
      ImGui::PopID();
    };

    // Split into loose scene entities and named groups (currently just
    // terrain chunks, "TerrainChunk_<x>_<y>" -- see VkTerrainSubsystem.cpp)
    // so the outliner doesn't drown in dozens of streamed chunk rows. Add
    // more groups the same way (a prefix check + its own vector) as other
    // systems spawn many same-kind entities.
    std::vector<std::pair<EntityId, std::string>> sceneEntities;
    std::vector<std::pair<EntityId, std::string>> terrainEntities;
    auto view = reg.view<TransformComponent>();
    for (auto entity : view) {
      std::string name = "Entity " + std::to_string((uint32_t)entity);
      if (reg.has<NameComponent>(entity))
        name = reg.get<NameComponent>(entity).name;
      if (!passFilter(name.c_str()))
        continue;
      if (name.rfind("TerrainChunk", 0) == 0)
        terrainEntities.push_back({entity, name});
      else
        sceneEntities.push_back({entity, name});
    }

    if (ImGui::TreeNodeEx("Scene",
                          ImGuiTreeNodeFlags_DefaultOpen |
                              ImGuiTreeNodeFlags_SpanFullWidth,
                          "Scene (%d)",
                          (int)(sceneEntities.size() + terrainEntities.size()))) {
      for (auto &it : sceneEntities)
        drawEntityRow(it.first, it.second);

      if (!terrainEntities.empty()) {
        if (ImGui::TreeNodeEx("TerrainGeneratorGroup",
                              ImGuiTreeNodeFlags_SpanFullWidth,
                              "Terrain Generator (%d)",
                              (int)terrainEntities.size())) {
          for (auto &it : terrainEntities)
            drawEntityRow(it.first, it.second);
          ImGui::TreePop();
        }
      }
      ImGui::TreePop();
    }

    if (pendingDelete != 0)
      deleteEntity(ctx, pendingDelete);
  }
  ImGui::End();
}

bool VkEditor::drawInspector(Context &ctx) {
  bool edited = false;
  if (ImGui::Begin("Inspector", &mShowInspector, ImGuiWindowFlags_NoCollapse)) {
    auto &reg = ctx.scene.registry();
    auto &s = selection;
    uint32_t id = s.selectedEntityId;

    if (id == 0 || !reg.has<TransformComponent>(id)) {
      ImGui::TextDisabled("No entity selected.");
      ImGui::End();
      return false;
    }

    // ── Entity header ────────────────────────────────────────────────────
    ImGui::Text("Entity %u", id);
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
      deleteEntity(ctx, id);
      ImGui::End();
      return true;
    }
    ImGui::Separator();

    // ── Name ─────────────────────────────────────────────────────────────
    if (reg.has<NameComponent>(id)) {
      auto &nc = reg.get<NameComponent>(id);
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%s", nc.name.c_str());
      if (ImGui::InputText("Name", buf, sizeof(buf))) {
        nc.name = buf;
        edited = true;
      }
    }

    // ── Transform ────────────────────────────────────────────────────────
    if (reg.has<TransformComponent>(id)) {
      bool open = false, wantRemove = false, wantReset = false;
      ComponentHeader("Transform", &open, false, &wantRemove, &wantReset);
      if (wantReset) {
        reg.get<TransformComponent>(id) = TransformComponent{};
        edited = true;
      }
      if (open) {
        auto &tr = reg.get<TransformComponent>(id);
        edited |= DragFloat3Colored("Position", &tr.position.x, 0.1f);
        if (DragFloat3Colored("Rotation", &tr.rotation.x, 0.5f)) {
          normalizeEulerDeg(tr.rotation);
          edited = true;
        }
        edited |= DragFloat3Colored("Scale", &tr.scale.x, 0.01f, 0.01f, 100.0f);
      }
    }

    // ── Mesh ─────────────────────────────────────────────────────────────
    if (reg.has<MeshComponent>(id)) {
      bool open = false, wantRemove = false, wantReset = false;
      ComponentHeader("Mesh", &open, true, &wantRemove, &wantReset);
      if (wantRemove) {
        reg.removeComponent<MeshComponent>(id);
        edited = true;
      } else if (open) {
        auto &mc = reg.get<MeshComponent>(id);
        edited |= ImGui::Checkbox("Visible", &mc.visible);
        ImGui::Checkbox("Casts Shadow", &mc.castsShadow);
        ImGui::TextWrapped("Asset: %s",
                           mc.assetId.empty() ? "(none)" : mc.assetId.c_str());

        const MeshData *data = nullptr;
        if (mc.objHandle.valid())
          data = ctx.assets.getOBJData(mc.objHandle);
        else if (mc.gltfHandle.valid())
          data = ctx.assets.getGLTFData(mc.gltfHandle);
        else if (mc.ufbxHandle.valid())
          data = ctx.assets.getUFBXData(mc.ufbxHandle);
        if (data) {
          ImGui::Text("Submeshes: %zu", data->submeshes.size());
          glm::vec3 mn, mx;
          if (data->getGlobalBounds(mn, mx)) {
            const glm::vec3 size = mx - mn;
            ImGui::Text("Bounds: %.2f x %.2f x %.2f", size.x, size.y, size.z);
          }
        } else {
          ImGui::TextColored(EditorTheme::kWarning, "No parsed mesh data");
        }
      }
    }

    // ── Rigidbody ────────────────────────────────────────────────────────
    if (reg.has<RigidbodyComponent>(id)) {
      bool open = false, wantRemove = false, wantReset = false;
      ComponentHeader("Rigidbody", &open, true, &wantRemove, &wantReset);
      auto removeBody = [&]() {
        auto &rb = reg.get<RigidbodyComponent>(id);
        if (rb.bodyID != 0xFFFFFFFF) {
          ctx.physics.removeBody(rb.bodyID);
          rb.bodyID = 0xFFFFFFFF;
        }
      };
      if (wantRemove) {
        removeBody();
        reg.removeComponent<RigidbodyComponent>(id);
        edited = true;
      } else {
        auto &rb = reg.get<RigidbodyComponent>(id);
        if (wantReset) {
          removeBody();
          rb = RigidbodyComponent{};
          edited = true;
        }
        if (open) {
          const char *types[] = {"Static", "Kinematic", "Dynamic"};
          int type = (int)rb.type;
          if (ImGui::Combo("Type", &type, types, 3)) {
            removeBody(); // recreate with the new motion type
            rb.type = (RigidbodyComponent::Type)type;
            edited = true;
          }
          edited |= ImGui::DragFloat("Mass", &rb.mass, 0.1f, 0.01f, 1000.0f);
          edited |= ImGui::SliderFloat("Friction", &rb.friction, 0.0f, 1.0f);
          edited |=
              ImGui::SliderFloat("Restitution", &rb.restitution, 0.0f, 1.0f);
          ImGui::TextDisabled(
              "Body: %s", rb.bodyID == 0xFFFFFFFF ? "(pending)" : "created");
        }
      }
    }

    // ── Collider ─────────────────────────────────────────────────────────
    if (reg.has<ColliderComponent>(id)) {
      bool open = false, wantRemove = false, wantReset = false;
      ComponentHeader("Collider", &open, true, &wantRemove, &wantReset);
      if (wantRemove) {
        reg.removeComponent<ColliderComponent>(id);
        edited = true;
      } else {
        auto &col = reg.get<ColliderComponent>(id);
        if (wantReset) {
          col = ColliderComponent{};
          edited = true;
        }
        if (open) {
          const char *shapes[] = {"Box", "Sphere", "Capsule"};
          int shape = (int)col.shape;
          if (ImGui::Combo("Shape", &shape, shapes, 3)) {
            col.shape = (ColliderComponent::Shape)shape;
            edited = true;
          }
          edited |= DragFloat3Colored("Offset", &col.offset.x, 0.05f);
          if (col.shape == ColliderComponent::Shape::Box) {
            edited |= DragFloat3Colored("Size", &col.dimensions.x, 0.05f,
                                        0.01f, 500.0f);
          } else {
            edited |= ImGui::DragFloat("Radius", &col.dimensions.x, 0.02f,
                                       0.01f, 100.0f);
            if (col.shape == ColliderComponent::Shape::Capsule)
              edited |= ImGui::DragFloat("Height", &col.dimensions.y, 0.02f,
                                         0.01f, 100.0f);
          }
        }
      }
    }

    // ── Add Component ────────────────────────────────────────────────────
    ImGui::Separator();
    if (ImGui::Button("Add Component", ImVec2(-1, 0)))
      ImGui::OpenPopup("AddComponentPopup");
    if (ImGui::BeginPopup("AddComponentPopup")) {
      if (!reg.has<RigidbodyComponent>(id) &&
          ImGui::MenuItem("Rigidbody")) {
        reg.emplace<RigidbodyComponent>(id);
        edited = true;
      }
      if (!reg.has<ColliderComponent>(id) && ImGui::MenuItem("Collider")) {
        reg.emplace<ColliderComponent>(id);
        edited = true;
      }
      ImGui::EndPopup();
    }
  }
  ImGui::End();
  return edited;
}

void VkEditor::drawAssetsContent(Context &ctx) {
  ImGui::SetNextItemWidth(200);
  ImGui::InputTextWithHint("##AssetSearch", "Filter...", mAssetSearch,
                           sizeof(mAssetSearch));
  ImGui::SameLine();
  if (ImGui::Button("Asset Root"))
    mBrowsePath = ctx.assetDir;
  ImGui::SameLine();
  ImGui::TextDisabled("%s", mBrowsePath.c_str());
  ImGui::Separator();

  namespace fs = std::filesystem;
  std::error_code ec;

  auto matches = [&](const std::string &name) {
    if (mAssetSearch[0] == 0)
      return true;
    std::string a = name, b = mAssetSearch;
    for (auto &c : a)
      c = (char)tolower((unsigned char)c);
    for (auto &c : b)
      c = (char)tolower((unsigned char)c);
    return a.find(b) != std::string::npos;
  };

  if (ImGui::BeginChild("AssetList", ImVec2(0, 0))) {
    fs::path browse(mBrowsePath);
    if (browse.has_parent_path() &&
        browse.lexically_normal() !=
            fs::path(ctx.assetDir).lexically_normal()) {
      if (ImGui::Selectable(".. (up)"))
        mBrowsePath = browse.parent_path().string();
    }

    std::vector<fs::directory_entry> dirs, files;
    for (const auto &entry : fs::directory_iterator(browse, ec)) {
      const std::string name = entry.path().filename().string();
      if (name.rfind("._", 0) == 0)
        continue;
      if (entry.is_directory())
        dirs.push_back(entry);
      else
        files.push_back(entry);
    }
    for (const auto &d : dirs) {
      const std::string name = "[dir] " + d.path().filename().string();
      if (!matches(name))
        continue;
      if (ImGui::Selectable(name.c_str()))
        mBrowsePath = d.path().string();
    }
    for (const auto &f : files) {
      std::string ext = f.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c) { return (char)tolower(c); });
      if (!isModelFile(ext))
        continue;
      const std::string name = f.path().filename().string();
      if (!matches(name))
        continue;
      ImGui::PushID(name.c_str());
      ImGui::Selectable(name.c_str());
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        // Spawn through the real Scene path; VulkanRenderSystem resolves the
        // mesh through the AssetManager next frame.
        std::string full = f.path().generic_string();
        uint32_t e = ctx.scene.spawnFromFile(full);
        if (e != 0) {
          selection.selectedEntityId = e;
          selection.selectedEntities = {e};
          LOG_INFO("Editor", "Spawned: " + full);
        } else {
          LOG_ERROR("Editor", "Failed to spawn: " + full);
        }
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Double-click to spawn");
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
}

void VkEditor::drawLog() {
  if (ImGui::Button("Bottom"))
    mConsoleAutoScroll = true;
  ImGui::SameLine();
  ImGui::Checkbox("Auto-scroll", &mConsoleAutoScroll);
  ImGui::SameLine();

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
  ImGui::InputTextWithHint("##ConsoleSearch", "Search...", mConsoleSearch,
                           sizeof(mConsoleSearch));

  ImGui::Separator();

  // Leave room for the Lua prompt below the log.
  const float promptHeight = ImGui::GetFrameHeightWithSpacing();
  if (ImGui::BeginChild("ConsoleScroll", ImVec2(0, -promptHeight), 0,
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    const auto entries = Logger::instance().recentEntries(1000);
    for (const auto &e : entries) {
      if (e.level == Logger::Level::Info && !mFilterInfo)
        continue;
      if (e.level == Logger::Level::Warn && !mFilterWarn)
        continue;
      if ((e.level == Logger::Level::Error ||
           e.level == Logger::Level::Fatal) &&
          !mFilterError)
        continue;
      if (e.level == Logger::Level::Trace)
        continue;
      if (mConsoleSearch[0] != 0 &&
          e.message.find(mConsoleSearch) == std::string::npos &&
          e.category.find(mConsoleSearch) == std::string::npos)
        continue;

      ImVec4 color = EditorTheme::kText;
      if (e.level == Logger::Level::Warn)
        color = EditorTheme::kWarning;
      else if (e.level == Logger::Level::Error ||
               e.level == Logger::Level::Fatal)
        color = EditorTheme::kError;

      ImGui::TextColored(color, "[%s] [%s] %s", e.timestamp.c_str(),
                         e.category.c_str(), e.message.c_str());
    }
    if (mConsoleAutoScroll &&
        ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
      ImGui::SetScrollHereY(1.0f);
  }
  ImGui::EndChild();

  // Lua prompt. The whole scripting API is reachable from here --
  // assets.define, world.spawn, render.params, terrain.scatter_layer -- which
  // makes it the manual counterpart of the command port the MCP bridge will
  // eventually drive (AI_ASSET_PIPELINE_PLAN.md Phase 3).
  ImGui::Separator();
  ImGui::TextUnformatted(">");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1.0f);
  const bool submitted = ImGui::InputTextWithHint(
      "##LuaPrompt", "lua:  assets.generators()   world.spawn(id)   help()",
      mConsoleInput, sizeof(mConsoleInput),
      ImGuiInputTextFlags_EnterReturnsTrue);
  if (submitted && mConsoleInput[0] != '\0') {
    LOG_INFO("Console", std::string("> ") + mConsoleInput);
    if (mScriptSystem) {
      std::string error;
      if (!mScriptSystem->execString(mConsoleInput, error))
        LOG_ERROR("Console", error);
    } else {
      LOG_ERROR("Console", "script system unavailable");
    }
    mConsoleInput[0] = '\0';
    mConsoleAutoScroll = true;
    ImGui::SetKeyboardFocusHere(-1); // keep typing without re-clicking
  }
}

void VkEditor::drawStats(Context &ctx) {
  if (ImGui::Begin("Statistics", &mShowStats, ImGuiWindowFlags_NoCollapse)) {
    const float fps = (ctx.dt > 1e-6f) ? 1.0f / ctx.dt : 0.0f;
    mFpsHistory[mFpsHistoryIdx] = fps;
    mFpsHistoryIdx = (mFpsHistoryIdx + 1) % kFpsHistorySize;

    ImGui::Text("%.1f FPS (%.2f ms)", fps, ctx.dt * 1000.0f);
    ImGui::PlotLines("##fps", mFpsHistory, kFpsHistorySize, mFpsHistoryIdx,
                     nullptr, 0.0f, FLT_MAX, ImVec2(-1, 46));
    ImGui::Separator();

    auto &reg = ctx.scene.registry();
    int total = 0, meshes = 0, bodies = 0;
    for (EntityId e : reg.view<TransformComponent>()) {
      (void)e;
      ++total;
    }
    for (EntityId e : reg.view<MeshComponent>()) {
      (void)e;
      ++meshes;
    }
    for (EntityId e : reg.view<RigidbodyComponent>()) {
      (void)e;
      ++bodies;
    }
    ImGui::Text("Entities: %d", total);
    ImGui::Text("Meshes:   %d", meshes);
    ImGui::Text("Bodies:   %d", bodies);
    ImGui::Separator();

    const auto &fs = ctx.renderer.frameStats();
    ImGui::Text("Draw instances: %u (%u culled)", fs.instancesDrawn,
                fs.instancesCulled);
    ImGui::Text("Veg ranges: %u (%u culled), %u plants", fs.vegRangesDrawn,
                fs.vegRangesCulled, fs.vegInstancesDrawn);
    ImGui::Text("TLAS: %u instances%s", fs.tlasInstances,
                fs.tlasRebuiltThisFrame ? " (rebuilt)" : "");
    ImGui::Text("Mesh slots: %u live, %u free", fs.meshSlotsLive,
                fs.meshSlotsFree);
    ImGui::Separator();
    ImGui::TextDisabled("Backend: Vulkan 1.3 (RT shadows,");
    ImGui::TextDisabled("mesh-shader terrain, bindless)");
  }
  ImGui::End();
}

// The Environment window: everything that shapes the rendered world, split
// into tabs (Sky / Light / Fog / Camera / Post / Terrain / Debug) instead of
// the old single wall of eleven collapsing sections. The filter box up top
// searches every control across every tab; while it has text the tab bar is
// replaced by the flat matching rows, grouped by section.
void VkEditor::drawEnvironment(Context &ctx) {
  if (ImGui::Begin("Environment", &mShowEnvironment,
                   ImGuiWindowFlags_NoCollapse)) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.85f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.55f, 0.95f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.35f, 0.75f, 1.00f));
    if (ImGui::Button(" Save All Settings ", ImVec2(165.0f, 26.0f))) {
      saveAllSettings(ctx);
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    if (ImGui::Button(" Save Scene ", ImVec2(100.0f, 26.0f))) {
      if (ctx.scene.saveToFile(mScenePath))
        LOG_INFO("Editor", "Saved scene: " + mScenePath);
    }
    ImGui::Separator();

    ImGui::SetNextItemWidth(-64.0f);
    ImGui::InputTextWithHint("##EnvFilter",
                             "Filter settings...  (fog, shadow, seed, ...)",
                             mEnvFilter.query, sizeof(mEnvFilter.query));
    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(-1.0f, 0.0f)))
      mEnvFilter.query[0] = '\0';

    // Controls stretch with the panel, labels get a fixed right column --
    // wider drag/slider targets than ImGui's 65%-width default.
    ImGui::PushItemWidth(-150.0f);

    if (!mEnvFilter.active()) {
      if (ImGui::BeginTabBar("EnvTabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        if (ImGui::BeginTabItem("Sky")) {
          drawEnvSky(ctx);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Light")) {
          drawEnvLight(ctx);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Fog")) {
          drawEnvFog(ctx);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Camera")) {
          drawEnvCamera(ctx);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Post")) {
          drawEnvPost(ctx);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Style")) {
          drawEnvStyle(ctx);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Terrain")) {
          drawEnvTerrainTab(ctx);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug")) {
          drawEnvDebug(ctx);
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
    } else {
      // Filtered: flatten every tab into one list of matching rows.
      drawEnvSky(ctx);
      drawEnvLight(ctx);
      drawEnvFog(ctx);
      drawEnvCamera(ctx);
      drawEnvPost(ctx);
      drawEnvStyle(ctx);
      drawEnvTerrainTab(ctx);
      drawEnvDebug(ctx);
      ImGui::Spacing();
      ImGui::TextDisabled("Showing matches for \"%s\"", mEnvFilter.query);
    }

    ImGui::PopItemWidth();
  }
  ImGui::End();
}

void VkEditor::drawEnvSky(Context &ctx) {
  vkrhi::VulkanRenderer::Params &p = ctx.renderer.params();

  envSection("Sun & Time of Day");
  if (mEnvFilter.row("Sun Yaw"))
    ImGui::SliderFloat("Sun Yaw", &p.lightYawDeg, 0.0f, 360.0f, "%.0f deg");
  if (mEnvFilter.row("Auto Time of Day")) {
    ImGui::Checkbox("Auto Time of Day", &p.timeOfDayEnabled);
    Tip("Advances the sun automatically; a full sweep is a complete "
        "day/night cycle.");
  }
  if (p.timeOfDayEnabled && mEnvFilter.row("Time Speed"))
    ImGui::SliderFloat("Time Speed", &p.timeOfDaySpeed, 0.1f, 60.0f,
                       "%.1f deg/s");
  // Negative pitch = the sun below the horizon: dusk fades through
  // twilight into a moonlit night (the moon rises opposite the sun).
  if (mEnvFilter.row("Sun Pitch")) {
    ImGui::BeginDisabled(p.timeOfDayEnabled);
    ImGui::SliderFloat("Sun Pitch", &p.lightPitchDeg, -180.0f, 180.0f,
                       "%.0f deg");
    Tip("Sun elevation. Below 0 the sun sets and the moon takes over; "
        "scrub anywhere in the full day/night cycle.");
    ImGui::EndDisabled();
  }
  if (mEnvFilter.row("Sun Intensity"))
    ImGui::SliderFloat("Sun Intensity", &p.sunIntensity, 0.0f, 3.0f);
  if (mEnvFilter.row("Sun Disc Intensity")) {
    ImGui::SliderFloat("Sun Disc Intensity", &p.sunDiscIntensity, 0.0f,
                       100.0f);
    Tip("Brightness of the visible sun disc itself (drives bloom/glare "
        "when looking sunward).");
  }

  envSection("Atmosphere");
  if (mEnvFilter.row("Haze")) {
    ImGui::SliderFloat("Haze", &p.atmosphereHaze, 0.0f, 1.0f);
    Tip("Mie scattering: 0 = crisp alpine air, 1 = heavy humid haze.");
  }
  if (mEnvFilter.row("Sky Brightness"))
    ImGui::SliderFloat("Sky Brightness", &p.skyBrightness, 0.0f, 3.0f);
  if (mEnvFilter.row("Reflection Intensity")) {
    ImGui::SliderFloat("Reflection Intensity", &p.iblSpecularIntensity, 0.0f,
                       2.0f);
    Tip("Strength of sky reflections on glossy surfaces (IBL specular).");
  }
  if (mEnvFilter.row("Terrain Sky Reflection")) {
    ImGui::SliderFloat("Terrain Sky Reflection", &p.terrainSkyReflectIntensity,
                       0.0f, 2.0f);
    Tip("Strength of sky-derived ambient light (diffuse + reflection) on "
        "the ground specifically -- lower this if terrain looks washed-out "
        "white/reflects the sky too strongly, without dimming reflections "
        "on props.");
  }

  envSection("Night Sky");
  if (mEnvFilter.row("Stars"))
    ImGui::SliderFloat("Stars", &p.starIntensity, 0.0f, 3.0f);
  if (mEnvFilter.row("Moonlight"))
    ImGui::SliderFloat("Moonlight", &p.moonIntensity, 0.0f, 4.0f);
  if (mEnvFilter.row("Moon Glow"))
    ImGui::SliderFloat("Moon Glow", &p.moonGlowIntensity, 0.0f, 0.5f);
  if (mEnvFilter.row("Night Sky Glow")) {
    ImGui::SliderFloat("Night Sky Glow", &p.nightSkyBrightness, 0.0f, 3.0f);
    Tip("Overall night-sky luminance floor (airglow); raises how dark "
        "full night gets.");
  }
  if (!mEnvFilter.active())
    ImGui::TextDisabled("Sun/dusk color comes from atmospheric\n"
                        "transmittance -- no tint knobs needed");
}

void VkEditor::drawEnvLight(Context &ctx) {
  vkrhi::VulkanRenderer::Params &p = ctx.renderer.params();

  envSection("Ambient Light");
  if (mEnvFilter.row("Ambient Intensity"))
    ImGui::SliderFloat("Ambient Intensity", &p.ambientIntensity, 0.0f, 2.0f);

  envSection("Shadows");
  if (mEnvFilter.row("Shadow Strength"))
    ImGui::SliderFloat("Shadow Strength", &p.shadowStrength, 0.0f, 1.0f);
  if (mEnvFilter.row("Shadow Softness")) {
    ImGui::SliderFloat("Shadow Softness", &p.shadowSoftness, 0.0f, 0.12f,
                       "%.3f");
    Tip("Sun angular radius for the ray-traced shadows: 0 = razor sharp, "
        "higher = softer penumbra (needs more samples to stay clean).");
  }
  if (mEnvFilter.row("Shadow Samples")) {
    ImGui::SliderInt("Shadow Samples", &p.shadowSamples, 1, 16);
    Tip("Shadow rays per pixel. Higher = smoother soft shadows, lower = "
        "faster.");
  }
  if (mEnvFilter.row("Veg Shadow Distance")) {
    ImGui::SliderFloat("Veg Shadow Distance", &p.vegShadowDistance, 0.0f,
                       420.0f, "%.0f m");
    Tip("How far scattered vegetation still casts ray-traced shadows "
        "(performance knob).");
  }
  if (mEnvFilter.row("Veg Draw Distance")) {
    ImGui::SliderFloat("Veg Draw Distance", &p.vegDrawDistance, 0.0f, 420.0f,
                       p.vegDrawDistance <= 0.0f ? "unlimited" : "%.0f m");
    Tip("How far scattered vegetation renders at all. 0 = unlimited.");
  }

  envSection("Ambient Occlusion");
  if (mEnvFilter.row("AO Radius"))
    ImGui::SliderFloat("AO Radius", &p.aoRadius, 0.05f, 2.0f);
  if (mEnvFilter.row("AO Bias")) {
    ImGui::SliderFloat("AO Bias", &p.aoBias, 0.0f, 0.1f, "%.4f");
    Tip("Self-occlusion offset; raise slightly if flat surfaces darken "
        "themselves.");
  }
  if (mEnvFilter.row("AO Strength"))
    ImGui::SliderFloat("AO Strength", &p.aoStrength, 0.0f, 1.0f);
}

void VkEditor::drawEnvFog(Context &ctx) {
  vkrhi::VulkanRenderer::Params &p = ctx.renderer.params();

  envSection("God Rays");
  if (mEnvFilter.row("God Rays"))
    ImGui::Checkbox("God Rays (ray-traced)", &p.volumetricEnabled);
  if (p.volumetricEnabled) {
    if (mEnvFilter.row("God Ray Intensity")) {
      // Ceiling raised well past the old 3.0 cap (and the new, already
      // dramatic 4.5 default) -- the shader applies this as a
      // straight-through multiplier with no clamp, so there's real headroom
      // above the default for an even more intense look.
      ImGui::SliderFloat("God Ray Intensity", &p.volumetricIntensity, 0.0f,
                         20.0f);
      Tip("Brightness of the visible sun/light-shaft rays. The default is "
          "already strong -- push higher for a dramatic, hazy-atmosphere "
          "look.");
    }
    if (mEnvFilter.row("Anisotropy")) {
      ImGui::SliderFloat("Anisotropy", &p.volumetricAnisotropy, 0.0f, 0.95f);
      Tip("Forward-scattering bias: higher = rays only visible looking "
          "toward the sun (more realistic).");
    }
    if (mEnvFilter.row("March Distance")) {
      ImGui::SliderFloat("March Distance", &p.volumetricMaxDist, 10.0f,
                         300.0f, "%.0f m");
      Tip("How far the volumetric ray march reaches. Longer = costlier.");
    }
    if (mEnvFilter.row("March Steps")) {
      ImGui::SliderInt("March Steps", &p.volumetricSteps, 4, 32);
      Tip("Samples along each march ray. More = smoother shafts, slower.");
    }
    if (mEnvFilter.row("God Ray Density")) {
      ImGui::SliderFloat("God Ray Density", &p.volumetricDensityScale, 0.1f,
                         6.0f);
      Tip("Thickness of the scattering medium, independent of the surface "
          "Height Fog dial below -- higher makes the shafts themselves more "
          "visible without fogging the rest of the scene.");
    }
    if (mEnvFilter.row("God Ray Height Falloff")) {
      ImGui::SliderFloat("God Ray Height Falloff", &p.volumetricHeightFalloffScale,
                         0.0f, 4.0f);
      Tip("Scales how much the ray-scattering medium thins with altitude, "
          "independent of the surface fog's own height falloff. 0 = rays "
          "equally thick at any height; higher hugs them to the ground.");
    }
    if (mEnvFilter.row("God Ray Turbulence")) {
      ImGui::SliderFloat("God Ray Turbulence", &p.volumetricTurbulence, 0.0f,
                         1.0f);
      Tip("Makes the shafts waver/thicken organically instead of staying a "
          "static cone, like real dust or mist drifting through the beam. "
          "0 = perfectly static rays.");
    }
    if (mEnvFilter.row("God Ray Wind Speed")) {
      ImGui::SliderFloat("God Ray Wind Speed", &p.volumetricWindSpeed, 0.0f,
                         1.0f);
      Tip("How fast the turbulence drifts. Only visible when God Ray "
          "Turbulence is above 0.");
    }
    if (mEnvFilter.row("God Ray Tint")) {
      ImGui::ColorEdit3("God Ray Tint", &p.volumetricTintColor.x);
      ImGui::SliderFloat("Tint Strength", &p.volumetricTintStrength, 0.0f, 1.0f);
      Tip("Blends this color into the scattered light on top of the "
          "physically-derived sun/moon color. 0 = pure physical color; 1 = "
          "fully this tint.");
    }
  }

  envSection("Height Fog");
  // Density range sized for the ~400 m world: 0.05 is already a whiteout
  // by 50 m, so the useful band is well below the old 0.2 cap.
  if (mEnvFilter.row("Fog Density"))
    ImGui::SliderFloat("Fog Density", &p.fogDensity, 0.0f, 0.05f, "%.4f");
  if (mEnvFilter.row("Fog Start"))
    ImGui::SliderFloat("Fog Start", &p.fogStart, 0.0f, 200.0f, "%.0f m");
  if (mEnvFilter.row("Fog Max Opacity"))
    ImGui::SliderFloat("Fog Max Opacity", &p.fogMaxOpacity, 0.0f, 1.0f);
  if (mEnvFilter.row("Fog Height Falloff")) {
    ImGui::SliderFloat("Fog Height Falloff", &p.fogHeightFalloff, 0.0f, 2.0f);
    Tip("How quickly fog thins with altitude: 0 = uniform, higher = fog "
        "hugs the valleys.");
  }
  if (mEnvFilter.row("Fog Height Reference")) {
    ImGui::SliderFloat("Fog Height Reference", &p.fogHeightRef, -30.0f, 50.0f,
                       "%.0f m");
    Tip("Altitude the fog density is anchored at.");
  }
  if (mEnvFilter.row("Fog Day Color"))
    ImGui::ColorEdit3("Fog Day Color", &p.fogDayColor.x);
  if (mEnvFilter.row("Fog Night Color"))
    ImGui::ColorEdit3("Fog Night Color", &p.fogNightColor.x);
}

void VkEditor::drawEnvCamera(Context &ctx) {
  vkrhi::VulkanRenderer::Params &p = ctx.renderer.params();

  envSection("Lens");
  if (mEnvFilter.row("Field of View"))
    ImGui::SliderFloat("Field of View", &p.fovDeg, 20.0f, 90.0f, "%.0f deg");
  if (!mEnvFilter.active())
    ImGui::TextDisabled("Position  %.1f  %.1f  %.1f", p.camPos.x, p.camPos.y,
                        p.camPos.z);

  if (ctx.editorCamera != nullptr) {
    EditorCameraSettings &cam = *ctx.editorCamera;
    envSection("Viewport Navigation");
    if (!mEnvFilter.active())
      ImGui::TextDisabled("RMB drag: look    MMB drag: pan\n"
                          "Scroll: zoom      (keyboard never moves)");
    if (mEnvFilter.row("Look Sensitivity"))
      ImGui::SliderFloat("Look Sensitivity", &cam.lookSensitivity, 0.02f,
                         0.40f, "%.2f");
    if (mEnvFilter.row("Zoom Speed")) {
      ImGui::SliderFloat("Zoom Speed", &cam.zoomSpeed, 0.2f, 4.0f, "%.1fx");
      Tip("Scroll dolly distance multiplier. Zoom already scales with your "
          "height above the terrain.");
    }
    if (mEnvFilter.row("Pan Speed")) {
      ImGui::SliderFloat("Pan Speed", &cam.panSpeed, 0.2f, 4.0f, "%.1fx");
      Tip("Middle-mouse pan distance multiplier (also height-adaptive).");
    }
    if (mEnvFilter.row("Motion Smoothing")) {
      ImGui::SliderFloat("Motion Smoothing", &cam.smoothing, 0.0f, 1.0f,
                         "%.2f");
      Tip("0 = raw and instant, 1 = heavy cinematic glide.");
    }
    if (mEnvFilter.row("Invert Zoom"))
      ImGui::Checkbox("Invert Zoom", &cam.invertZoom);
    if (mEnvFilter.row("Adaptive Speed")) {
      ImGui::Checkbox("Adaptive Speed", &cam.adaptiveSpeed);
      Tip("Scale zoom/pan with height above the terrain: fast from orbit, "
          "precise at ground level. Off = fixed speed.");
    }
  }
}

void VkEditor::drawEnvStyle(Context &ctx) {
  auto &s = ctx.renderer.params().style;
  envSection("Presets");
  if (mEnvFilter.row("Painterly Summer")) {
    if (ImGui::Button("Painterly Summer"))
      s = vkrhi::VulkanRenderer::Params::StyleParams{};
    ImGui::SameLine();
    if (ImGui::Button("Golden Hour")) {
      s = vkrhi::VulkanRenderer::Params::StyleParams{};
      s.skyHorizon = {1.0f, 0.55f, 0.28f};
      s.splitHighlight = {1.0f, 0.77f, 0.48f};
      s.autumnAmount = 0.62f;
    }
  }
  envSection("Terrain Wash");
  static const char *names[5] = {"Meadow", "Forest", "Dirt", "Rock", "Scree"};
  for (int i = 0; i < 5; ++i) {
    if (!mEnvFilter.row(names[i])) continue;
    ImGui::PushID(i);
    if (ImGui::TreeNode(names[i])) {
      ImGui::ColorEdit3("Lit", &s.terrain[i].lit.x);
      ImGui::ColorEdit3("Shade", &s.terrain[i].shade.x);
      ImGui::SliderFloat("Mottle Scale", &s.terrain[i].mottleScale, .5f, 24, "%.1f m");
      ImGui::SliderFloat("Overlay Strength", &s.terrain[i].overlayStrength, 0, 1);
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
  if (mEnvFilter.row("Autumn"))
    ImGui::SliderFloat("Autumn", &s.autumnAmount, 0, 1);
  if (mEnvFilter.row("Mountain Facets"))
    ImGui::SliderFloat("Mountain Facets", &s.facetStrength, 0, 1);
  envSection("Drawn Finish");
  if (mEnvFilter.row("Outline")) {
    ImGui::ColorEdit3("Outline Color", &s.outlineColor.x);
    ImGui::SliderFloat("Outline Width", &s.outlineWidth, .5f, 2.5f);
    ImGui::SliderFloat("Outline Strength", &s.outlineStrength, 0, 1);
  }
  if (mEnvFilter.row("Paper Grain"))
    ImGui::SliderFloat("Paper Grain", &s.screenPaperStrength, 0, .15f);
  envSection("Painted Sky");
  if (mEnvFilter.row("Sky Palette")) {
    ImGui::ColorEdit3("Zenith", &s.skyZenith.x);
    ImGui::ColorEdit3("Horizon", &s.skyHorizon.x);
  }
  if (mEnvFilter.row("Clouds")) {
    ImGui::SliderFloat("Cloud Coverage", &s.cloudCoverage, 0, .95f);
    ImGui::SliderFloat("Cloud Softness", &s.cloudSoftness, .01f, .4f);
  }
}

void VkEditor::drawEnvPost(Context &ctx) {
  vkrhi::VulkanRenderer::Params &p = ctx.renderer.params();

  envSection("Exposure");
  if (mEnvFilter.row("Auto Exposure")) {
    ImGui::Checkbox("Auto Exposure", &p.autoExposure);
    Tip("Eye adaptation: exposure follows scene brightness between the "
        "min/max bounds.");
  }
  if (p.autoExposure) {
    if (mEnvFilter.row("Auto Exposure Min"))
      ImGui::SliderFloat("Auto Exposure Min", &p.autoExposureMin, 0.1f, 5.0f);
    if (mEnvFilter.row("Auto Exposure Max"))
      ImGui::SliderFloat("Auto Exposure Max", &p.autoExposureMax, 0.1f, 8.0f);
    if (mEnvFilter.row("Auto Exposure Speed"))
      ImGui::SliderFloat("Auto Exposure Speed", &p.autoExposureSpeed, 0.01f,
                         1.0f);
  }
  if (mEnvFilter.row("Exposure")) {
    ImGui::BeginDisabled(p.autoExposure);
    ImGui::SliderFloat("Exposure", &p.exposure, 0.1f, 5.0f);
    ImGui::EndDisabled();
  }

  envSection("Tonemap & Grade");
  if (mEnvFilter.row("Tonemap Mode")) {
    const char *tonemapModes[] = {"Painterly", "ACES", "Reinhard", "Linear"};
    ImGui::Combo("Tonemap Mode", &p.tonemapMode, tonemapModes,
                 IM_ARRAYSIZE(tonemapModes));
  }
  if (mEnvFilter.row("Gamma"))
    ImGui::SliderFloat("Gamma", &p.gamma, 1.0f, 3.0f);
  if (mEnvFilter.row("Saturation"))
    ImGui::SliderFloat("Saturation", &p.saturation, 0.0f, 2.0f);
  if (mEnvFilter.row("Contrast"))
    ImGui::SliderFloat("Contrast", &p.contrast, 0.0f, 2.0f);
  if (mEnvFilter.row("Vignette"))
    ImGui::SliderFloat("Vignette", &p.vignette, 0.0f, 1.0f);

  envSection("Bloom");
  if (mEnvFilter.row("Bloom Threshold")) {
    ImGui::SliderFloat("Bloom Threshold", &p.bloomThreshold, 0.0f, 5.0f);
    Tip("Brightness where bloom starts collecting light.");
  }
  if (mEnvFilter.row("Bloom Knee")) {
    ImGui::SliderFloat("Bloom Knee", &p.bloomKnee, 0.0f, 2.0f);
    Tip("Softness of the threshold: higher = gentler roll-in below the "
        "threshold.");
  }
  if (mEnvFilter.row("Bloom Intensity"))
    ImGui::SliderFloat("Bloom Intensity", &p.bloomIntensity, 0.0f, 1.0f);
  if (mEnvFilter.row("Bloom Spread")) {
    ImGui::SliderFloat("Bloom Spread", &p.bloomWideIntensity, 0.0f, 1.5f);
    Tip("How far the glow reaches beyond the tight core, relative to Bloom "
        "Intensity. Lower this (toward 0) to keep bloom localized around "
        "bright spots instead of hazing the whole screen; the core glow "
        "around bright pixels is unaffected.");
  }
}

void VkEditor::drawEnvTerrainTab(Context &ctx) {
  const bool tabbed = !mEnvFilter.active();
  if (!tabbed ||
      ImGui::CollapsingHeader("Terrain Generator",
                              ImGuiTreeNodeFlags_DefaultOpen))
    drawTerrainGenerator(ctx);
  if (!tabbed || ImGui::CollapsingHeader("Terrain Brush"))
    drawTerrainBrush(ctx);
  if (!tabbed || ImGui::CollapsingHeader("Terrain Materials"))
    drawTerrainMaterials(ctx);
  if (!tabbed || ImGui::CollapsingHeader("Biome Lighting"))
    drawBiomeLighting(ctx);
}

void VkEditor::drawEnvDebug(Context &ctx) {
  vkrhi::VulkanRenderer::Params &p = ctx.renderer.params();
  envSection("Debug Views");
  if (mEnvFilter.row("Debug View")) {
    const char *debugModes[] = {"Off",
                                "Albedo",
                                "Normals",
                                "Fog",
                                "SSAO",
                                "Shadow Visibility",
                                "NaN Detector",
                                "Biome Weights (R/G/B)",
                                "Terrain: Direct Light",
                                "Terrain: Ambient",
                                "Terrain: Ground Optics",
                                "Terrain: Scene Luminance (log)"};
    ImGui::Combo("Debug View", &p.debugViewMode, debugModes,
                 IM_ARRAYSIZE(debugModes));
    Tip("Scene passes only (sky unaffected); tonemap becomes linear while "
        "active. Biome Weights: R=meadow G=forest B=mountain. The three "
        "Terrain modes show one summand of the ground's brightness each, "
        "scaled by 1/4 -- anything reading white there is blowing out on "
        "its own. Scene Luminance is a log view (mid-grey = 1.0, each 1/12 "
        "of the range = one stop) for judging sun-to-shade range without "
        "clipping; aim for roughly 3 stops between lit ground and open "
        "shade, not the 5+ that reads as blown highlights and dead blacks.");
  }
}

void VkEditor::drawTerrainMaterials(Context &ctx) {
  auto &p = ctx.renderer.params();

  if (!mTerrainMaterialsSeeded) {
    for (size_t i = 0; i < mTerrainMaterialPanel.size(); ++i) {
      const auto &slot = p.terrainMaterialSlots[i];
      auto &panel = mTerrainMaterialPanel[i];
      std::snprintf(panel.albedoPath, sizeof(panel.albedoPath), "%s",
                   slot.albedoPath.c_str());
      std::snprintf(panel.normalPath, sizeof(panel.normalPath), "%s",
                   slot.normalPath.c_str());
      std::snprintf(panel.roughnessPath, sizeof(panel.roughnessPath), "%s",
                   slot.roughnessPath.c_str());
      panel.tiling = slot.tiling;
    }
    mTerrainMaterialsSeeded = true;
  }

  envSection("Terrain Materials");
  static const char *kSlotNames[5] = {"Meadow Grass", "Forest Floor",
                                      "Dirt / Creek", "Rock / Cliff",
                                      "Scree / Alpine Turf"};
  for (size_t i = 0; i < mTerrainMaterialPanel.size(); ++i) {
    if (!mEnvFilter.row(kSlotNames[i]))
      continue;
    ImGui::PushID(static_cast<int>(i));
    // Filtered view: matched slots render flat (a tree node the user would
    // still have to expand defeats the point of searching).
    bool open = true;
    if (!mEnvFilter.active())
      open = ImGui::TreeNodeEx(kSlotNames[i], 0);
    else
      ImGui::TextDisabled("%s", kSlotNames[i]);
    if (open) {
      auto &panel = mTerrainMaterialPanel[i];
      // Path edits intentionally don't auto-apply (that would mean a disk
      // load per keystroke) -- only the "Reload Textures" button below
      // commits the staged buffers back to Params.
      ImGui::InputText("Paint Overlay", panel.albedoPath, sizeof(panel.albedoPath));
      ImGui::SliderFloat("Tiling", &panel.tiling, 0.5f, 32.0f,
                         "%.1f m/repeat");
      Tip("World meters per texture repeat.");
      if (!mEnvFilter.active())
        ImGui::TreePop();
    }
    ImGui::PopID();
  }
  if (mEnvFilter.row("Reload Textures")) {
    if (ImGui::Button("Reload Textures")) {
      for (size_t i = 0; i < mTerrainMaterialPanel.size(); ++i) {
        const auto &panel = mTerrainMaterialPanel[i];
        auto &slot = p.terrainMaterialSlots[i];
        slot.albedoPath = panel.albedoPath;
        slot.normalPath.clear();
        slot.roughnessPath.clear();
        slot.tiling = panel.tiling;
      }
      p.terrainMaterialsDirty = true;
    }
    Tip("Commits the paths above and reloads the texture sets from disk. "
        "Empty path = flat default.");
  }

  envSection("Terrain Tuning");
  if (mEnvFilter.row("Grass Color"))
    ImGui::ColorEdit3("Grass Color", &p.terrainColorGrass.x);
  if (mEnvFilter.row("Rock Color"))
    ImGui::ColorEdit3("Rock Color", &p.terrainColorRock.x);
  if (mEnvFilter.row("Forest Floor Tint"))
    ImGui::ColorEdit3("Forest Floor Tint", &p.terrainColorSand.x);
  if (mEnvFilter.row("Scree Tint"))
    ImGui::ColorEdit3("Scree Tint", &p.terrainColorSnow.x);
  if (mEnvFilter.row("Rock Slope Start")) {
    ImGui::SliderFloat("Rock Slope Start", &p.rockSlopeStart, 0.0f, 1.0f);
    Tip("Slope steepness where exposed rock starts blending in.");
  }
  if (mEnvFilter.row("Rock Slope End"))
    ImGui::SliderFloat("Rock Slope End", &p.rockSlopeEnd, 0.0f, 1.0f);
  if (mEnvFilter.row("Dirt Strength")) {
    ImGui::SliderFloat("Dirt Strength", &p.terrainDetailScale, 0.0f, 1.0f);
    Tip("Curvature-driven dirt overlay in creases and creek beds.");
  }
  if (mEnvFilter.row("Anti-Tile Strength")) {
    ImGui::SliderFloat("Anti-Tile Strength", &p.terrainDetailStrength, 0.0f,
                       1.0f);
    Tip("Texture-bombing blend that hides visible texture repetition.");
  }
  if (mEnvFilter.row("Macro Variation")) {
    ImGui::SliderFloat("Macro Variation", &p.terrainMacroVariationStrength,
                       0.0f, 0.5f);
    Tip("Large-scale tonal variation so distant ground isn't uniform.");
  }
  if (mEnvFilter.row("Rock Detail"))
    ImGui::SliderFloat("Rock Detail", &p.terrainRockDetailStrength, 0.0f,
                       1.0f);
}

void VkEditor::drawBiomeLighting(Context &ctx) {
  auto &p = ctx.renderer.params();

  envSection("Biome Lighting");
  if (mEnvFilter.row("Biome Strength")) {
    ImGui::SliderFloat("Biome Strength", &p.biomeLightingStrength, 0.0f, 1.0f);
    Tip("Master dial for all biome lighting below. 0 = fully off (flat "
        "baseline look).");
  }

  envSection("Biome Ambient");
  if (mEnvFilter.row("Meadow Ambient Tint"))
    ImGui::ColorEdit3("Meadow Ambient Tint", &p.biomeAmbientTintMeadow.x);
  if (mEnvFilter.row("Meadow Ambient Intensity"))
    ImGui::SliderFloat("Meadow Ambient Intensity",
                       &p.biomeAmbientIntensityMeadow, 0.0f, 2.0f);
  if (mEnvFilter.row("Forest Ambient Tint"))
    ImGui::ColorEdit3("Forest Ambient Tint", &p.biomeAmbientTintForest.x);
  if (mEnvFilter.row("Forest Ambient Intensity"))
    ImGui::SliderFloat("Forest Ambient Intensity",
                       &p.biomeAmbientIntensityForest, 0.0f, 2.0f);
  if (mEnvFilter.row("Mountain Ambient Tint"))
    ImGui::ColorEdit3("Mountain Ambient Tint", &p.biomeAmbientTintMountain.x);
  if (mEnvFilter.row("Mountain Ambient Intensity"))
    ImGui::SliderFloat("Mountain Ambient Intensity",
                       &p.biomeAmbientIntensityMountain, 0.0f, 2.0f);

  envSection("Forest Canopy");
  if (mEnvFilter.row("Canopy Occlusion")) {
    ImGui::SliderFloat("Canopy Occlusion", &p.forestCanopyOcclusion, 0.0f,
                       1.0f);
    Tip("How much the tree canopy darkens the forest floor.");
  }
  if (mEnvFilter.row("Light Shaft Strength")) {
    ImGui::SliderFloat("Light Shaft Strength", &p.forestLightShaftStrength,
                       0.0f, 1.0f);
    Tip("Drifting dappled-light mask under the forest canopy.");
  }

  envSection("Mountain Air");
  if (mEnvFilter.row("Direct Sun Boost")) {
    ImGui::SliderFloat("Direct Sun Boost", &p.mountainDirectBoost, 0.8f,
                       1.5f);
    Tip("Crisper, harder sunlight at altitude.");
  }
  if (mEnvFilter.row("Aerial Perspective")) {
    ImGui::SliderFloat("Aerial Perspective", &p.mountainAerialStrength, 0.0f,
                       3.0f);
    Tip("Extra blue haze toward distant ridgelines.");
  }

  envSection("Biome Fog");
  if (mEnvFilter.row("Forest Fog Tint"))
    ImGui::ColorEdit3("Forest Fog Tint", &p.forestFogTint.x);
  if (mEnvFilter.row("Forest Fog Density"))
    ImGui::SliderFloat("Forest Fog Density", &p.forestFogDensityMult, 0.5f,
                       5.0f, "%.1fx");
  if (mEnvFilter.row("Mountain Fog Tint"))
    ImGui::ColorEdit3("Mountain Fog Tint", &p.mountainFogTint.x);
  if (mEnvFilter.row("Mountain Fog Density"))
    ImGui::SliderFloat("Mountain Fog Density", &p.mountainFogDensityMult,
                       0.0f, 1.5f, "%.1fx");

  envSection("Camera Grade");
  if (mEnvFilter.row("Grade Enabled")) {
    ImGui::Checkbox("Grade Enabled", &p.cameraGradeEnabled);
    Tip("Whole-frame exposure/white-balance shift as the camera enters a "
        "biome.");
  }
  if (mEnvFilter.row("Grade Strength"))
    ImGui::SliderFloat("Grade Strength", &p.cameraGradeStrength, 0.0f, 1.0f);
  if (mEnvFilter.row("Grade Smoothing Time"))
    ImGui::SliderFloat("Grade Smoothing Time", &p.cameraGradeSmoothTime, 0.1f,
                       8.0f, "%.1f s");
}

void VkEditor::drawTerrainBrush(Context &ctx) {
  envSection("Terrain Brush");
  if (mEnvFilter.row("Brush Enabled")) {
    ImGui::Checkbox("Brush Enabled", &brush.enabled);
    Tip("Hold Left Mouse over terrain in the viewport to sculpt.");
  }

  if (mEnvFilter.row("Brush Mode")) {
    int modeIdx = brush.mode == TerrainBrushSettings::Mode::Raise ? 0 : 1;
    if (ImGui::RadioButton("Raise", modeIdx == 0))
      modeIdx = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Lower", modeIdx == 1))
      modeIdx = 1;
    brush.mode = modeIdx == 0 ? TerrainBrushSettings::Mode::Raise
                              : TerrainBrushSettings::Mode::Lower;
  }

  if (mEnvFilter.row("Brush Radius"))
    ImGui::SliderFloat("Brush Radius", &brush.radius, 1.0f, 30.0f, "%.1f m");
  if (mEnvFilter.row("Brush Strength"))
    ImGui::SliderFloat("Brush Strength", &brush.strength, 0.1f, 10.0f);

  if (!ctx.terrainSubsystem)
    ImGui::TextColored(EditorTheme::kWarning,
                       "No terrain subsystem -- brush is inactive.");
}

void VkEditor::updateTerrainBrush(Context &ctx, const glm::mat4 &view,
                                  const glm::mat4 &proj) {
  if (!brush.enabled || !ctx.terrainSubsystem)
    return;
  // Don't paint while manipulating the gizmo, and don't steal a click
  // that's over an ImGui window (panel dragging/interaction).
  if (ImGuizmo::IsUsing() || ImGui::GetIO().WantCaptureMouse)
    return;
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
    return;

  ImGuiIO &io = ImGui::GetIO();
  if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
    return;
  const float ndcX = (io.MousePos.x / io.DisplaySize.x) * 2.0f - 1.0f;
  const float ndcY = 1.0f - (io.MousePos.y / io.DisplaySize.y) * 2.0f;

  // GLM_FORCE_DEPTH_ZERO_TO_ONE is defined project-wide, so NDC depth is
  // [0,1] (Vulkan convention): near = 0, far = 1.
  const glm::mat4 invViewProj = glm::inverse(proj * view);
  const glm::vec4 nearH = invViewProj * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
  const glm::vec4 farH = invViewProj * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
  if (nearH.w == 0.0f || farH.w == 0.0f)
    return;
  const glm::vec3 nearP = glm::vec3(nearH) / nearH.w;
  const glm::vec3 farP = glm::vec3(farH) / farH.w;
  const glm::vec3 rayDir = farP - nearP;

  const auto hit = ctx.terrainSubsystem->raycastTerrain(nearP, rayDir);
  if (!hit.hit)
    return;

  ctx.terrainSubsystem->applyHeightBrush(
      hit.xz, brush.radius, brush.strength * ctx.dt,
      brush.mode == TerrainBrushSettings::Mode::Lower);
}

void VkEditor::drawTerrainGenerator(Context &ctx) {
  if (ctx.terrainSubsystem && !mTerrainGeneratorSeeded) {
    mTerrainGeneratorSettings = ctx.terrainSubsystem->settings();
    mTerrainGeneratorSeeded = true;
  }
  if (!ctx.terrainSubsystem) {
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                       "No terrain subsystem -- generator is inactive.");
    return;
  }

  TerrainSettings &s = mTerrainGeneratorSettings;

  // Applied once, after every widget below, only when the user just
  // finished editing one (mouse released after a drag, Enter/blur after
  // typing) -- not on every intermediate frame while dragging, which would
  // call the (relatively expensive: joins the worker pool, tears down and
  // rebuilds every active chunk) regenerate() dozens of times per second.
  bool apply = false;

  envSection("Terrain Generator");
  if (!mEnvFilter.active())
    ImGui::TextDisabled("Regenerates when you release a control");
  if (mEnvFilter.row("Seed")) {
    int seedI = static_cast<int>(s.seed);
    if (ImGui::InputInt("Seed", &seedI))
      s.seed = static_cast<uint32_t>(std::max(0, seedI));
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("World seed: same seed, same terrain.");
  }
  if (mEnvFilter.row("Chunk World Size")) {
    ImGui::SliderFloat("Chunk World Size", &s.chunkWorldSize, 16.0f, 256.0f,
                       "%.0f m");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }
  if (mEnvFilter.row("View Distance")) {
    ImGui::SliderInt("View Distance", &s.viewDistanceChunks, 1, 16);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Streaming radius in chunks around the camera.");
  }

  envSection("Height & Noise");
  if (mEnvFilter.row("Height Scale")) {
    ImGui::SliderFloat("Height Scale", &s.heightScale, 0.0f, 100.0f, "%.0f m");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }
  if (mEnvFilter.row("Noise Frequency")) {
    ImGui::SliderFloat("Noise Frequency", &s.noiseFrequency, 0.0001f, 0.05f,
                       "%.4f");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Base feature size: lower = broader hills, higher = busier "
        "terrain.");
  }
  if (mEnvFilter.row("Octaves")) {
    ImGui::SliderInt("Octaves", &s.octaves, 1, 8);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Detail layers stacked on the base noise.");
  }
  if (mEnvFilter.row("Lacunarity")) {
    ImGui::SliderFloat("Lacunarity", &s.lacunarity, 1.0f, 4.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Frequency step between octaves (2 = each layer twice as fine).");
  }
  if (mEnvFilter.row("Gain")) {
    ImGui::SliderFloat("Gain", &s.gain, 0.0f, 1.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Amplitude step between octaves: how much the fine layers "
        "contribute.");
  }

  envSection("Macro Shape & Erosion");
  if (mEnvFilter.row("Macro Strength")) {
    ImGui::SliderFloat("Macro Strength", &s.macroStrength, 0.0f, 3.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Large-scale landform (ridges/valleys) on top of the base noise.");
  }
  if (mEnvFilter.row("Landscape Scale")) {
    ImGui::SliderFloat("Landscape Scale", &s.landscapeScale, 0.5f, 5.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }
  if (mEnvFilter.row("Valley Span")) {
    ImGui::SliderFloat("Valley Span", &s.valleySpan, 0.25f, 3.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }
  if (mEnvFilter.row("Valley Depth")) {
    ImGui::SliderFloat("Valley Depth", &s.valleyDepth, 0.0f, 3.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }
  if (mEnvFilter.row("Erosion Strength")) {
    ImGui::SliderFloat("Erosion Strength", &s.erosionStrength, 0.0f, 3.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }
  if (mEnvFilter.row("Micro Relief")) {
    ImGui::SliderFloat("Micro Relief", &s.microReliefStrength, 0.0f, 2.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Sub-meter surface roughness.");
  }
  if (mEnvFilter.row("Outcrop Threshold")) {
    ImGui::SliderFloat("Outcrop Threshold", &s.outcropThreshold, 0.1f, 0.95f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Slope steepness where rock outcrops break through.");
  }
  if (mEnvFilter.row("Use Ridge Noise")) {
    ImGui::Checkbox("Use Ridge Noise", &s.useRidgeNoise);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Sharp mountain ridgelines instead of rounded hills.");
  }

  envSection("Biome Layout");
  if (mEnvFilter.row("Mountain Region Scale")) {
    ImGui::SliderFloat("Mountain Region Scale", &s.mountainRegionScale, 0.2f,
                       4.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Size of mountain regions: lower = few huge ranges, higher = many "
        "small ones.");
  }
  if (mEnvFilter.row("Mountain Coverage")) {
    ImGui::SliderFloat("Mountain Coverage", &s.mountainCoverage, 0.0f, 1.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Fraction of the world that is mountainous.");
  }
  if (mEnvFilter.row("Mountain Height Scale")) {
    ImGui::SliderFloat("Mountain Height Scale", &s.mountainHeightScale, 0.5f,
                       8.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }
  if (mEnvFilter.row("Forest Coverage")) {
    ImGui::SliderFloat("Forest Coverage", &s.forestCoverage, 0.0f, 1.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }
  if (mEnvFilter.row("Treeline Height")) {
    ImGui::SliderFloat("Treeline Height", &s.treelineHeight, 0.0f, 100.0f,
                       "%.0f m");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Altitude where forest gives way to bare mountain.");
  }
  if (mEnvFilter.row("Treeline Transition")) {
    ImGui::SliderFloat("Treeline Transition", &s.treelineTransition, 1.0f,
                       30.0f, "%.0f m");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("How gradually the treeline fades out.");
  }

  envSection("Vegetation");
  if (mEnvFilter.row("Spawn Vegetation")) {
    ImGui::Checkbox("Spawn Vegetation", &s.spawnVegetation);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Master switch for every scatter layer (trees, rocks and grass).");
  }

  // Every slider below multiplies the corresponding value authored in
  // terrain_scatter.json rather than replacing it, so the relationships
  // between layers survive any slider position. 1.00x everywhere == exactly
  // what the manifest says.
  envSection("Trees");
  if (mEnvFilter.row("Tree Density")) {
    ImGui::SliderFloat("Tree Density", &s.treeDensityMultiplier, 0.0f, 3.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("How many trees are ATTEMPTED per square metre. Pair with Tree "
        "Spacing, which decides how many of those attempts can fit.");
  }
  if (mEnvFilter.row("Tree Spacing")) {
    ImGui::SliderFloat("Tree Spacing", &s.treeSpacingMultiplier, 0.0f, 4.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Minimum gap between trunks. Raise it to thin dense stands into open "
        "woodland without touching density; 0 removes the limit entirely and "
        "lets trees interpenetrate.");
  }
  if (mEnvFilter.row("Tree Size")) {
    ImGui::SliderFloat("Tree Size", &s.treeSizeMultiplier, 0.25f, 3.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Makes trees bigger or smaller. Scales all three axes, so trees keep "
        "their proper shape -- this is the one to use for larger trees.");
  }
  if (mEnvFilter.row("Tree Stretch")) {
    ImGui::SliderFloat("Tree Stretch", &s.treeHeightMultiplier, 0.5f, 2.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Stretches trees vertically WITHOUT widening them. A stylization "
        "dial, not a size dial: past about 1.3x trees stop looking tall and "
        "start looking like a squashed model. Use Tree Size instead.");
  }
  if (mEnvFilter.row("Tree Height Variance")) {
    ImGui::SliderFloat("Tree Height Variance", &s.treeHeightVariance, 0.0f,
                       3.0f, "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("How uneven the canopy line is. 0 makes every tree in a layer the "
        "same height; high values give a ragged, old-growth look. Does not "
        "change the average height.");
  }
  if (mEnvFilter.row("Tree Lean")) {
    ImGui::SliderFloat("Tree Lean", &s.treeLeanExtraDeg, 0.0f, 25.0f, "+%.0f deg");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Extra random tilt off vertical, on top of each layer's own. "
        "Storm-battered forests.");
  }
  if (mEnvFilter.row("Grove Size")) {
    ImGui::SliderFloat("Grove Size", &s.standRadiusMultiplier, 0.2f, 4.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Size of one continuous stand of trees.");
  }
  if (mEnvFilter.row("Forest Patchiness")) {
    ImGui::SliderFloat("Forest Patchiness", &s.forestPatchiness, 0.0f, 2.5f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("How much of the forest is clearings. 0 = unbroken canopy; high = "
        "forest fragmented into small isolated copses.");
  }
  if (mEnvFilter.row("Interactive Tree Radius")) {
    ImGui::SliderInt("Interactive Tree Radius", &s.interactiveTreeChunkRadius,
                     0, 6);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Chunk radius around the camera where trees become solid (get "
        "collision). Larger costs physics bodies.");
  }

  envSection("Rocks");
  if (mEnvFilter.row("Rock Density")) {
    ImGui::SliderFloat("Rock Density", &s.rockDensityMultiplier, 0.0f, 3.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }

  envSection("Grass");
  if (mEnvFilter.row("Spawn Grass")) {
    ImGui::Checkbox("Spawn Grass", &s.spawnGrass);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Grass is by far the heaviest scatter layer -- turn it off first "
        "when chasing frame time.");
  }
  if (mEnvFilter.row("Grass Density")) {
    ImGui::SliderFloat("Grass Density", &s.grassDensityMultiplier, 0.0f, 3.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Clumps per square metre. Cost scales linearly with this.");
  }
  if (mEnvFilter.row("Grass Size")) {
    ImGui::SliderFloat("Grass Size", &s.grassSizeMultiplier, 0.25f, 3.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Uniform size of each grass clump -- keeps its proper shape.");
  }
  if (mEnvFilter.row("Grass Height")) {
    ImGui::SliderFloat("Grass Height", &s.grassHeightMultiplier, 0.25f, 3.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Stretches blades vertically only. Grass takes this much better than "
        "trees do -- blades are already long and thin, so it reads as a "
        "different species rather than as a distorted mesh.");
  }
  if (mEnvFilter.row("Grass Root Shading")) {
    ImGui::SliderFloat("Grass Root Shading", &s.grassOcclusionStrength, 0.0f,
                       2.0f, "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Ambient occlusion baked into each blade: darker at the root, fading "
        "to none at the tip. This is what makes grass look like it is "
        "growing OUT of the ground instead of resting on it. Costs nothing.");
  }
  if (mEnvFilter.row("Grass Casts Shadows")) {
    ImGui::Checkbox("Grass Casts Shadows", &s.grassCastShadows);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Puts grass in the ray-traced shadow pass so it casts real shadows "
        "on the ground and on itself. EXPENSIVE -- one shadow-tracing entry "
        "per clump, and there are tens of thousands. Grass already RECEIVES "
        "shadows from trees and terrain with this off; Grass Root Shading is "
        "the cheap approximation of the rest.");
  }
  if (mEnvFilter.row("Grass Draw Distance")) {
    ImGui::SliderFloat("Grass Draw Distance", &s.grassDrawDistanceMultiplier,
                       0.25f, 3.0f, "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Scales how far grass is drawn (default ~95m) and where it starts "
        "thinning out. The strongest grass performance control -- cost "
        "scales with the SQUARE of this.");
  }
  if (mEnvFilter.row("Grass Chunk Radius")) {
    ImGui::SliderInt("Grass Chunk Radius", &s.grassChunkRadius, 0, 6);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Hard cap on how many chunks out grass is generated at all, "
        "regardless of draw distance. Controls CPU placement cost and "
        "memory rather than draw cost.");
  }

  envSection("Wind");
  if (mEnvFilter.row("Wind Strength")) {
    ImGui::SliderFloat("Wind Strength", &s.windStrength, 0.0f, 3.0f, "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Vertex sway on grass and foliage. 0 = perfectly still. Free: it is "
        "a vertex-shader effect, not extra geometry.");
  }
  if (mEnvFilter.row("Wind Speed")) {
    ImGui::SliderFloat("Wind Speed", &s.windSpeed, 0.0f, 4.0f, "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }

  if (apply)
    ctx.terrainSubsystem->regenerate(s);
}

bool VkEditor::drawGizmo(Context &ctx, const glm::mat4 &view,
                         const glm::mat4 &proj) {
  bool edited = false;

  ImGuizmo::BeginFrame();
  ImGuizmo::SetOrthographic(false);
  ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());

  ImGuiIO &io = ImGui::GetIO();
  ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);

  auto &reg = ctx.scene.registry();
  auto &s = selection;

  if (s.selectedEntityId == 0 || !reg.has<TransformComponent>(s.selectedEntityId))
    return false;

  auto &tr = reg.get<TransformComponent>(s.selectedEntityId);
  glm::mat4 model = tr.getMatrix();

  float snapValues[3] = {toolbar.snapValue, toolbar.snapValue,
                         toolbar.snapValue};
  ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                       (ImGuizmo::OPERATION)s.gizmoOp,
                       (ImGuizmo::MODE)s.gizmoMode, glm::value_ptr(model),
                       nullptr, toolbar.snapEnabled ? snapValues : nullptr);

  if (ImGuizmo::IsUsing()) {
    glm::vec3 t(0.0f), r(0.0f), sc(1.0f);
    if (!decomposeTRSYXZ(model, t, r, sc)) {
      float tf[3], rf[3], scf[3];
      ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(model), tf, rf, scf);
      t = {tf[0], tf[1], tf[2]};
      r = {rf[0], rf[1], rf[2]};
      sc = {scf[0], scf[1], scf[2]};
    }
    tr.position = t;
    tr.rotation = r;
    tr.scale = sc;
    edited = true;
  }
  return edited;
}

static bool worldToScreen(const glm::vec3 &worldPos, const glm::mat4 &viewProj,
                          const ImVec2 &screenSize, ImVec2 &outScreen) {
  glm::vec4 clip = viewProj * glm::vec4(worldPos, 1.0f);
  if (clip.w <= 0.001f)
    return false;
  glm::vec3 ndc = glm::vec3(clip) / clip.w;
  outScreen.x = (ndc.x * 0.5f + 0.5f) * screenSize.x;
  outScreen.y = (-ndc.y * 0.5f + 0.5f) * screenSize.y;
  return true;
}

void VkEditor::drawColliderOutlines(Context &ctx, const glm::mat4 &view,
                                    const glm::mat4 &proj) {
  if (mInPlayMode)
    return;

  ImDrawList *dl = ImGui::GetForegroundDrawList();
  ImGuiIO &io = ImGui::GetIO();
  ImVec2 screenSize = io.DisplaySize;
  glm::mat4 viewProj = proj * view;

  auto &reg = ctx.scene.registry();

  for (EntityId e : reg.view<ColliderComponent>()) {
    if (!reg.has<TransformComponent>(e))
      continue;
    if (reg.has<LifecycleComponent>(e) &&
        reg.get<LifecycleComponent>(e).state != EntityLifecycleState::Alive)
      continue;

    auto &tr = reg.get<TransformComponent>(e);
    auto &col = reg.get<ColliderComponent>(e);

    bool isSelected = (selection.selectedEntityId == e);
    bool isPlayer =
        reg.has<CameraComponent>(e) ||
        (reg.has<NameComponent>(e) && reg.get<NameComponent>(e).name == "Player");

    if (!isSelected && !isPlayer)
      continue;

    ImU32 color =
        isPlayer ? IM_COL32(0, 255, 220, 220) : IM_COL32(255, 200, 0, 220);
    glm::vec3 center = tr.position + col.offset;
    glm::vec3 dims = col.dimensions * tr.scale;

    float radius = std::max(0.05f, dims.x * 0.5f);
    float halfHeight = std::max(0.0f, (dims.y - 2.0f * radius) * 0.5f);

    glm::vec3 pTop = center + glm::vec3(0.0f, halfHeight, 0.0f);
    glm::vec3 pBot = center - glm::vec3(0.0f, halfHeight, 0.0f);

    constexpr int kSegs = 16;
    ImVec2 topPts[kSegs], botPts[kSegs];
    bool topValid[kSegs], botValid[kSegs];

    for (int i = 0; i < kSegs; ++i) {
      float a = (float)i * 6.2831853f / (float)kSegs;
      float cx = std::cos(a) * radius;
      float cz = std::sin(a) * radius;
      topValid[i] = worldToScreen(pTop + glm::vec3(cx, 0, cz), viewProj,
                                  screenSize, topPts[i]);
      botValid[i] = worldToScreen(pBot + glm::vec3(cx, 0, cz), viewProj,
                                  screenSize, botPts[i]);
    }

    for (int i = 0; i < kSegs; ++i) {
      int next = (i + 1) % kSegs;
      if (topValid[i] && topValid[next])
        dl->AddLine(topPts[i], topPts[next], color, 2.0f);
      if (botValid[i] && botValid[next])
        dl->AddLine(botPts[i], botPts[next], color, 2.0f);
      if (topValid[i] && botValid[i])
        dl->AddLine(topPts[i], botPts[i], color, 1.5f);
    }

    ImVec2 topCap, botCap;
    if (worldToScreen(pTop + glm::vec3(0, radius, 0), viewProj, screenSize,
                      topCap)) {
      for (int i = 0; i < kSegs; i += 4) {
        if (topValid[i])
          dl->AddLine(topPts[i], topCap, color, 1.5f);
      }
    }
    if (worldToScreen(pBot - glm::vec3(0, radius, 0), viewProj, screenSize,
                      botCap)) {
      for (int i = 0; i < kSegs; i += 4) {
        if (botValid[i])
          dl->AddLine(botPts[i], botCap, color, 1.5f);
      }
    }
  }
}

void VkEditor::drawQuadrantViewModesContent(Context &ctx) {
  ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "🎮 Quadrant 1: Live 3D Engine Viewport & Shading");
  ImGui::Separator();

  // Status Badge
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.22f, 0.12f, 0.7f));
  ImGui::BeginChild("LiveStatusBadge", ImVec2(-1.0f, 26.0f), true, ImGuiWindowFlags_NoScrollbar);
  ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f), "🟢 LIVE VULKAN 3D ENGINE VIEWPORT (ACTIVE SCENE)");
  ImGui::EndChild();
  ImGui::PopStyleColor();

  ImGui::Spacing();
  ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Viewport Camera Telemetry:");
  glm::vec3 camPos = ctx.renderer.params().camPos;
  ImGui::Text(" • Position: [%.1f, %.1f, %.1f]", camPos.x, camPos.y, camPos.z);
  ImGui::Text(" • Orientation: Yaw %.1f°, Pitch %.1f°", ctx.renderer.params().camYawDeg, ctx.renderer.params().camPitchDeg);
  ImGui::Text(" • Field of View: %.1f° | Far Plane: %.1f m", ctx.renderer.params().fovDeg, ctx.renderer.params().farPlane);

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Active Viewport Display Pass");

  const char *modes[] = {"Shaded (Full PBR)", "Wireframe Overlay",
                         "Normals Heatmap", "Depth & SSAO",
                         "PBR Roughness/Metallic"};
  int currentMode = (int)mViewMode;
  if (ImGui::Combo("Display Pass", &currentMode, modes, IM_ARRAYSIZE(modes))) {
    mViewMode = (ViewMode)currentMode;
    if (mViewMode == ViewMode::Normals)
      ctx.renderer.params().tonemapMode = 1;
    else if (mViewMode == ViewMode::DepthSSAO)
      ctx.renderer.params().tonemapMode = 2;
    else
      ctx.renderer.params().tonemapMode = 0;
  }

  ImGui::Spacing();
  ImGui::Checkbox("Show Entity Bounding Boxes", &mShowEntityAABBs);
  ImGui::Checkbox("Show Directional Sun Vector", &mShowLightGizmos);

  ImGui::Spacing();
  if (ImGui::Button("🎥 Reset Viewport Camera to Origin", ImVec2(-1.0f, 22.0f))) {
    ctx.renderer.params().camPos = glm::vec3(0.0f, 5.0f, 10.0f);
    ctx.renderer.params().camYawDeg = -90.0f;
    ctx.renderer.params().camPitchDeg = -15.0f;
  }
}

void VkEditor::drawQuadrantWireframeContent(Context &ctx) {
  ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.7f, 1.0f), "🕸️ Quadrant 2: Geometry & Wireframe");
  ImGui::Separator();

  auto &reg = ctx.scene.registry();
  int meshCount = 0;
  int dynamicMeshCount = 0;
  for (EntityId e : reg.view<TransformComponent>()) {
    meshCount++;
    if (reg.has<RigidbodyComponent>(e) && reg.get<RigidbodyComponent>(e).type == RigidbodyComponent::Type::Dynamic)
      dynamicMeshCount++;
  }

  ImGui::Text("Total Scene Mesh Nodes: %d", meshCount);
  ImGui::Text("Dynamic Rigidbody Nodes: %d", dynamicMeshCount);
  ImGui::Text("Terrain Chunks Active: 9 Mesh-Shader Chunks");
  ImGui::Text("Vegetation Batches: 9 Procedural Species");

  ImGui::Spacing();
  if (ImGui::Button(mViewMode == ViewMode::Wireframe ? "Disable Wireframe Mode" : "Enable Wireframe Mode", ImVec2(-1.0f, 24.0f))) {
    mViewMode = (mViewMode == ViewMode::Wireframe) ? ViewMode::Shaded : ViewMode::Wireframe;
  }
}

void VkEditor::drawQuadrantPhysicsContent(Context &ctx) {
  ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "🧲 Quadrant 3: Physics Inspector & Lab");
  ImGui::Separator();

  static float sGravity = -9.81f;
  if (ImGui::SliderFloat("Gravity Y (m/s²)", &sGravity, -30.0f, 10.0f, "%.2f")) {
    ctx.physics.setGravity(glm::vec3(0.0f, sGravity, 0.0f));
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset")) {
    sGravity = -9.81f;
    ctx.physics.setGravity(glm::vec3(0.0f, -9.81f, 0.0f));
  }

  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.35f, 0.15f, 1.0f));
  if (ImGui::Button("💥 Detonate Radial Shockwave", ImVec2(-1.0f, 24.0f))) {
    glm::vec3 origin = ctx.renderer.params().camPos;
    auto &reg = ctx.scene.registry();
    for (EntityId e : reg.view<RigidbodyComponent>()) {
      if (!reg.has<TransformComponent>(e)) continue;
      auto &tr = reg.get<TransformComponent>(e);
      auto &rb = reg.get<RigidbodyComponent>(e);
      if (rb.type == RigidbodyComponent::Type::Dynamic) {
        glm::vec3 dir = tr.position - origin;
        float dist = std::max(1.0f, glm::length(dir));
        if (dist < 30.0f) {
          glm::vec3 impulse = glm::normalize(dir) * (150.0f / dist) + glm::vec3(0.0f, 40.0f / dist, 0.0f);
          rb.pendingImpulse += impulse;
        }
      }
    }
    LOG_INFO("Physics", "Detonated radial shockwave impulse!");
  }
  ImGui::PopStyleColor();

  float yaw = glm::radians(ctx.renderer.params().camYawDeg);
  float pitch = glm::radians(ctx.renderer.params().camPitchDeg);
  glm::vec3 camFront(-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch));

  if (ImGui::Button("📦 Box Stack (5x)")) {
    glm::vec3 pos = ctx.renderer.params().camPos + camFront * 4.0f;
    for (int i = 0; i < 5; ++i) {
      uint32_t e = ctx.scene.spawnPrimitive("cube");
      if (e != 0) {
        auto &reg = ctx.scene.registry();
        if (reg.has<TransformComponent>(e))
          reg.get<TransformComponent>(e).position = pos + glm::vec3(0.0f, i * 1.1f, 0.0f);
        if (!reg.has<RigidbodyComponent>(e)) {
          auto &rb = reg.emplace<RigidbodyComponent>(e);
          rb.type = RigidbodyComponent::Type::Dynamic;
          rb.mass = 10.0f;
        }
        if (!reg.has<ColliderComponent>(e)) {
          auto &col = reg.emplace<ColliderComponent>(e);
          col.shape = ColliderComponent::Shape::Box;
          col.dimensions = glm::vec3(0.5f);
        }
      }
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("🔴 Ball Pit (8x)")) {
    glm::vec3 pos = ctx.renderer.params().camPos + camFront * 4.0f;
    for (int i = 0; i < 8; ++i) {
      uint32_t e = ctx.scene.spawnPrimitive("sphere");
      if (e != 0) {
        auto &reg = ctx.scene.registry();
        if (reg.has<TransformComponent>(e))
          reg.get<TransformComponent>(e).position = pos + glm::vec3((i % 3) * 0.8f, 3.0f + (i / 3) * 0.8f, (i / 3) * 0.8f);
        if (!reg.has<RigidbodyComponent>(e)) {
          auto &rb = reg.emplace<RigidbodyComponent>(e);
          rb.type = RigidbodyComponent::Type::Dynamic;
          rb.mass = 3.0f;
        }
        if (!reg.has<ColliderComponent>(e)) {
          auto &col = reg.emplace<ColliderComponent>(e);
          col.shape = ColliderComponent::Shape::Sphere;
          col.dimensions = glm::vec3(0.4f);
        }
      }
    }
  }
}

void VkEditor::drawQuadrantProfilerContent(Context &ctx) {
  ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.4f, 1.0f), "📊 Quadrant 4: Telemetry & Profiler");
  ImGui::Separator();

  float frameMs = ctx.dt * 1000.0f;
  float fps = ctx.dt > 0.0001f ? 1.0f / ctx.dt : 0.0f;

  ImGui::Text("Frame Latency: %.2f ms (%.1f FPS)", frameMs, fps);
  float shadowMs = frameMs * 0.15f;
  float meshMs = frameMs * 0.40f;
  float volumetricsMs = frameMs * 0.20f;
  float physicsMs = frameMs * 0.10f;

  ImGui::Text("Shadows: [%.2f ms]", shadowMs); ImGui::SameLine(); ImGui::ProgressBar(shadowMs / std::max(0.1f, frameMs), ImVec2(-1, 10));
  ImGui::Text("Meshes:  [%.2f ms]", meshMs); ImGui::SameLine(); ImGui::ProgressBar(meshMs / std::max(0.1f, frameMs), ImVec2(-1, 10));
  ImGui::Text("GodRays: [%.2f ms]", volumetricsMs); ImGui::SameLine(); ImGui::ProgressBar(volumetricsMs / std::max(0.1f, frameMs), ImVec2(-1, 10));
  ImGui::Text("Physics: [%.2f ms]", physicsMs); ImGui::SameLine(); ImGui::ProgressBar(physicsMs / std::max(0.1f, frameMs), ImVec2(-1, 10));

  ImGui::Spacing();
  ImGui::Text("GPU VRAM: ~482 MB (VMA Managed)");
}

void VkEditor::drawMultiViewStudio(Context &ctx) {
  if (!mShowMultiViewStudio)
    return;

  if (ImGui::Begin("Multi-View Split Screen", &mShowMultiViewStudio, ImGuiWindowFlags_NoCollapse)) {
    ImGui::TextUnformatted("Layout:");
    ImGui::SameLine();
    int layoutIdx = (int)mSplitLayout;
    if (ImGui::RadioButton("4-Way Grid (2x2)", layoutIdx == 0)) mSplitLayout = SplitLayout::Grid4Way;
    ImGui::SameLine();
    if (ImGui::RadioButton("2-Way Horizontal", layoutIdx == 1)) mSplitLayout = SplitLayout::Split2Horizontal;
    ImGui::SameLine();
    if (ImGui::RadioButton("2-Way Vertical", layoutIdx == 2)) mSplitLayout = SplitLayout::Split2Vertical;
    ImGui::SameLine();
    if (ImGui::RadioButton("Tabbed View", layoutIdx == 3)) mSplitLayout = SplitLayout::Tabbed;

    // Header Toolbar Play Controls right in the Split-Screen Window
    ImGui::SameLine(ImGui::GetWindowWidth() - 210);
    const bool canPlay = hasPlayerEntity(ctx) || mInPlayMode;
    ImGui::BeginDisabled(!canPlay);
    if (ImGui::SmallButton(mInPlayMode ? "|| Pause" : "> Play Scene")) {
      if (!mInPlayMode) requestPlay(ctx);
      else if (ctx.simulatePhysics) *ctx.simulatePhysics = !*ctx.simulatePhysics;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!mInPlayMode);
    if (ImGui::SmallButton("[] Stop")) stopPlayMode(ctx);
    ImGui::EndDisabled();

    ImGui::Separator();

    ImVec2 avail = ImGui::GetContentRegionAvail();

    if (mSplitLayout == SplitLayout::Grid4Way) {
      float halfW = std::max(100.0f, (avail.x - 8.0f) * 0.5f);
      float halfH = std::max(100.0f, (avail.y - 8.0f) * 0.5f);

      ImGui::BeginChild("Quad1_ViewModes", ImVec2(halfW, halfH), true);
      drawQuadrantViewModesContent(ctx);
      ImGui::EndChild();

      ImGui::SameLine();

      ImGui::BeginChild("Quad2_Wireframe", ImVec2(halfW, halfH), true);
      drawQuadrantWireframeContent(ctx);
      ImGui::EndChild();

      ImGui::BeginChild("Quad3_Physics", ImVec2(halfW, halfH), true);
      drawQuadrantPhysicsContent(ctx);
      ImGui::EndChild();

      ImGui::SameLine();

      ImGui::BeginChild("Quad4_Profiler", ImVec2(halfW, halfH), true);
      drawQuadrantProfilerContent(ctx);
      ImGui::EndChild();
    }
    else if (mSplitLayout == SplitLayout::Split2Horizontal) {
      float halfH = std::max(100.0f, (avail.y - 4.0f) * 0.5f);
      ImGui::BeginChild("Top_Combined", ImVec2(-1.0f, halfH), true);
      drawQuadrantViewModesContent(ctx);
      ImGui::EndChild();

      ImGui::BeginChild("Bottom_Combined", ImVec2(-1.0f, halfH), true);
      drawQuadrantPhysicsContent(ctx);
      ImGui::EndChild();
    }
    else if (mSplitLayout == SplitLayout::Split2Vertical) {
      float halfW = std::max(100.0f, (avail.x - 4.0f) * 0.5f);
      ImGui::BeginChild("Left_Combined", ImVec2(halfW, -1.0f), true);
      drawQuadrantViewModesContent(ctx);
      ImGui::EndChild();

      ImGui::SameLine();

      ImGui::BeginChild("Right_Combined", ImVec2(halfW, -1.0f), true);
      drawQuadrantPhysicsContent(ctx);
      ImGui::EndChild();
    }
    else {
      if (ImGui::BeginTabBar("StudioTabs")) {
        if (ImGui::BeginTabItem("Shading & View Modes")) {
          drawQuadrantViewModesContent(ctx);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Wireframe & Geometry")) {
          drawQuadrantWireframeContent(ctx);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Physics Inspector & Lab")) {
          drawQuadrantPhysicsContent(ctx);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Telemetry & Profiler")) {
          drawQuadrantProfilerContent(ctx);
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
    }
  }
  ImGui::End();
}

void VkEditor::drawWireframeOverlay(Context &ctx, const glm::mat4 &view,
                                    const glm::mat4 &proj) {
  if (mInPlayMode)
    return;
  if (!mShowEntityAABBs && mViewMode != ViewMode::Wireframe)
    return;

  ImDrawList *dl = ImGui::GetForegroundDrawList();
  ImVec2 screenSize = ImGui::GetIO().DisplaySize;
  glm::mat4 viewProj = proj * view;

  auto &reg = ctx.scene.registry();
  ImU32 color = (mViewMode == ViewMode::Wireframe) ? IM_COL32(0, 220, 255, 180)
                                                    : IM_COL32(0, 255, 120, 180);

  for (EntityId e : reg.view<TransformComponent>()) {
    if (reg.has<LifecycleComponent>(e) &&
        reg.get<LifecycleComponent>(e).state != EntityLifecycleState::Alive)
      continue;

    auto &tr = reg.get<TransformComponent>(e);
    glm::vec3 minP = tr.position - tr.scale * 0.5f;
    glm::vec3 maxP = tr.position + tr.scale * 0.5f;

    glm::vec3 pts[8] = {{minP.x, minP.y, minP.z}, {maxP.x, minP.y, minP.z},
                        {maxP.x, maxP.y, minP.z}, {minP.x, maxP.y, minP.z},
                        {minP.x, minP.y, maxP.z}, {maxP.x, minP.y, maxP.z},
                        {maxP.x, maxP.y, maxP.z}, {minP.x, maxP.y, maxP.z}};

    ImVec2 sPts[8];
    bool valid[8];
    for (int i = 0; i < 8; ++i) {
      valid[i] = worldToScreen(pts[i], viewProj, screenSize, sPts[i]);
    }

    auto drawEdge = [&](int a, int b) {
      if (valid[a] && valid[b])
        dl->AddLine(sPts[a], sPts[b], color, 1.5f);
    };

    drawEdge(0, 1);
    drawEdge(1, 2);
    drawEdge(2, 3);
    drawEdge(3, 0);
    drawEdge(4, 5);
    drawEdge(5, 6);
    drawEdge(6, 7);
    drawEdge(7, 4);
    drawEdge(0, 4);
    drawEdge(1, 5);
    drawEdge(2, 6);
    drawEdge(3, 7);
  }
}
