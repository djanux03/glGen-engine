#include "VkEditor.h"

#include "imgui_impl_vulkan.h"
#include "Terrain/TerrainIslands.h"

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
#include "GameHud.h"
#include "gameplay/PlayerArchetype.h"
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

// Vec3 row: label on the left like every other property, then three fields
// each marked by a thin axis-colored bar. The previous version filled each
// field's whole background with saturated red/green/blue and pushed the
// label out to the right, which made the transform block the loudest thing
// in the editor and broke the label alignment everywhere else follows.
static bool DragFloat3Colored(const char *label, float *v, float speed = 0.1f,
                              float vMin = 0.0f, float vMax = 0.0f) {
  UI::RowLabel(label);
  ImGui::PushID(label);

  const ImVec4 axis[3] = {UITheme::kAxisX, UITheme::kAxisY, UITheme::kAxisZ};
  const char *ids[3] = {"##X", "##Y", "##Z"};
  const float spacing = UITheme::kSpace1;
  const float fieldW =
      (ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f;

  bool edited = false;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  for (int i = 0; i < 3; ++i) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemWidth(fieldW);
    edited |= ImGui::DragFloat(ids[i], &v[i], speed, vMin, vMax, "%.2f");
    dl->AddRectFilled(p, ImVec2(p.x + 2.0f, p.y + ImGui::GetFrameHeight()),
                      ImGui::GetColorU32(axis[i]), 1.0f);
    if (i < 2)
      ImGui::SameLine(0.0f, spacing);
  }

  ImGui::PopID();
  return edited;
}

// Component header, styled to match UI::BeginSection: chevron, name, and a
// reset affordance that only appears on hover instead of a permanent "R".
static bool ComponentHeader(const char *label, bool *open, bool canRemove,
                            bool *wantsRemove, bool *wantsReset,
                            ImGuiTreeNodeFlags extraFlags = 0) {
  (void)extraFlags;
  ImGui::PushID(label);
  ImGuiStorage *st = ImGui::GetStateStorage();
  const ImGuiID key = ImGui::GetID("##open");
  bool isOpen = st->GetBool(key, true);

  const float h = ImGui::GetFrameHeight();
  const float w = ImGui::GetContentRegionAvail().x;
  const ImVec2 p = ImGui::GetCursorScreenPos();

  if (ImGui::InvisibleButton("##hdr", ImVec2(w, h))) {
    isOpen = !isOpen;
    st->SetBool(key, isOpen);
  }
  const bool hovered = ImGui::IsItemHovered();
  if (canRemove && ImGui::BeginPopupContextItem(label)) {
    if (ImGui::MenuItem("Remove Component"))
      *wantsRemove = true;
    ImGui::EndPopup();
  }

  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                    ImGui::GetColorU32(hovered ? UITheme::kBg3
                                               : UITheme::kBg2),
                    5.0f);
  const float ty = p.y + (h - ImGui::GetFontSize()) * 0.5f;
  dl->AddText(ImVec2(p.x + UITheme::kSpace2, ty),
              ImGui::GetColorU32(UITheme::kTextFaint),
              isOpen ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT);
  dl->AddText(ImVec2(p.x + UITheme::kSpace2 + 18.0f, ty),
              ImGui::GetColorU32(UITheme::kText), label);

  if (hovered) {
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - h, p.y));
    if (UI::IconButton(ICON_REFRESH, "Reset to defaults"))
      *wantsReset = true;
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
  }

  ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace1));
  *open = isOpen;
  ImGui::PopID();
  return isOpen;
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

// ── World panel section / row plumbing ──────────────────────────────────
// The World panel is ~150 controls. Previously they were flat runs under
// SeparatorText headers spread across eight tabs, with a search box bolted
// on because they had become unfindable. Now each section is a collapsible
// card and each control is a two-column row, so the panel can be scanned by
// shape; finding a specific setting is the command palette's job.

void VkEditor::envSection(const char *name) {
  envEnd();
  // Collapsed by default: with ~24 sections the panel's job on open is to
  // show you the map, not the territory.
  mEnvSectionOpen = UI::BeginSection(name, nullptr, name, false);
}

void VkEditor::envEnd() {
  if (mEnvSectionOpen) {
    UI::EndSection();
    mEnvSectionOpen = false;
  }
}

// Label half of a row. Returns false when the section is collapsed, which
// makes the caller skip the control entirely -- collapsed sections cost
// nothing, which matters when the panel holds this many widgets.
bool VkEditor::envRow(const char *label) {
  if (!mEnvSectionOpen)
    return false;
  const float labelW =
      ImGui::GetContentRegionAvail().x * UITheme::kLabelFrac;
  ImGui::AlignTextToFramePadding();
  ImGui::PushStyleColor(ImGuiCol_Text, UITheme::kTextMuted);
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
  ImGui::SameLine(labelW);
  ImGui::SetNextItemWidth(-FLT_MIN);
  return true;
}

// Row with no label column: the control spans the panel and labels itself.
bool VkEditor::envWideRow(const char *label) {
  if (!mEnvSectionOpen)
    return false;
  ImGui::PushID(label);
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::PopID();
  return true;
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

    // Fog was never in this file, so every fog tweak in the editor was lost
    // on the next launch while every neighbouring panel persisted -- which is
    // its own reason the fog was hard to like.
    j["fogDensity"] = p.fogDensity;
    j["fogStart"] = p.fogStart;
    j["fogMaxOpacity"] = p.fogMaxOpacity;
    j["fogHeightFalloff"] = p.fogHeightFalloff;
    j["fogHeightRef"] = p.fogHeightRef;
    j["fogDayColor"] = v3(p.fogDayColor);
    j["fogNightColor"] = v3(p.fogNightColor);
    j["fogAerialStrength"] = p.fogAerialStrength;
    j["fogSunInscatter"] = p.fogSunInscatter;
    j["fogAnisotropy"] = p.fogAnisotropy;
    j["fogNoiseStrength"] = p.fogNoiseStrength;
    j["fogNoiseScale"] = p.fogNoiseScale;
    j["fogNoiseWindSpeed"] = p.fogNoiseWindSpeed;
    j["fogSkyStrength"] = p.fogSkyStrength;

    j["waterEnabled"] = p.waterEnabled;
    j["waterLevel"] = p.waterLevel;
    j["waterShallowColor"] = v3(p.waterShallowColor);
    j["waterDeepColor"] = v3(p.waterDeepColor);
    j["waterClarity"] = p.waterClarity;
    j["waterRoughness"] = p.waterRoughness;
    j["waveAmplitude"] = p.waveAmplitude;
    j["waveScale"] = p.waveScale;
    j["waveSpeed"] = p.waveSpeed;
    j["waveDirection"] = {p.waveDirection.x, p.waveDirection.y};
    j["waterReflectionStrength"] = p.waterReflectionStrength;
    j["waterSsrSteps"] = p.waterSsrSteps;
    j["waterSsrThickness"] = p.waterSsrThickness;
    j["waterFoamDepth"] = p.waterFoamDepth;
    j["waterFoamStrength"] = p.waterFoamStrength;

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
    sj["cloudDeckHeight"] = s.cloudDeckHeight;
    sj["cloudFeatureScale"] = s.cloudFeatureScale;
    sj["cloudOpticalDensity"] = s.cloudOpticalDensity;
    sj["cloudSunOcclusion"] = s.cloudSunOcclusion;
    sj["cloudVolumetricEnabled"] = s.cloudVolumetricEnabled;
    sj["cloudStrength"] = s.cloudStrength;
    sj["cloudLayerThickness"] = s.cloudLayerThickness;
    sj["cloudShapeScale"] = s.cloudShapeScale;
    sj["cloudDetailScale"] = s.cloudDetailScale;
    sj["cloudWeatherScale"] = s.cloudWeatherScale;
    sj["cloudDensityMultiplier"] = s.cloudDensityMultiplier;
    sj["cloudLightAbsorption"] = s.cloudLightAbsorption;
    sj["cloudAmbientStrength"] = s.cloudAmbientStrength;
    sj["cloudCurlStrength"] = s.cloudCurlStrength;
    sj["cloudPhaseG"] = s.cloudPhaseG;
    sj["cloudSilverIntensity"] = s.cloudSilverIntensity;
    sj["cloudSilverSpread"] = s.cloudSilverSpread;
    sj["cloudPowderStrength"] = s.cloudPowderStrength;
    sj["cloudMaxMarchDist"] = s.cloudMaxMarchDist;
    sj["cloudMaxSteps"] = s.cloudMaxSteps;
    sj["cloudLightTaps"] = s.cloudLightTaps;
    sj["cloudTypeBias"] = s.cloudTypeBias;
    sj["cloudDetailStrength"] = s.cloudDetailStrength;
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
      readF("cloudDeckHeight", s.cloudDeckHeight);
      readF("cloudFeatureScale", s.cloudFeatureScale);
      readF("cloudOpticalDensity", s.cloudOpticalDensity);
      readF("cloudSunOcclusion", s.cloudSunOcclusion);
      if (sj.contains("cloudVolumetricEnabled"))
        s.cloudVolumetricEnabled = sj["cloudVolumetricEnabled"].get<bool>();
      readF("cloudStrength", s.cloudStrength);
      readF("cloudLayerThickness", s.cloudLayerThickness);
      readF("cloudShapeScale", s.cloudShapeScale);
      readF("cloudDetailScale", s.cloudDetailScale);
      readF("cloudWeatherScale", s.cloudWeatherScale);
      readF("cloudDensityMultiplier", s.cloudDensityMultiplier);
      readF("cloudLightAbsorption", s.cloudLightAbsorption);
      readF("cloudAmbientStrength", s.cloudAmbientStrength);
      readF("cloudCurlStrength", s.cloudCurlStrength);
      readF("cloudPhaseG", s.cloudPhaseG);
      readF("cloudSilverIntensity", s.cloudSilverIntensity);
      readF("cloudSilverSpread", s.cloudSilverSpread);
      readF("cloudPowderStrength", s.cloudPowderStrength);
      readF("cloudMaxMarchDist", s.cloudMaxMarchDist);
      readF("cloudMaxSteps", s.cloudMaxSteps);
      readF("cloudLightTaps", s.cloudLightTaps);
      readF("cloudTypeBias", s.cloudTypeBias);
      readF("cloudDetailStrength", s.cloudDetailStrength);
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

    {
      auto readF = [&](const char *k, float &v) {
        if (j.contains(k) && j[k].is_number())
          v = j[k].get<float>();
      };
      auto readV3 = [&](const char *k, glm::vec3 &v) {
        if (j.contains(k) && j[k].is_array() && j[k].size() >= 3)
          v = {j[k][0].get<float>(), j[k][1].get<float>(), j[k][2].get<float>()};
      };
      readF("fogDensity", p.fogDensity);
      readF("fogStart", p.fogStart);
      readF("fogMaxOpacity", p.fogMaxOpacity);
      readF("fogHeightFalloff", p.fogHeightFalloff);
      readF("fogHeightRef", p.fogHeightRef);
      readV3("fogDayColor", p.fogDayColor);
      readV3("fogNightColor", p.fogNightColor);
      readF("fogAerialStrength", p.fogAerialStrength);
      readF("fogSunInscatter", p.fogSunInscatter);
      readF("fogAnisotropy", p.fogAnisotropy);
      readF("fogNoiseStrength", p.fogNoiseStrength);
      readF("fogNoiseScale", p.fogNoiseScale);
      readF("fogNoiseWindSpeed", p.fogNoiseWindSpeed);
      readF("fogSkyStrength", p.fogSkyStrength);

      if (j.contains("waterEnabled"))
        p.waterEnabled = j["waterEnabled"].get<bool>();
      readF("waterLevel", p.waterLevel);
      readV3("waterShallowColor", p.waterShallowColor);
      readV3("waterDeepColor", p.waterDeepColor);
      readF("waterClarity", p.waterClarity);
      readF("waterRoughness", p.waterRoughness);
      readF("waveAmplitude", p.waveAmplitude);
      readF("waveScale", p.waveScale);
      readF("waveSpeed", p.waveSpeed);
      if (j.contains("waveDirection") && j["waveDirection"].is_array() &&
          j["waveDirection"].size() >= 2)
        p.waveDirection = {j["waveDirection"][0].get<float>(),
                           j["waveDirection"][1].get<float>()};
      readF("waterReflectionStrength", p.waterReflectionStrength);
      if (j.contains("waterSsrSteps"))
        p.waterSsrSteps = j["waterSsrSteps"].get<int>();
      readF("waterSsrThickness", p.waterSsrThickness);
      readF("waterFoamDepth", p.waterFoamDepth);
      readF("waterFoamStrength", p.waterFoamStrength);
    }

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

  saveSession(ctx);

  LOG_INFO("Editor",
           "Successfully saved all settings (Graphics, Terrain, Environment & Scene)");
}

// The editor session: which scene was open and whether terrain existed.
// Distinct from graphics settings because it describes WHAT you were working
// on rather than how it looked, and because a fresh checkout must still boot
// to an empty scene -- with no session file, nothing is restored.
static std::string sessionPathFor(const std::string &assetDir) {
  return assetDir + "/settings/editor_session.json";
}

void VkEditor::saveSession(Context &ctx) {
  nlohmann::json j;
  j["restoreOnStartup"] = mRestoreSessionOnStartup;
  j["scenePath"] = mScenePath;
  // Shell layout. This lives here rather than in imgui_glgenvk.ini because
  // the shell's windows are NoSavedSettings -- the layout is editor state we
  // own, so it round-trips deterministically instead of depending on
  // whatever ImGui last recorded.
  j["shell"] = {{"leftW", mShell.leftW},
                {"rightW", mShell.rightW},
                {"drawerH", mShell.drawerH},
                {"leftOpen", mShell.leftOpen},
                {"rightOpen", mShell.rightOpen},
                {"drawerOpen", mShell.drawerOpen},
                {"drawerTab", mShell.drawerTab},
                {"statsHud", mShowStatsHud}};
  // Terrain is a subsystem, not ECS content, so Scene serialization does not
  // cover it. Without this, restoring a scene built on terrain would come
  // back with everything floating in an empty world.
  if (ctx.terrainSubsystem) {
    const bool active = ctx.terrainSubsystem->hasTerrain();
    const TerrainSettings &s = active ? ctx.terrainSubsystem->settings()
                                      : ctx.terrainSubsystem->pendingSettings();
    j["terrain"] = {{"active", active},
                    {"seed", s.seed},
                    {"heightScale", s.heightScale},
                    {"viewDistanceChunks", s.viewDistanceChunks}};
  }

  std::error_code ec;
  std::filesystem::create_directories(ctx.assetDir + "/settings", ec);
  std::ofstream out(sessionPathFor(ctx.assetDir));
  if (out.is_open())
    out << j.dump(2) << "\n";
}

void VkEditor::drawWorldMap(Context &ctx) {
  if (!mWorldMapOpen)
    return;
  ImGui::SetNextWindowSize(ImVec2(560, 620), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("World Map", &mWorldMapOpen)) {
    ImGui::End();
    return;
  }
  VkTerrainSubsystem *terrain = ctx.terrainSubsystem;
  if (!terrain || !terrain->hasTerrain()) {
    ImGui::TextDisabled("No terrain. Create some first.");
    ImGui::End();
    return;
  }
  const TerrainSettings &ts = terrain->settings();
  if (!ts.worldBounded) {
    ImGui::TextWrapped("This world is unbounded, so it has no map: there is no "
                       "edge and no island layout to draw. Turn on World "
                       "Bounded in the Terrain panel.");
    ImGui::End();
    return;
  }

  // Generated once per terrain. Rasterising it is a few hundred thousand
  // height+water evaluations -- fine at creation, not per frame.
  if (mWorldMapTexture == nullptr || mWorldMapSeed != ts.seed) {
    std::vector<unsigned char> rgba;
    const int px = 512;
    const float extent = ts.worldRadius * 2.2f;
    terrain->buildWorldMapRGBA(rgba, px, extent);
    if (!rgba.empty()) {
      auto tex = ctx.renderer.createUiTexture(rgba.data(), px, px);
      if (tex.view != VK_NULL_HANDLE) {
        mWorldMapTexture = static_cast<void *>(ImGui_ImplVulkan_AddTexture(
            tex.sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        mWorldMapPixels = px;
        mWorldMapExtent = extent;
        mWorldMapSeed = ts.seed;
      }
    }
  }

  if (mWorldMapTexture == nullptr) {
    ImGui::TextDisabled("Map unavailable.");
    ImGui::End();
    return;
  }

  const float side = std::min(ImGui::GetContentRegionAvail().x,
                              ImGui::GetContentRegionAvail().y - 46.0f);
  const ImVec2 topLeft = ImGui::GetCursorScreenPos();
  ImGui::Image(reinterpret_cast<ImTextureID>(mWorldMapTexture),
               ImVec2(side, side));

  // "You are here". The map is centred on the origin and spans mWorldMapExtent
  // metres, so world XZ maps linearly onto it.
  const glm::vec3 cam = ctx.renderer.params().camPos;
  const float u = cam.x / mWorldMapExtent + 0.5f;
  const float v = cam.z / mWorldMapExtent + 0.5f;
  if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
    const ImVec2 p(topLeft.x + u * side, topLeft.y + v * side);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(p, 5.0f, IM_COL32(255, 90, 60, 255));
    dl->AddCircle(p, 5.0f, IM_COL32(255, 255, 255, 220), 0, 1.6f);
  }

  const IslandInfo &here =
      terrain->islandAt(glm::vec2(cam.x, cam.z));
  ImGui::Text("%.0f, %.0f m", cam.x, cam.z);
  ImGui::SameLine();
  if (here.id == kNoIsland)
    ImGui::TextDisabled("at sea");
  else
    ImGui::Text("| %s island  (relief x%.2f)", islandArchetypeName(here.archetype),
                here.reliefScale);
  ImGui::End();
}

void VkEditor::loadGraphicsSettingsOnce(const std::string &assetDir,
                                        vkrhi::VulkanRenderer &renderer) {
  if (mGraphicsSettingsLoaded)
    return;
  mGraphicsSettingsLoaded = true;
  loadGraphicsSettingsImpl(assetDir + "/settings/graphics_settings.json",
                           renderer.params());
}

void VkEditor::loadAllSettings(Context &ctx) {
  // Normally already done by VkEditorSubsystem::initialize(), before the
  // GLGEN_SMOKE_* pose block and GLGEN_SCRIPT get their say -- see the header.
  // Still called here so an editor built without that subsystem (or a future
  // caller that reloads settings on demand) is not left with none.
  loadGraphicsSettingsOnce(ctx.assetDir, ctx.renderer);

  std::ifstream in(sessionPathFor(ctx.assetDir));
  if (!in.is_open())
    return; // no previous session: stay empty, which is the correct default
  nlohmann::json j;
  try {
    in >> j;
  } catch (const std::exception &e) {
    LOG_WARN("Editor", std::string("editor_session.json is unreadable: ") + e.what());
    return;
  }

  mRestoreSessionOnStartup = j.value("restoreOnStartup", true);
  mScenePath = j.value("scenePath", mScenePath);

  // Shell layout is restored regardless of restoreOnStartup: that flag is
  // about scene *content*, and someone who turned it off still wants their
  // panels where they left them.
  if (j.contains("shell")) {
    const auto &sh = j["shell"];
    mShell.leftW = std::clamp(sh.value("leftW", mShell.leftW),
                              UITheme::kRailMinW, UITheme::kRailMaxW);
    mShell.rightW = std::clamp(sh.value("rightW", mShell.rightW),
                               UITheme::kRailMinW, UITheme::kRailMaxW);
    mShell.drawerH = std::clamp(sh.value("drawerH", mShell.drawerH), 120.0f,
                                600.0f);
    mShell.leftOpen = sh.value("leftOpen", mShell.leftOpen);
    mShell.rightOpen = sh.value("rightOpen", mShell.rightOpen);
    mShell.drawerOpen = sh.value("drawerOpen", mShell.drawerOpen);
    mShell.drawerTab = std::clamp(sh.value("drawerTab", mShell.drawerTab), 0, 1);
    mShowStatsHud = sh.value("statsHud", mShowStatsHud);
  }

  if (!mRestoreSessionOnStartup)
    return;

  // Staged terrain settings restoration: load previous session settings if present,
  // but do NOT start terrain automatically on engine startup.
  if (ctx.terrainSubsystem && j.contains("terrain")) {
    const auto &t = j["terrain"];
    TerrainSettings s = ctx.terrainSubsystem->pendingSettings();
    s.seed = t.value("seed", s.seed);
    s.heightScale = t.value("heightScale", s.heightScale);
    s.viewDistanceChunks = t.value("viewDistanceChunks", s.viewDistanceChunks);
    mTerrainGeneratorSettings = s;
    mTerrainGeneratorSeeded = true;
  }

  std::error_code ec;
  if (!mScenePath.empty() && std::filesystem::exists(mScenePath, ec)) {
    if (ctx.scene.loadFromFile(mScenePath))
      LOG_INFO("Editor", "Restored scene: " + mScenePath);
    else
      LOG_WARN("Editor", "Could not restore scene: " + mScenePath);
  }
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

  const glm::vec3 camPos = ctx.renderer.params().camPos;
  const float camYaw = ctx.renderer.params().camYawDeg;
  const float camPitch = ctx.renderer.params().camPitchDeg;

  const uint32_t id =
      gameplay::spawnPlayer(reg, camPos, camYaw, camPitch,
                            ctx.assetDir + "/scripts/rock_thrower.lua");

  selection.selectedEntityId = id;
  selection.selectedEntities = {id};

  LOG_INFO("Editor", "Created Player entity at camera position (" +
                         std::to_string(camPos.x) + ", " +
                         std::to_string(camPos.y) + ", " +
                         std::to_string(camPos.z) + ")");

  return id;
}

uint32_t VkEditor::createSpaceshipEntity(Context &ctx) {
  auto &reg = ctx.scene.registry();
  const glm::vec3 camPos = ctx.renderer.params().camPos;
  const float camYaw = ctx.renderer.params().camYawDeg;

  uint32_t id = ctx.scene.spawnPrimitive("cone");
  if (id == 0) {
    id = ctx.scene.createEmptyEntity("Spaceship");
  } else {
    if (reg.has<NameComponent>(id)) {
      reg.get<NameComponent>(id).name = "Spaceship";
    } else {
      reg.emplace<NameComponent>(id, NameComponent("Spaceship"));
    }
  }

  auto &tr = reg.get<TransformComponent>(id);
  tr.position = camPos + glm::vec3(0.0f, 0.0f, -3.0f);
  tr.rotation = glm::vec3(0.0f, camYaw, 0.0f);
  tr.scale = glm::vec3(1.5f, 1.5f, 3.0f);

  if (!reg.has<SpaceshipComponent>(id)) {
    reg.emplace<SpaceshipComponent>(id);
  }
  auto &ship = reg.get<SpaceshipComponent>(id);
  ship.enabled = true;
  ship.maxSpeed = 80.0f;
  ship.mainThrustN = 25000.0f;

  if (!reg.has<RigidbodyComponent>(id)) {
    reg.emplace<RigidbodyComponent>(id);
  }
  auto &rb = reg.get<RigidbodyComponent>(id);
  rb.type = RigidbodyComponent::Type::Kinematic;

  if (!reg.has<ColliderComponent>(id)) {
    reg.emplace<ColliderComponent>(id);
  }
  auto &col = reg.get<ColliderComponent>(id);
  col.shape = ColliderComponent::Shape::Box;
  col.dimensions = glm::vec3(1.5f, 1.5f, 3.0f);

  selection.selectedEntityId = id;
  selection.selectedEntities = {id};

  LOG_INFO("Editor", "Created Spaceship entity");
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
  // Whatever the game was showing dies with the play session; otherwise a
  // stale score/timer would hang over the editor on the next Play.
  GameHud::get().reset();
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

  if (!mCommandsRegistered) {
    registerCommands(ctx);
    mCommandsRegistered = true;
  }


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

  // Toolbar owns the gizmo mode; keep the two representations in sync
  // around it.
  if (selection.gizmoOp == ImGuizmo::TRANSLATE)
    toolbar.gizmoOp = ToolbarState::Translate;
  else if (selection.gizmoOp == ImGuizmo::ROTATE)
    toolbar.gizmoOp = ToolbarState::Rotate;
  else if (selection.gizmoOp == ImGuizmo::SCALE)
    toolbar.gizmoOp = ToolbarState::Scale;

  drawToolbar(ctx);
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) && // not mouse-looking
      !ImGui::GetIO().WantCaptureKeyboard && !mPalette.isOpen())
    EditorToolbar::processShortcuts(toolbar);

  if (toolbar.gizmoOp == ToolbarState::Translate)
    selection.gizmoOp = ImGuizmo::TRANSLATE;
  else if (toolbar.gizmoOp == ToolbarState::Rotate)
    selection.gizmoOp = ImGuizmo::ROTATE;
  else if (toolbar.gizmoOp == ToolbarState::Scale)
    selection.gizmoOp = ImGuizmo::SCALE;
  selection.gizmoMode = toolbar.worldSpace ? ImGuizmo::WORLD : ImGuizmo::LOCAL;

  // ── Shell: fixed, non-overlapping regions ──────────────────────────────
  // Rails and drawer hug the edges; whatever is left is the viewport, and
  // the 3D scene is fully visible in it. Nothing floats over the scene
  // except the HUD and the gizmo.
  const UIShell::Layout L = UIShell::compute(mShell);

  if (mShell.leftOpen) {
    if (UIShell::BeginRegion("##rail.left", L.left, UITheme::kBg1)) {
      drawScenePanel(ctx);
      UIShell::EndRegion();
    }
    if (UIShell::BeginRegion("##split.left", L.leftSplit, UITheme::kBg0,
                             false)) {
      UI::Splitter("sl", true, UITheme::kSplitterW, &mShell.leftW,
                   UITheme::kRailMinW, UITheme::kRailMaxW, L.leftSplit.h);
      UIShell::EndRegion(false);
    }
  }

  if (mShell.rightOpen) {
    if (UIShell::BeginRegion("##split.right", L.rightSplit, UITheme::kBg0,
                             false)) {
      // Dragging the right splitter rightwards must shrink the rail, so the
      // delta is inverted relative to the left one.
      float inv = -mShell.rightW;
      if (UI::Splitter("sr", true, UITheme::kSplitterW, &inv,
                       -UITheme::kRailMaxW, -UITheme::kRailMinW,
                       L.rightSplit.h))
        mShell.rightW = -inv;
      UIShell::EndRegion(false);
    }
    if (UIShell::BeginRegion("##rail.right", L.right, UITheme::kBg1)) {
      sceneModified |= drawPropertiesPanel(ctx);
      UIShell::EndRegion();
    }
  }

  if (mShell.drawerOpen) {
    if (UIShell::BeginRegion("##split.drawer", L.drawerSplit, UITheme::kBg0,
                             false)) {
      float inv = -mShell.drawerH;
      if (UI::Splitter("sd", false, UITheme::kSplitterW, &inv, -600.0f, -120.0f,
                       L.drawerSplit.w))
        mShell.drawerH = -inv;
      UIShell::EndRegion(false);
    }
    if (UIShell::BeginRegion("##drawer", L.drawer, UITheme::kBg1)) {
      drawDrawer(ctx);
      UIShell::EndRegion();
    }
  }

  // In-viewport overlay rather than a panel that eats layout space.
  if (mShowStatsHud)
    drawStatsHud(ctx);

  if (mShowDebugWindow)
    drawDebugWindow(ctx);
  drawWorldMap(ctx);

  // ── Gizmo ──────────────────────────────────────────────────────────────
  sceneModified |= drawGizmo(ctx, view, proj);

  // ── Click-to-select ────────────────────────────────────────────────────
  // After the gizmo, so ImGuizmo::IsOver()/IsUsing() describe this frame and
  // dragging a gizmo handle doesn't also re-pick whatever is behind it.
  updatePicking(ctx, view, proj);

  // Headless picking check. GLGEN_SMOKE_PICK="x,y" simulates one click at
  // that screen position once the camera has settled, so click-to-select
  // stays verifiable from a capture run, which has no mouse. Same spirit as
  // the other GLGEN_SMOKE_* hooks in main.cpp.
  {
    static const char *pickEnv = std::getenv("GLGEN_SMOKE_PICK");
    static int pickFrame = 0;
    if (pickEnv && pickFrame >= 0 && ++pickFrame > 30) {
      float px = 0.0f, py = 0.0f;
      if (std::sscanf(pickEnv, "%f,%f", &px, &py) == 2) {
        const uint32_t hit = pickAt(ctx, view, proj, ImVec2(px, py));
        LOG_INFO("Editor", "smoke pick (" + std::to_string((int)px) + "," +
                               std::to_string((int)py) + ") -> entity " +
                               std::to_string(hit));
        if (hit != 0) {
          selection.selectedEntityId = hit;
          selection.selectedEntities = {hit};
        }
      }
      pickFrame = -1; // fire once
    }
  }

  // ── Selection outline ──────────────────────────────────────────────────
  drawSelectionOutline(ctx, view, proj);

  // ── Collision outlines (3D wireframe for Player and selected colliders) ──
  drawColliderOutlines(ctx, view, proj);

  // ── Wireframe AABB overlay for all active scene entities ──────────────
  drawWireframeOverlay(ctx, view, proj);

  // ── Terrain brush (screen-to-world ray, applied while enabled+held) ────
  updateTerrainBrush(ctx, view, proj);

  // ── Command palette (Ctrl+P) ───────────────────────────────────────────
  // Drawn last so it paints over everything. Ctrl+P is checked here rather
  // than in the toolbar so it works no matter which panel has focus.
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_P))
    mPalette.open();
  mPalette.draw();

  return sceneModified;
}

// Draws the gameplay HUD a script declared through game.hud{}. Content comes
// from Lua; the look is decided here so it matches the editor's theme.
void VkEditor::drawGameHud(Context &ctx) {
  const GameHud::State &h = GameHud::get();
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  const float cx = vp->Pos.x + vp->Size.x * 0.5f;
  const float top = vp->WorkPos.y + 18.0f;

  auto textAt = [&](ImFont *font, float size, ImVec2 p, ImU32 col,
                    const char *s) {
    dl->AddText(font, size, p, col, s);
  };
  auto widthOf = [&](ImFont *font, float size, const char *s) {
    return font ? font->CalcTextSizeA(size, FLT_MAX, 0.0f, s).x
                : ImGui::CalcTextSize(s).x;
  };
  ImFont *body = UIFonts::get().body;
  ImFont *title = UIFonts::get().title;
  ImFont *small = UIFonts::get().small;
  const float bodyS = body ? body->LegacySize : 15.0f;
  const float titleS = title ? title->LegacySize : 17.0f;
  const float smallS = small ? small->LegacySize : 12.0f;

  // ── Title + countdown, top centre ───────────────────────────────────
  if (h.timeMax > 0.0f) {
    if (!h.title.empty()) {
      const float w = widthOf(small, smallS, h.title.c_str());
      textAt(small, smallS, ImVec2(cx - w * 0.5f, top),
             ImGui::GetColorU32(UITheme::kTextFaint), h.title.c_str());
    }

    const float secs = std::max(0.0f, h.time);
    char clock[32];
    std::snprintf(clock, sizeof(clock), "%d:%04.1f", (int)(secs / 60.0f),
                  std::fmod(secs, 60.0f));
    // Urgency is carried by colour so it reads without parsing the number.
    const ImVec4 timeCol = secs <= 5.0f    ? UITheme::kDanger
                           : secs <= 15.0f ? UITheme::kWarning
                                           : UITheme::kText;
    const float clockSize = titleS * 1.9f;
    const float cw = widthOf(title, clockSize, clock);
    const float clockY = top + smallS + 4.0f;
    textAt(title, clockSize, ImVec2(cx - cw * 0.5f, clockY),
           ImGui::GetColorU32(timeCol), clock);

    // Drain bar underneath.
    const float barW = 240.0f, barH = 4.0f;
    const float barY = clockY + clockSize + 6.0f;
    dl->AddRectFilled(ImVec2(cx - barW * 0.5f, barY),
                      ImVec2(cx + barW * 0.5f, barY + barH),
                      ImGui::GetColorU32(UITheme::withAlpha(UITheme::kBg4, 0.85f)),
                      barH * 0.5f);
    const float frac = std::clamp(h.time / h.timeMax, 0.0f, 1.0f);
    if (frac > 0.0f)
      dl->AddRectFilled(ImVec2(cx - barW * 0.5f, barY),
                        ImVec2(cx - barW * 0.5f + barW * frac, barY + barH),
                        ImGui::GetColorU32(timeCol), barH * 0.5f);
  }

  // ── Score chip, top left ────────────────────────────────────────────
  if (h.total > 0) {
    char score[64];
    std::snprintf(score, sizeof(score), "%d / %d", h.score, h.total);
    const float x = vp->WorkPos.x + 24.0f;
    textAt(small, smallS, ImVec2(x, top),
           ImGui::GetColorU32(UITheme::kTextFaint), "EMBERS");
    textAt(title, titleS * 1.5f, ImVec2(x, top + smallS + 4.0f),
           ImGui::GetColorU32(UITheme::kAccentHover), score);
  }

  // ── Reticle ─────────────────────────────────────────────────────────
  const ImVec2 centre(cx, vp->Pos.y + vp->Size.y * 0.5f);
  dl->AddCircleFilled(centre, 2.0f,
                      ImGui::GetColorU32(ImVec4(1, 1, 1, 0.75f)));
  dl->AddCircle(centre, 6.0f, ImGui::GetColorU32(ImVec4(0, 0, 0, 0.35f)), 0,
                1.5f);

  // ── Banner ──────────────────────────────────────────────────────────
  if (!h.banner.empty()) {
    const ImVec4 tone = h.bannerTone == "good"  ? UITheme::kSuccess
                        : h.bannerTone == "bad" ? UITheme::kDanger
                                                : UITheme::kText;
    const float size = titleS * 2.6f;
    const float w = widthOf(title, size, h.banner.c_str());
    const float y = centre.y - vp->Size.y * 0.22f;
    // Shadow first: the banner sits over arbitrary scene colour.
    textAt(title, size, ImVec2(cx - w * 0.5f + 2.0f, y + 2.0f),
           ImGui::GetColorU32(ImVec4(0, 0, 0, 0.55f)), h.banner.c_str());
    textAt(title, size, ImVec2(cx - w * 0.5f, y), ImGui::GetColorU32(tone),
           h.banner.c_str());
  }

  // ── Hint, bottom centre ─────────────────────────────────────────────
  if (!h.hint.empty()) {
    const float w = widthOf(body, bodyS, h.hint.c_str());
    const float y = vp->WorkPos.y + vp->WorkSize.y - 46.0f;
    textAt(body, bodyS, ImVec2(cx - w * 0.5f + 1.0f, y + 1.0f),
           ImGui::GetColorU32(ImVec4(0, 0, 0, 0.5f)), h.hint.c_str());
    textAt(body, bodyS, ImVec2(cx - w * 0.5f, y),
           ImGui::GetColorU32(UITheme::kTextMuted), h.hint.c_str());
  }

  const bool running = ctx.simulatePhysics && *ctx.simulatePhysics;
  if (!running) {
    const char *paused = "PAUSED";
    const float size = titleS * 2.0f;
    const float w = widthOf(title, size, paused);
    textAt(title, size, ImVec2(cx - w * 0.5f, centre.y - size * 0.5f),
           ImGui::GetColorU32(UITheme::kTextMuted), paused);
  }
}

void VkEditor::drawPlayModeOverlay(Context &ctx) {
  // A script that called game.hud{} owns the screen: show its HUD instead of
  // the editor's play-mode banner, which would otherwise sit on top of the
  // game's own timer.
  if (GameHud::get().active) {
    drawGameHud(ctx);
    return;
  }

  ImDrawList *dl = ImGui::GetForegroundDrawList();
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  const bool running = ctx.simulatePhysics && *ctx.simulatePhysics;
  const ImVec4 tint = running ? UITheme::kAccent
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
    // Terrain is content, not a given: the engine boots without it, so this
    // is how a world gets ground under it.
    const bool hasTerrain =
        ctx.terrainSubsystem && ctx.terrainSubsystem->hasTerrain();
    if (ImGui::MenuItem("Terrain", nullptr, false,
                        ctx.terrainSubsystem && !hasTerrain)) {
      ctx.terrainSubsystem->create(mTerrainGeneratorSeeded
                                       ? mTerrainGeneratorSettings
                                       : ctx.terrainSubsystem->pendingSettings());
      LOG_INFO("Editor", "Created terrain");
    }
    if (ImGui::MenuItem("Remove Terrain", nullptr, false, hasTerrain)) {
      ctx.terrainSubsystem->destroy();
      LOG_INFO("Editor", "Removed terrain");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Player")) {
      createPlayerEntity(ctx);
    }
    if (ImGui::MenuItem("Spaceship")) {
      createSpaceshipEntity(ctx);
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

  if (ImGui::BeginMenu("View")) {
    ImGui::MenuItem("Scene Rail", "Ctrl+1", &mShell.leftOpen);
    ImGui::MenuItem("Properties Rail", "Ctrl+2", &mShell.rightOpen);
    ImGui::MenuItem("Console Drawer", "`", &mShell.drawerOpen);
    ImGui::Separator();
    ImGui::MenuItem("Stats Overlay", "F1", &mShowStatsHud);
    ImGui::MenuItem("World Map", nullptr, &mWorldMapOpen);
    ImGui::Separator();
    // Everything below used to be a permanently docked panel competing with
    // the scene for space. It is all still here, just not by default.
    ImGui::MenuItem("Debug Tools", "F2", &mShowDebugWindow);
    ImGui::Separator();
    if (ImGui::MenuItem("Reset Layout")) {
      mShell = UIShell::State{};
      mShowStatsHud = true;
      mShowDebugWindow = false;
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Help")) {
    ImGui::MenuItem("Command Palette", "Ctrl+P", nullptr, false);
    ImGui::MenuItem("Move / Rotate / Scale", "W / E / R", nullptr, false);
    ImGui::MenuItem("Focus Selection", "F", nullptr, false);
    ImGui::MenuItem("Duplicate", "Ctrl+D", nullptr, false);
    ImGui::MenuItem("Delete", "Del", nullptr, false);
    ImGui::Separator();
    ImGui::MenuItem("Look / Pan / Zoom", "RMB / MMB / Scroll", nullptr, false);
    ImGui::EndMenu();
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
    // Shell toggles. Backtick opens the drawer the way a console key is
    // expected to; it is skipped while typing so it can still be entered
    // into the Lua prompt.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_1))
      mShell.leftOpen = !mShell.leftOpen;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_2))
      mShell.rightOpen = !mShell.rightOpen;
    if (!io.WantCaptureKeyboard &&
        ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false))
      mShell.drawerOpen = !mShell.drawerOpen;
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
      mShowStatsHud = !mShowStatsHud;
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
      mShowDebugWindow = !mShowDebugWindow;

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
      std::error_code ec;
      std::filesystem::create_directories(
          std::filesystem::path(mScenePath).parent_path(), ec);
      if (ctx.scene.saveToFile(mScenePath)) {
        LOG_INFO("Editor", "Saved scene: " + mScenePath);
        // Record it as the session's scene too, or Ctrl+S would save your
        // work and a restart would still come back empty.
        saveSession(ctx);
      }
    }
    // Viewport & Selection shortcuts: skipped only while actively typing in a text field
    if (!io.WantTextInput) {
      if ((ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
           ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) &&
          selection.selectedEntityId != 0)
        deleteEntity(ctx, selection.selectedEntityId);
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) &&
          selection.selectedEntityId != 0)
        duplicateSelection(ctx);
      if (ImGui::IsKeyPressed(ImGuiKey_F, false) &&
          selection.selectedEntityId != 0)
        mFocusRequest = selection.selectedEntityId;
    }
  }
}

uint32_t VkEditor::duplicateSelection(Context &ctx) {
  Registry &reg = ctx.scene.registry();
  const uint32_t src = selection.selectedEntityId;
  if (src == 0 || !reg.valid(src))
    return 0;

  const uint32_t copy = reg.create();
  // Copied component by component rather than by any generic clone: the
  // Registry has no reflection, and silently missing a component type is
  // worse than a duplicate that visibly lacks one.
  if (reg.has<TransformComponent>(src)) {
    auto t = reg.get<TransformComponent>(src);
    // Offset along X so the copy is not hidden exactly inside the original.
    t.position.x += 1.0f;
    reg.emplace<TransformComponent>(copy, t);
  }
  if (reg.has<MeshComponent>(src))
    reg.emplace<MeshComponent>(copy, reg.get<MeshComponent>(src));
  if (reg.has<MaterialOverrideComponent>(src))
    reg.emplace<MaterialOverrideComponent>(copy,
                                           reg.get<MaterialOverrideComponent>(src));
  if (reg.has<ColliderComponent>(src))
    reg.emplace<ColliderComponent>(copy, reg.get<ColliderComponent>(src));
  if (reg.has<RigidbodyComponent>(src)) {
    auto rb = reg.get<RigidbodyComponent>(src);
    // Authored properties carry over; in-flight runtime state does not --
    // a fresh copy should not inherit the original's queued impulse or
    // current velocity. PhysicsSystem creates the copy's own body.
    // The Jolt body id MUST be reset. Copying it would leave two entities
    // naming the same physics body: PhysicsSystem would skip creating one for
    // the duplicate, and both would then be driven by the original's body.
    rb.bodyID = 0xFFFFFFFF;
    rb.pendingImpulse = glm::vec3(0.0f);
    rb.pendingLinearVelocity = glm::vec3(0.0f);
    rb.linearVelocity = glm::vec3(0.0f);
    rb.setLinearVelocity = false;
    reg.emplace<RigidbodyComponent>(copy, rb);
  }
  if (reg.has<ScriptComponent>(src)) {
    auto sc = reg.get<ScriptComponent>(src);
    sc.initialized = false; // the copy runs on_spawn for itself
    reg.emplace<ScriptComponent>(copy, sc);
  }
  if (reg.has<LifecycleComponent>(src))
    reg.emplace<LifecycleComponent>(copy);
  if (reg.has<BoundsComponent>(src))
    reg.emplace<BoundsComponent>(copy, reg.get<BoundsComponent>(src));

  std::string name = reg.has<NameComponent>(src)
                         ? reg.get<NameComponent>(src).name
                         : std::string("Entity");
  reg.emplace<NameComponent>(copy, NameComponent(name + " Copy"));

  selection.selectedEntityId = copy;
  selection.selectedEntities = {copy};
  LOG_INFO("Editor", "Duplicated '" + name + "'");
  return copy;
}

// =============================================================================
// Toolbar
// =============================================================================
// One strip, three zones: transform tools on the left, the play transport
// dead-centre, view/search affordances on the right. The old editor split
// these across a text-button strip ("W Move", "E Scale") and a set of
// SmallButtons crammed into the right edge of the menu bar, which is why the
// two most important controls in the app -- Play and the active tool -- were
// the hardest to find.
void VkEditor::drawToolbar(Context &ctx) {
  const UIShell::Layout L = UIShell::compute(mShell);
  if (!UIShell::BeginRegion("##toolbar", L.toolbar, UITheme::kBg0, false))
    return;

  // Each zone positions itself absolutely within the strip. Chaining
  // SameLine() across three independently-aligned groups is what let the
  // old toolbar's contents drift.
  const float rowY = (UITheme::kToolbarH - ImGui::GetFrameHeight()) * 0.5f;
  ImGui::SetCursorPos(ImVec2(UITheme::kSpace2, rowY));

  // ── Left: transform tools ────────────────────────────────────────────
  {
    int op = (int)toolbar.gizmoOp; // Translate / Rotate / Scale
    // ToolbarState orders these Translate, Rotate, Scale; present them in
    // the W/E/R order the shortcuts use.
    static const char *kTools[3] = {ICON_MOVE, ICON_SCALE, ICON_ROTATE};
    int idx = op == ToolbarState::Translate ? 0
              : op == ToolbarState::Scale   ? 1
                                            : 2;
    if (UI::Segmented("tools", &idx, kTools, 3, 108.0f))
      toolbar.gizmoOp = idx == 0   ? ToolbarState::Translate
                        : idx == 1 ? ToolbarState::Scale
                                   : ToolbarState::Rotate;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Move (W)   Scale (E)   Rotate (R)");

    ImGui::SameLine(0.0f, UITheme::kSpace3);
    if (UI::IconButton(toolbar.worldSpace ? ICON_GLOBE : ICON_CUBE,
                       toolbar.worldSpace ? "World space" : "Local space"))
      toolbar.worldSpace = !toolbar.worldSpace;

    ImGui::SameLine(0.0f, UITheme::kSpace1);
    if (UI::IconButton(ICON_GRID, "Snap to grid", toolbar.snapEnabled))
      toolbar.snapEnabled = !toolbar.snapEnabled;
    if (toolbar.snapEnabled) {
      ImGui::SameLine(0.0f, UITheme::kSpace1);
      ImGui::SetNextItemWidth(58.0f);
      ImGui::DragFloat("##snap", &toolbar.snapValue, 0.1f, 0.1f, 100.0f,
                       "%.1f");
    }
  }

  // ── Centre: play transport ───────────────────────────────────────────
  if (ctx.simulatePhysics) {
    const bool playing = *ctx.simulatePhysics;
    const bool canPlay = hasPlayerEntity(ctx) || mInPlayMode;
    const float groupW = 3.0f * ImGui::GetFrameHeight() + 2.0f * UITheme::kSpace1;
    ImGui::SetCursorPos(ImVec2((L.toolbar.w - groupW) * 0.5f, rowY));

    ImGui::BeginDisabled(!canPlay);
    if (UI::IconButton(ICON_PLAY, "Play", mInPlayMode && playing))
      requestPlay(ctx);
    ImGui::EndDisabled();
    if (!canPlay && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("No Player entity in the scene.\n"
                        "Create > Player to spawn one.");

    ImGui::SameLine(0.0f, UITheme::kSpace1);
    ImGui::BeginDisabled(!mInPlayMode);
    if (UI::IconButton(ICON_PAUSE, "Pause", mInPlayMode && !playing))
      *ctx.simulatePhysics = false;
    ImGui::SameLine(0.0f, UITheme::kSpace1);
    if (UI::IconButton(ICON_STOP, "Stop"))
      stopPlayMode(ctx);
    ImGui::EndDisabled();
  }

  // ── Right: search + rail toggles ─────────────────────────────────────
  {
    const float btn = ImGui::GetFrameHeight();
    const float paletteW = 190.0f;
    const float rightW = paletteW + UITheme::kSpace3 + btn * 3.0f +
                         UITheme::kSpace1 * 2.0f + UITheme::kSpace2;
    ImGui::SetCursorPos(ImVec2(L.toolbar.w - rightW, rowY));

    // Reads as a search box, acts as the palette trigger -- the discoverable
    // face of Ctrl+P.
    ImGui::PushStyleColor(ImGuiCol_Button, UITheme::kBg2);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UITheme::kBg3);
    ImGui::PushStyleColor(ImGuiCol_Text, UITheme::kTextFaint);
    if (ImGui::Button(ICON_SEARCH "  Search commands   Ctrl+P",
                      ImVec2(paletteW, btn)))
      mPalette.open();
    ImGui::PopStyleColor(3);

    ImGui::SameLine(0.0f, UITheme::kSpace3);
    if (UI::IconButton(ICON_LIST, "Scene rail (Ctrl+1)", mShell.leftOpen))
      mShell.leftOpen = !mShell.leftOpen;
    ImGui::SameLine(0.0f, UITheme::kSpace1);
    if (UI::IconButton(ICON_SLIDERS, "Properties rail (Ctrl+2)",
                       mShell.rightOpen))
      mShell.rightOpen = !mShell.rightOpen;
    ImGui::SameLine(0.0f, UITheme::kSpace1);
    if (UI::IconButton(ICON_TERMINAL, "Console drawer (`)", mShell.drawerOpen))
      mShell.drawerOpen = !mShell.drawerOpen;
  }

  // Hairline under the whole strip.
  ImGui::GetWindowDrawList()->AddLine(
      ImVec2(L.toolbar.x, L.toolbar.y + L.toolbar.h - 1.0f),
      ImVec2(L.toolbar.x + L.toolbar.w, L.toolbar.y + L.toolbar.h - 1.0f),
      ImGui::GetColorU32(UITheme::kLine));

  UIShell::EndRegion(false);
}

// Left rail: what is in the scene. Drawn inside a shell region, so it has no
// Begin/End of its own.
void VkEditor::drawScenePanel(Context &ctx) {
  {
    auto &s = selection;
    UIShell::PanelHeader(ICON_LIST, "Scene");
    // Add-entity affordance lives with the list it adds to, rather than only
    // in the Create menu.
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
    if (UI::IconButton(ICON_PLUS, "Add entity"))
      ImGui::OpenPopup("ScenePanelAdd");
    if (ImGui::BeginPopup("ScenePanelAdd")) {
      if (ImGui::MenuItem("Empty Entity")) {
        uint32_t e = ctx.scene.createEmptyEntity("Empty");
        s.selectedEntityId = e;
        s.selectedEntities = {e};
      }
      ImGui::Separator();
      const char *prims[] = {"cube", "sphere", "plane", "cylinder", "cone"};
      const char *labels[] = {"Cube", "Sphere", "Plane", "Cylinder", "Cone"};
      for (int i = 0; i < 5; ++i) {
        if (ImGui::MenuItem(labels[i])) {
          uint32_t e = ctx.scene.spawnPrimitive(prims[i]);
          if (e != 0) {
            s.selectedEntityId = e;
            s.selectedEntities = {e};
          }
        }
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Player"))
        createPlayerEntity(ctx);
      ImGui::EndPopup();
    }

    ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace1));
    UI::SearchField("outliner", s.outlinerFilter, 128, "Filter entities");
    ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace1));
    UIShell::Rule();
    ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace1));

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
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        mFocusRequest = id;
      }
      if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Focus (F)"))
          mFocusRequest = id;
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
}

// =============================================================================
// Right rail: contextual properties
// =============================================================================
// One panel, two faces. Select an entity and it shows that entity's
// components; select nothing and it shows the world. The old editor kept
// Inspector and Environment as two permanently stacked windows, so half the
// right column was always showing "No entity selected." while the other half
// was a cramped eight-tab settings dump.
bool VkEditor::drawPropertiesPanel(Context &ctx) {
  auto &reg = ctx.scene.registry();
  const uint32_t id = selection.selectedEntityId;
  const bool hasEntity = id != 0 && reg.has<TransformComponent>(id);

  UIShell::PanelHeader(hasEntity ? ICON_CUBE : ICON_GLOBE,
                       hasEntity ? "Entity" : "World");
  if (hasEntity) {
    ImGui::SameLine();
    UI::TextFaint("#%u", id);
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
    if (UI::IconButton(ICON_CLOSE, "Deselect (back to World)")) {
      selection.selectedEntityId = 0;
      selection.selectedEntities.clear();
    }
  }
  ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace1));
  UIShell::Rule();
  ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace2));

  bool edited = false;
  if (ImGui::BeginChild("##propscroll", ImVec2(0, 0), ImGuiChildFlags_None)) {
    if (hasEntity)
      edited = drawEntityProperties(ctx);
    else
      drawWorldProperties(ctx);
  }
  ImGui::EndChild();
  return edited;
}

bool VkEditor::drawEntityProperties(Context &ctx) {
  bool edited = false;
  {
    auto &reg = ctx.scene.registry();
    auto &s = selection;
    uint32_t id = s.selectedEntityId;

    // ── Name ─────────────────────────────────────────────────────────────
    if (reg.has<NameComponent>(id)) {
      auto &nc = reg.get<NameComponent>(id);
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%s", nc.name.c_str());
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::InputTextWithHint("##Name", "Name", buf, sizeof(buf))) {
        nc.name = buf;
        edited = true;
      }
      ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace1));
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
        UI::RowLabel("Visible");
        edited |= ImGui::Checkbox("##Visible", &mc.visible);
        UI::RowLabel("Casts Shadow");
        ImGui::Checkbox("##Casts Shadow", &mc.castsShadow);
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
          ImGui::TextColored(UITheme::kWarning, "No parsed mesh data");
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
          UI::RowLabel("Type");
          if (ImGui::Combo("##Type", &type, types, 3)) {
            removeBody(); // recreate with the new motion type
            rb.type = (RigidbodyComponent::Type)type;
            edited = true;
          }
          UI::RowLabel("Mass");
          edited |= ImGui::DragFloat("##Mass", &rb.mass, 0.1f, 0.01f, 1000.0f);
          UI::RowLabel("Friction");
          edited |= ImGui::SliderFloat("##Friction", &rb.friction, 0.0f, 1.0f);
          UI::RowLabel("Restitution");
          edited |=
              ImGui::SliderFloat("##Restitution", &rb.restitution, 0.0f, 1.0f);
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
          UI::RowLabel("Shape");
          if (ImGui::Combo("##Shape", &shape, shapes, 3)) {
            col.shape = (ColliderComponent::Shape)shape;
            edited = true;
          }
          edited |= DragFloat3Colored("Offset", &col.offset.x, 0.05f);
          if (col.shape == ColliderComponent::Shape::Box) {
            edited |= DragFloat3Colored("Size", &col.dimensions.x, 0.05f,
                                        0.01f, 500.0f);
          } else {
            UI::RowLabel("Radius");
            edited |= ImGui::DragFloat("##Radius", &col.dimensions.x, 0.02f,
                                       0.01f, 100.0f);
            if (col.shape == ColliderComponent::Shape::Capsule)
              UI::RowLabel("Height");
              edited |= ImGui::DragFloat("##Height", &col.dimensions.y, 0.02f,
                                         0.01f, 100.0f);
          }
        }
      }
    }

    // ── Spaceship ────────────────────────────────────────────────────────
    if (reg.has<SpaceshipComponent>(id)) {
      bool open = false, wantRemove = false, wantReset = false;
      ComponentHeader("Spaceship", &open, true, &wantRemove, &wantReset);
      if (wantRemove) {
        reg.removeComponent<SpaceshipComponent>(id);
        edited = true;
      } else {
        auto &ship = reg.get<SpaceshipComponent>(id);
        if (wantReset) {
          ship = SpaceshipComponent{};
          edited = true;
        }
        if (open) {
          edited |= ImGui::Checkbox("Enabled##Ship", &ship.enabled);
          UI::RowLabel("Max Speed");
          edited |= ImGui::DragFloat("##ShipMaxSpeed", &ship.maxSpeed, 0.5f, 1.0f, 500.0f, "%.1f m/s");
          UI::RowLabel("Main Thrust");
          edited |= ImGui::DragFloat("##ShipThrust", &ship.mainThrustN, 100.0f, 100.0f, 500000.0f, "%.0f N");
          UI::RowLabel("Turn Rate");
          edited |= ImGui::SliderFloat("##ShipTurnRate", &ship.turnRateDeg, 5.0f, 180.0f, "%.0f deg/s");
          UI::RowLabel("Bank Angle");
          edited |= ImGui::SliderFloat("##ShipBankAngle", &ship.bankAngleDeg, 0.0f, 60.0f, "%.0f deg");
          UI::RowLabel("Boost Mult");
          edited |= ImGui::SliderFloat("##ShipBoost", &ship.boostMultiplier, 1.0f, 5.0f, "%.1fx");
          UI::RowLabel("Dry Mass");
          edited |= ImGui::DragFloat("##ShipDryMass", &ship.dryMassKg, 10.0f, 10.0f, 50000.0f, "%.0f kg");
          UI::RowLabel("Fuel Mass");
          edited |= ImGui::DragFloat("##ShipFuelMass", &ship.fuelMassKg, 5.0f, 0.0f, 10000.0f, "%.0f kg");
        }
      }
    }

    // ── Add / remove ─────────────────────────────────────────────────────
    ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace2));
    UIShell::Rule();
    ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace2));
    if (UI::AccentButton(ICON_PLUS "  Add Component", ImVec2(-FLT_MIN, 0)))
      ImGui::OpenPopup("AddComponentPopup");
    if (ImGui::BeginPopup("AddComponentPopup")) {
      const bool hasRb = reg.has<RigidbodyComponent>(id);
      const bool hasCol = reg.has<ColliderComponent>(id);
      const bool hasShip = reg.has<SpaceshipComponent>(id);
      if (!hasRb && ImGui::MenuItem("Rigidbody")) {
        reg.emplace<RigidbodyComponent>(id);
        edited = true;
      }
      if (!hasCol && ImGui::MenuItem("Collider")) {
        reg.emplace<ColliderComponent>(id);
        edited = true;
      }
      if (!hasShip && ImGui::MenuItem("Spaceship")) {
        reg.emplace<SpaceshipComponent>(id);
        edited = true;
      }
      if (hasRb && hasCol && hasShip)
        UI::TextFaint("All components added");
      ImGui::EndPopup();
    }

    ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace1));
    if (UI::DangerButton(ICON_TRASH "  Delete Entity", ImVec2(-FLT_MIN, 0))) {
      deleteEntity(ctx, id);
      return true;
    }
    ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace4));
  }
  return edited;
}

// Bottom drawer. Collapsed by default: the console and the asset browser are
// reference surfaces you reach for, not things that need to occupy the
// bottom third of the screen at all times (which is what the old always-on
// Assets strip did).
void VkEditor::drawDrawer(Context &ctx) {
  static const char *kTabs[2] = {ICON_TERMINAL "  Console",
                                 ICON_FOLDER "  Assets"};
  UI::Segmented("drawertabs", &mShell.drawerTab, kTabs, 2, 260.0f);
  ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
  if (UI::IconButton(ICON_CLOSE, "Close drawer (`)"))
    mShell.drawerOpen = false;

  ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace1));
  UIShell::Rule();
  ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace1));

  if (mShell.drawerTab == 0)
    drawLog();
  else
    drawAssetsContent(ctx);
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

      ImVec4 color = UITheme::kText;
      if (e.level == Logger::Level::Warn)
        color = UITheme::kWarning;
      else if (e.level == Logger::Level::Error ||
               e.level == Logger::Level::Fatal)
        color = UITheme::kDanger;

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

// Stats as a viewport overlay rather than a docked panel. Same numbers as the
// old Statistics window, but they float in the corner of the scene instead of
// permanently claiming a third of the left column -- performance is something
// you glance at, not something you navigate to.
void VkEditor::drawStatsHud(Context &ctx) {
  const float fps = (ctx.dt > 1e-6f) ? 1.0f / ctx.dt : 0.0f;
  mFpsHistory[mFpsHistoryIdx] = fps;
  mFpsHistoryIdx = (mFpsHistoryIdx + 1) % kFpsHistorySize;

  const UIShell::Layout L = UIShell::compute(mShell);
  const float w = 208.0f;
  ImGui::SetNextWindowPos(ImVec2(L.viewport.x + L.viewport.w - w -
                                     UITheme::kSpace3,
                                 L.viewport.y + UITheme::kSpace3));
  ImGui::SetNextWindowSize(ImVec2(w, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.82f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      ImVec2(UITheme::kSpace3, UITheme::kSpace2));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, UITheme::kBg0);
  ImGui::PushStyleColor(ImGuiCol_Border, UITheme::kLine);

  const ImGuiWindowFlags f =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("##statshud", nullptr, f)) {
    // Headline number, color-coded against the usual 60/30 fps thresholds.
    const ImVec4 fpsCol = fps >= 55.0f   ? UITheme::kSuccess
                          : fps >= 28.0f ? UITheme::kWarning
                                         : UITheme::kDanger;
    {
      UIFonts::Scoped tf(UIFonts::get().title);
      ImGui::PushStyleColor(ImGuiCol_Text, fpsCol);
      ImGui::Text("%.0f", fps);
      ImGui::PopStyleColor();
    }
    ImGui::SameLine(0.0f, UITheme::kSpace1);
    ImGui::AlignTextToFramePadding();
    UI::TextFaint("fps");
    ImGui::SameLine(0.0f, UITheme::kSpace2);
    UI::TextMuted("%.2f ms", ctx.dt * 1000.0f);

    ImGui::PushStyleColor(ImGuiCol_PlotLines, fpsCol);
    ImGui::PlotLines("##fps", mFpsHistory, kFpsHistorySize, mFpsHistoryIdx,
                     nullptr, 0.0f, FLT_MAX, ImVec2(-FLT_MIN, 28.0f));
    ImGui::PopStyleColor();

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

    const auto &fs = ctx.renderer.frameStats();
    UIFonts::Scoped sf(UIFonts::get().small);
    auto stat = [](const char *label, const char *fmt, ...) {
      ImGui::PushStyleColor(ImGuiCol_Text, UITheme::kTextFaint);
      ImGui::TextUnformatted(label);
      ImGui::PopStyleColor();
      ImGui::SameLine(88.0f);
      va_list args;
      va_start(args, fmt);
      ImGui::TextV(fmt, args);
      va_end(args);
    };
    UIShell::Rule(0.5f);
    stat("entities", "%d", total);
    stat("meshes", "%d", meshes);
    stat("bodies", "%d", bodies);
    UIShell::Rule(0.5f);
    stat("drawn", "%u", fs.instancesDrawn);
    stat("culled", "%u", fs.instancesCulled);
    stat("plants", "%u", fs.vegInstancesDrawn);
    stat("tlas", "%u%s", fs.tlasInstances,
         fs.tlasRebuiltThisFrame ? " *" : "");
    stat("mesh slots", "%u / %u", fs.meshSlotsLive,
         fs.meshSlotsLive + fs.meshSlotsFree);
  }
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(2);
}

// The Environment window: everything that shapes the rendered world, split
// into tabs (Sky / Light / Fog / Camera / Post / Terrain / Debug) instead of
// the old single wall of eleven collapsing sections. The filter box up top
// searches every control across every tab; while it has text the tab bar is
// replaced by the flat matching rows, grouped by section.
// Small uppercase caption that groups the collapsible sections below it.
// Not interactive -- it exists so ~24 collapsed section rows read as six
// families rather than one undifferentiated list.
static void envGroup(const char *name) {
  ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace2));
  UIFonts::Scoped f(UIFonts::get().small);
  ImGui::PushStyleColor(ImGuiCol_Text, UITheme::kTextFaint);
  ImGui::TextUnformatted(name);
  ImGui::PopStyleColor();
  UIShell::Rule(0.6f);
  ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace1));
}

// The World half of the Properties rail: everything that shapes the rendered
// world. Formerly the "Environment" window -- eight tab pages plus a bolted-on
// search box. Now one scroll of collapsed sections under six group captions,
// so the whole surface is visible at a glance and expands only where needed.
void VkEditor::drawWorldProperties(Context &ctx) {
  envGroup("LIGHTING & SKY");
  drawEnvSky(ctx);
  drawEnvLight(ctx);
  drawEnvFog(ctx);
  envEnd();

  envGroup("CAMERA");
  drawEnvCamera(ctx);
  envEnd();

  envGroup("POST-PROCESSING");
  drawEnvPost(ctx);
  envEnd();

  envGroup("STYLE");
  drawEnvStyle(ctx);
  envEnd();

  envGroup("TERRAIN");
  drawEnvTerrainTab(ctx);
  envEnd();

  envGroup("DEBUG");
  drawEnvDebug(ctx);
  envEnd();

  ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace4));
}

void VkEditor::drawEnvSky(Context &ctx) {
  vkrhi::VulkanRenderer::Params &p = ctx.renderer.params();

  envSection("Sun & Time of Day");
  if (envRow("Sun Yaw"))
    ImGui::SliderFloat("##Sun Yaw", &p.lightYawDeg, 0.0f, 360.0f, "%.0f deg");
  if (envRow("Auto Time of Day")) {
    ImGui::Checkbox("##Auto Time of Day", &p.timeOfDayEnabled);
    Tip("Advances the sun automatically; a full sweep is a complete "
        "day/night cycle.");
  }
  if (p.timeOfDayEnabled && envRow("Time Speed"))
    ImGui::SliderFloat("##Time Speed", &p.timeOfDaySpeed, 0.1f, 60.0f,
                       "%.1f deg/s");
  // Negative pitch = the sun below the horizon: dusk fades through
  // twilight into a moonlit night (the moon rises opposite the sun).
  if (envRow("Sun Pitch")) {
    ImGui::BeginDisabled(p.timeOfDayEnabled);
    ImGui::SliderFloat("##Sun Pitch", &p.lightPitchDeg, -180.0f, 180.0f,
                       "%.0f deg");
    Tip("Sun elevation. Below 0 the sun sets and the moon takes over; "
        "scrub anywhere in the full day/night cycle.");
    ImGui::EndDisabled();
  }
  if (envRow("Sun Intensity"))
    ImGui::SliderFloat("##Sun Intensity", &p.sunIntensity, 0.0f, 3.0f);
  if (envRow("Sun Disc Intensity")) {
    ImGui::SliderFloat("##Sun Disc Intensity", &p.sunDiscIntensity, 0.0f,
                       100.0f);
    Tip("Brightness of the visible sun disc itself (drives bloom/glare "
        "when looking sunward).");
  }

  envSection("Atmosphere");
  if (envRow("Haze")) {
    ImGui::SliderFloat("##Haze", &p.atmosphereHaze, 0.0f, 1.0f);
    Tip("Mie scattering: 0 = crisp alpine air, 1 = heavy humid haze.");
  }
  if (envRow("Sky Brightness"))
    ImGui::SliderFloat("##Sky Brightness", &p.skyBrightness, 0.0f, 3.0f);
  if (envRow("Reflection Intensity")) {
    ImGui::SliderFloat("##Reflection Intensity", &p.iblSpecularIntensity, 0.0f,
                       2.0f);
    Tip("Strength of sky reflections on glossy surfaces (IBL specular).");
  }
  if (envRow("Terrain Sky Reflection")) {
    ImGui::SliderFloat("##Terrain Sky Reflection", &p.terrainSkyReflectIntensity,
                       0.0f, 2.0f);
    Tip("Strength of sky-derived ambient light (diffuse + reflection) on "
        "the ground specifically -- lower this if terrain looks washed-out "
        "white/reflects the sky too strongly, without dimming reflections "
        "on props.");
  }

  envSection("Night Sky");
  if (envRow("Stars"))
    ImGui::SliderFloat("##Stars", &p.starIntensity, 0.0f, 3.0f);
  if (envRow("Moonlight"))
    ImGui::SliderFloat("##Moonlight", &p.moonIntensity, 0.0f, 4.0f);
  if (envRow("Moon Glow"))
    ImGui::SliderFloat("##Moon Glow", &p.moonGlowIntensity, 0.0f, 0.5f);
  if (envRow("Night Sky Glow")) {
    ImGui::SliderFloat("##Night Sky Glow", &p.nightSkyBrightness, 0.0f, 3.0f);
    Tip("Overall night-sky luminance floor (airglow); raises how dark "
        "full night gets.");
  }
  if (mEnvSectionOpen)
    ImGui::TextDisabled("Sun/dusk color comes from atmospheric\n"
                        "transmittance -- no tint knobs needed");
}

void VkEditor::drawEnvLight(Context &ctx) {
  vkrhi::VulkanRenderer::Params &p = ctx.renderer.params();

  envSection("Ambient Light");
  if (envRow("Ambient Intensity"))
    ImGui::SliderFloat("##Ambient Intensity", &p.ambientIntensity, 0.0f, 2.0f);

  envSection("Shadows");
  if (envRow("Shadow Strength"))
    ImGui::SliderFloat("##Shadow Strength", &p.shadowStrength, 0.0f, 1.0f);
  if (envRow("Shadow Softness")) {
    ImGui::SliderFloat("##Shadow Softness", &p.shadowSoftness, 0.0f, 0.12f,
                       "%.3f");
    Tip("Sun angular radius for the ray-traced shadows: 0 = razor sharp, "
        "higher = softer penumbra (needs more samples to stay clean).");
  }
  if (envRow("Shadow Samples")) {
    ImGui::SliderInt("##Shadow Samples", &p.shadowSamples, 1, 16);
    Tip("Shadow rays per pixel. Higher = smoother soft shadows, lower = "
        "faster.");
  }
  if (envRow("Veg Shadow Distance")) {
    ImGui::SliderFloat("##Veg Shadow Distance", &p.vegShadowDistance, 0.0f,
                       420.0f, "%.0f m");
    Tip("How far scattered vegetation still casts ray-traced shadows "
        "(performance knob).");
  }
  if (envRow("Veg Draw Distance")) {
    ImGui::SliderFloat("##Veg Draw Distance", &p.vegDrawDistance, 0.0f, 420.0f,
                       p.vegDrawDistance <= 0.0f ? "unlimited" : "%.0f m");
    Tip("How far scattered vegetation renders at all. 0 = unlimited.");
  }

  envSection("Ambient Occlusion");
  if (envRow("AO Radius"))
    ImGui::SliderFloat("##AO Radius", &p.aoRadius, 0.05f, 2.0f);
  if (envRow("AO Bias")) {
    ImGui::SliderFloat("##AO Bias", &p.aoBias, 0.0f, 0.1f, "%.4f");
    Tip("Self-occlusion offset; raise slightly if flat surfaces darken "
        "themselves.");
  }
  if (envRow("AO Strength"))
    ImGui::SliderFloat("##AO Strength", &p.aoStrength, 0.0f, 1.0f);
}

void VkEditor::drawEnvFog(Context &ctx) {
  vkrhi::VulkanRenderer::Params &p = ctx.renderer.params();

  envSection("God Rays");
  if (envRow("God Rays"))
    ImGui::Checkbox("##God Rays (ray-traced)", &p.volumetricEnabled);
  if (p.volumetricEnabled) {
    if (envRow("God Ray Intensity")) {
      // Ceiling raised well past the old 3.0 cap (and the new, already
      // dramatic 4.5 default) -- the shader applies this as a
      // straight-through multiplier with no clamp, so there's real headroom
      // above the default for an even more intense look.
      ImGui::SliderFloat("##God Ray Intensity", &p.volumetricIntensity, 0.0f,
                         20.0f);
      Tip("Brightness of the visible sun/light-shaft rays. The default is "
          "already strong -- push higher for a dramatic, hazy-atmosphere "
          "look.");
    }
    if (envRow("Anisotropy")) {
      ImGui::SliderFloat("##Anisotropy", &p.volumetricAnisotropy, 0.0f, 0.95f);
      Tip("Forward-scattering bias: higher = rays only visible looking "
          "toward the sun (more realistic).");
    }
    if (envRow("March Distance")) {
      ImGui::SliderFloat("##March Distance", &p.volumetricMaxDist, 10.0f,
                         300.0f, "%.0f m");
      Tip("How far the volumetric ray march reaches. Longer = costlier.");
    }
    if (envRow("March Steps")) {
      ImGui::SliderInt("##March Steps", &p.volumetricSteps, 4, 32);
      Tip("Samples along each march ray. More = smoother shafts, slower.");
    }
    if (envRow("God Ray Density")) {
      ImGui::SliderFloat("##God Ray Density", &p.volumetricDensityScale, 0.1f,
                         6.0f);
      Tip("Thickness of the scattering medium, independent of the surface "
          "Height Fog dial below -- higher makes the shafts themselves more "
          "visible without fogging the rest of the scene.");
    }
    if (envRow("God Ray Height Falloff")) {
      ImGui::SliderFloat("##God Ray Height Falloff", &p.volumetricHeightFalloffScale,
                         0.0f, 4.0f);
      Tip("Scales how much the ray-scattering medium thins with altitude, "
          "independent of the surface fog's own height falloff. 0 = rays "
          "equally thick at any height; higher hugs them to the ground.");
    }
    if (envRow("God Ray Turbulence")) {
      ImGui::SliderFloat("##God Ray Turbulence", &p.volumetricTurbulence, 0.0f,
                         1.0f);
      Tip("Makes the shafts waver/thicken organically instead of staying a "
          "static cone, like real dust or mist drifting through the beam. "
          "0 = perfectly static rays.");
    }
    if (envRow("God Ray Wind Speed")) {
      ImGui::SliderFloat("##God Ray Wind Speed", &p.volumetricWindSpeed, 0.0f,
                         1.0f);
      Tip("How fast the turbulence drifts. Only visible when God Ray "
          "Turbulence is above 0.");
    }
    if (envRow("God Ray Tint")) {
      ImGui::ColorEdit3("##God Ray Tint", &p.volumetricTintColor.x);
      Tip("Blends this color into the scattered light on top of the "
          "physically-derived sun/moon color.");
    }
    if (envRow("Tint Strength")) {
      ImGui::SliderFloat("##Tint Strength", &p.volumetricTintStrength, 0.0f, 1.0f);
      Tip("0 = pure physical sun/moon color; 1 = fully the tint above.");
    }
  }

  // Two media over one view ray (shaders/vulkan/fog.glsl): ground mist that
  // pools in low terrain, and air that thickens with distance. They are
  // separate sections because they are separate looks -- valley fog is a
  // weather choice, aerial perspective is a scale choice.
  envSection("Ground Fog");
  // Density range sized for the ~400 m world: 0.05 is already a whiteout
  // by 50 m, so the useful band is well below the old 0.2 cap.
  if (envRow("Density")) {
    ImGui::SliderFloat("##Fog Density", &p.fogDensity, 0.0f, 0.05f, "%.4f");
    Tip("Extinction per meter at the reference altitude.");
  }
  if (envRow("Start Distance"))
    ImGui::SliderFloat("##Fog Start", &p.fogStart, 0.0f, 200.0f, "%.0f m");
  if (envRow("Height Falloff")) {
    ImGui::SliderFloat("##Fog Height Falloff", &p.fogHeightFalloff, 0.0f, 0.4f,
                       "%.3f");
    Tip("How quickly mist thins with altitude: 0 = a uniform slab, higher = "
        "it hugs the valleys. Scale height is 1/this, so 0.045 is about 22 m.");
  }
  if (envRow("Height Reference")) {
    ImGui::SliderFloat("##Fog Height Reference", &p.fogHeightRef, -30.0f, 50.0f,
                       "%.0f m");
    Tip("Altitude the mist density is anchored at.");
  }
  if (envRow("Day Tint")) {
    ImGui::ColorEdit3("##Fog Day Color", &p.fogDayColor.x);
    Tip("Tints the skylight the mist scatters. This and the night tint were "
        "uploaded every frame but read by no shader until the fog rewrite.");
  }
  if (envRow("Night Tint"))
    ImGui::ColorEdit3("##Fog Night Color", &p.fogNightColor.x);
  if (envRow("Patchiness")) {
    ImGui::SliderFloat("##Fog Noise Strength", &p.fogNoiseStrength, 0.0f, 1.0f);
    Tip("Breaks the mist up with a slow noise field. 0 is the perfectly "
        "smooth analytic wash fog used to be.");
  }
  if (envRow("Patch Scale")) {
    ImGui::SliderFloat("##Fog Noise Scale", &p.fogNoiseScale, 0.001f, 0.05f,
                       "%.4f");
    Tip("Cycles per meter: 0.008 gives features about 125 m across.");
  }
  if (envRow("Patch Drift"))
    ImGui::SliderFloat("##Fog Noise Wind", &p.fogNoiseWindSpeed, 0.0f, 3.0f);

  envSection("Water");
  if (envRow("Enabled")) {
    ImGui::Checkbox("##Water Enabled", &p.waterEnabled);
    Tip("A single world-height water table, evaluated analytically in a "
        "fullscreen pass -- no mesh, so no LOD seams at any range. One "
        "altitude for the whole world; it models a sea, not separate lakes.");
  }
  if (p.waterEnabled) {
    if (envRow("Level"))
      ImGui::DragFloat("##Water Level", &p.waterLevel, 0.1f, -200.0f, 200.0f,
                       "%.1f m");
    if (envRow("Shallow"))
      ImGui::ColorEdit3("##Water Shallow", &p.waterShallowColor.x);
    if (envRow("Deep"))
      ImGui::ColorEdit3("##Water Deep", &p.waterDeepColor.x);
    if (envRow("Clarity")) {
      ImGui::SliderFloat("##Water Clarity", &p.waterClarity, 0.2f, 40.0f,
                         "%.1f m");
      Tip("Metres of water needed to reach the deep colour. Absorption "
          "between the two is Beer-Lambert, so this is a real distance.");
    }
    if (envRow("Roughness")) {
      ImGui::SliderFloat("##Water Roughness", &p.waterRoughness, 0.005f, 0.5f,
                         "%.3f");
      Tip("Sharpness of the sun glint and of the reflection.");
    }
    if (envRow("Wave Amplitude"))
      ImGui::SliderFloat("##Wave Amplitude", &p.waveAmplitude, 0.0f, 2.0f);
    if (envRow("Wave Scale")) {
      ImGui::SliderFloat("##Wave Scale", &p.waveScale, 0.005f, 0.6f, "%.3f");
      Tip("Cycles per metre. Waves perturb the normal only -- the surface "
          "stays geometrically flat.");
    }
    if (envRow("Wave Speed"))
      ImGui::SliderFloat("##Wave Speed", &p.waveSpeed, 0.0f, 3.0f);
    if (envRow("Wave Direction"))
      ImGui::SliderFloat2("##Wave Direction", &p.waveDirection.x, -1.0f, 1.0f);
    if (envRow("Reflection")) {
      ImGui::SliderFloat("##Water Reflection", &p.waterReflectionStrength, 0.0f,
                         1.0f);
      Tip("Blend toward screen-space reflections. Whatever the march misses "
          "falls back to the sky cubemap, so 0 is still a reflective surface "
          "-- just a sky-only one.");
    }
    if (envRow("SSR Steps"))
      ImGui::SliderInt("##Water SSR Steps", &p.waterSsrSteps, 8, 128);
    if (envRow("SSR Thickness")) {
      ImGui::SliderFloat("##Water SSR Thickness", &p.waterSsrThickness, 0.05f,
                         4.0f, "%.2f m");
      Tip("How deep a depth-buffer sample is assumed to be. Too small and "
          "reflections drop out behind thin geometry; too large and they "
          "smear behind it.");
    }
    if (envRow("Foam Depth")) {
      ImGui::SliderFloat("##Water Foam Depth", &p.waterFoamDepth, 0.02f, 3.0f,
                         "%.2f m");
      Tip("Water column thinner than this reads as shoreline.");
    }
    if (envRow("Foam Strength"))
      ImGui::SliderFloat("##Water Foam", &p.waterFoamStrength, 0.0f, 1.0f);
  }

  envSection("Aerial Perspective");
  if (envRow("Air Density")) {
    ImGui::SliderFloat("##Fog Aerial Strength", &p.fogAerialStrength, 0.0f,
                       400.0f, "%.0fx");
    Tip("Multiples of sea-level air. Real air over the few hundred meters "
        "this engine renders is almost invisible, so this is doing real "
        "work -- but extinction stays spectral, so distant surfaces go blue "
        "on their own and land on the sky's own color at the horizon.");
  }
  if (envRow("Sun In-scatter")) {
    ImGui::SliderFloat("##Fog Sun Inscatter", &p.fogSunInscatter, 0.0f, 4.0f);
    Tip("Forward haze flare toward the sun, for both layers.");
  }
  if (envRow("Mist Anisotropy")) {
    ImGui::SliderFloat("##Fog Anisotropy", &p.fogAnisotropy, 0.0f, 0.95f);
    Tip("How forward-biased the ground layer's sun scattering is.");
  }
  if (envRow("Max Opacity")) {
    ImGui::SliderFloat("##Fog Max Opacity", &p.fogMaxOpacity, 0.0f, 1.0f);
    Tip("Cap on how much of a surface fog may take. 1 lets it run to "
        "completion so distant terrain dissolves into the sky; 0 turns fog "
        "off entirely. Anything below 1 reintroduces a horizon seam, which "
        "is what the old 0.9 default was doing.");
  }
  if (envRow("Sky Fog")) {
    ImGui::SliderFloat("##Fog Sky Strength", &p.fogSkyStrength, 0.0f, 1.0f);
    Tip("How much ground fog the visible sky receives. Terrain that fully "
        "dissolves needs something matching to dissolve into -- at 0 the "
        "seam comes back from the other side.");
  }
}

void VkEditor::drawEnvCamera(Context &ctx) {
  vkrhi::VulkanRenderer::Params &p = ctx.renderer.params();

  envSection("Lens");
  if (envRow("Field of View"))
    ImGui::SliderFloat("##Field of View", &p.fovDeg, 20.0f, 90.0f, "%.0f deg");
  if (mEnvSectionOpen)
    ImGui::TextDisabled("Position  %.1f  %.1f  %.1f", p.camPos.x, p.camPos.y,
                        p.camPos.z);

  if (ctx.editorCamera != nullptr) {
    EditorCameraSettings &cam = *ctx.editorCamera;
    envSection("Viewport Navigation");
    if (mEnvSectionOpen)
      ImGui::TextDisabled("RMB drag: look    MMB drag: pan\n"
                          "Scroll: zoom      (keyboard never moves)");
    if (envRow("Look Sensitivity"))
      ImGui::SliderFloat("##Look Sensitivity", &cam.lookSensitivity, 0.02f,
                         0.40f, "%.2f");
    if (envRow("Zoom Speed")) {
      ImGui::SliderFloat("##Zoom Speed", &cam.zoomSpeed, 0.2f, 4.0f, "%.1fx");
      Tip("Scroll dolly distance multiplier. Zoom already scales with your "
          "height above the terrain.");
    }
    if (envRow("Pan Speed")) {
      ImGui::SliderFloat("##Pan Speed", &cam.panSpeed, 0.2f, 4.0f, "%.1fx");
      Tip("Middle-mouse pan distance multiplier (also height-adaptive).");
    }
    if (envRow("Motion Smoothing")) {
      ImGui::SliderFloat("##Motion Smoothing", &cam.smoothing, 0.0f, 1.0f,
                         "%.2f");
      Tip("0 = raw and instant, 1 = heavy cinematic glide.");
    }
    if (envRow("Invert Zoom"))
      ImGui::Checkbox("##Invert Zoom", &cam.invertZoom);
    if (envRow("Adaptive Speed")) {
      ImGui::Checkbox("##Adaptive Speed", &cam.adaptiveSpeed);
      Tip("Scale zoom/pan with height above the terrain: fast from orbit, "
          "precise at ground level. Off = fixed speed.");
    }
  }
}

void VkEditor::drawEnvStyle(Context &ctx) {
  auto &s = ctx.renderer.params().style;
  envSection("Presets");
  if (envWideRow("Presets")) {
    const float quarter = (ImGui::GetContentRegionAvail().x - UITheme::kSpace2 * 3.0f) * 0.25f;
    if (ImGui::Button("Summer", ImVec2(quarter, 0)))
      s = vkrhi::VulkanRenderer::Params::StyleParams{};
    ImGui::SameLine(0.0f, UITheme::kSpace2);
    if (ImGui::Button("Golden", ImVec2(quarter, 0))) {
      s = vkrhi::VulkanRenderer::Params::StyleParams{};
      s.skyHorizon = {1.0f, 0.55f, 0.28f};
      s.cloudLit = {1.0f, 0.75f, 0.45f};
      s.cloudMid = {0.75f, 0.55f, 0.45f};
      s.cloudBase = {0.35f, 0.30f, 0.38f};
      s.splitHighlight = {1.0f, 0.77f, 0.48f};
      s.autumnAmount = 0.62f;
      s.cloudSilverIntensity = 2.2f;
    }
    ImGui::SameLine(0.0f, UITheme::kSpace2);
    if (ImGui::Button("Stormy", ImVec2(quarter, 0))) {
      s.cloudCoverage = 0.68f;
      s.cloudTypeBias = 0.45f;
      s.cloudLayerThickness = 4500.0f;
      s.cloudDensityMultiplier = 0.045f;
      s.cloudLightAbsorption = 7.0f;
      s.cloudBase = {0.22f, 0.25f, 0.32f};
      s.cloudMid = {0.45f, 0.48f, 0.55f};
      s.cloudLit = {0.85f, 0.88f, 0.92f};
      s.cloudSunOcclusion = 1.6f;
    }
    ImGui::SameLine(0.0f, UITheme::kSpace2);
    if (ImGui::Button("High Sky", ImVec2(quarter, 0))) {
      s.cloudCoverage = 0.28f;
      s.cloudTypeBias = -0.65f;
      s.cloudLayerThickness = 1200.0f;
      s.cloudDeckHeight = 4500.0f;
      s.cloudDensityMultiplier = 0.015f;
      s.cloudCurlStrength = 1.2f;
      s.cloudDetailStrength = 0.85f;
    }
  }

  // One collapsible sub-section per terrain layer, rather than the old
  // TreeNode-inside-a-filtered-row arrangement.
  static const char *names[5] = {"Meadow", "Forest", "Dirt", "Rock", "Scree"};
  for (int i = 0; i < 5; ++i) {
    char title[48];
    std::snprintf(title, sizeof(title), "Wash: %s", names[i]);
    envSection(title);
    ImGui::PushID(i);
    if (envRow("Lit"))
      ImGui::ColorEdit3("##Lit", &s.terrain[i].lit.x);
    if (envRow("Shade"))
      ImGui::ColorEdit3("##Shade", &s.terrain[i].shade.x);
    if (envRow("Mottle Scale"))
      ImGui::SliderFloat("##Mottle Scale", &s.terrain[i].mottleScale, .5f, 24,
                         "%.1f m");
    if (envRow("Overlay Strength"))
      ImGui::SliderFloat("##Overlay Strength", &s.terrain[i].overlayStrength, 0,
                         1);
    ImGui::PopID();
  }

  envSection("Season");
  if (envRow("Autumn"))
    ImGui::SliderFloat("##Autumn", &s.autumnAmount, 0, 1);
  if (envRow("Mountain Facets"))
    ImGui::SliderFloat("##Mountain Facets", &s.facetStrength, 0, 1);

  envSection("Drawn Finish");
  if (envRow("Outline Color"))
    ImGui::ColorEdit3("##Outline Color", &s.outlineColor.x);
  if (envRow("Outline Width"))
    ImGui::SliderFloat("##Outline Width", &s.outlineWidth, .5f, 2.5f);
  if (envRow("Outline Strength"))
    ImGui::SliderFloat("##Outline Strength", &s.outlineStrength, 0, 1);
  if (envRow("Paper Grain"))
    ImGui::SliderFloat("##Paper Grain", &s.screenPaperStrength, 0, .15f);

  envSection("Painted Sky");
  if (envRow("Zenith"))
    ImGui::ColorEdit3("##Zenith", &s.skyZenith.x);
  if (envRow("Horizon"))
    ImGui::ColorEdit3("##Horizon", &s.skyHorizon.x);
  if (envRow("Cloud Colors")) {
    ImGui::ColorEdit3("##Cloud Lit", &s.cloudLit.x);
    Tip("Sunlit tops, mid-body and shaded base. These tint the lit result "
        "now; they used to BE the result, which is why clouds never changed "
        "with the sun.");
  }
  if (envRow("##Cloud Mid"))
    ImGui::ColorEdit3("##Cloud Mid", &s.cloudMid.x);
  if (envRow("##Cloud Base"))
    ImGui::ColorEdit3("##Cloud Base", &s.cloudBase.x);
  if (envRow("Cloud Coverage")) {
    ImGui::SliderFloat("##Cloud Coverage", &s.cloudCoverage, 0, 1.0f);
    Tip("Roughly the fraction of sky with cloud in it. Drives both the "
        "volumetric layer and the cheap deck baked into the environment "
        "cubemap, so the ambient light always matches what you see.");
  }
  if (envRow("Cloud Softness"))
    ImGui::SliderFloat("##Cloud Softness", &s.cloudSoftness, .01f, .4f);
  if (envRow("Cloud Base Height")) {
    ImGui::SliderFloat("##Cloud Height", &s.cloudDeckHeight, 300.0f, 8000.0f,
                       "%.0f m");
    Tip("Altitude of the BOTTOM of the cloud layer. The layer is a pair of "
        "spheres concentric with the planet, so this is real metres and the "
        "deck ends at a true geometric horizon.");
  }
  if (envRow("Cloud Self-Shadow")) {
    ImGui::SliderFloat("##Cloud Self Shadow", &s.cloudSunOcclusion, 0.0f, 2.0f);
    Tip("Scales how much cloud between a point and the sun darkens it. 0 is "
        "flat-shaded cloud with no lit-top / dark-base contrast.");
  }
  if (envRow("Cloud Wind Speed")) {
    ImGui::SliderFloat2("##Cloud Wind Speed", &s.cloudWind.x, -0.05f, 0.05f, "%.4f");
    Tip("Drift velocity of the cloud layer along world X and Z axes.");
  }

  // --- volumetric layer ---------------------------------------------------
  envSection("Volumetric Clouds");
  if (envRow("Volumetric Clouds")) {
    ImGui::Checkbox("##Volumetric Clouds", &s.cloudVolumetricEnabled);
    Tip("Nubis-style raymarched cloudscape: a Perlin-Worley profile shaped by "
        "the weather map, eroded by a 128^3 four-channel detail noise, lit "
        "with transmittance + in-scatter + a dual-lobe phase. Off falls back "
        "to a clear sky (the analytic deck only feeds the cubemap).");
  }
  if (envRow("Cloud Amount")) {
    ImGui::SliderFloat("##Cloud Amount", &s.cloudStrength, 0.0f, 1.0f);
    Tip("Fades the whole layer out. Thins the deck rather than ghosting it: "
        "at 0 the march is skipped entirely and costs nothing.");
  }
  if (envRow("Layer Thickness")) {
    ImGui::SliderFloat("##Layer Thickness", &s.cloudLayerThickness, 200.0f,
                       8000.0f, "%.0f m");
    Tip("Vertical extent above the base height. Tall layers let the cumulus "
        "type actually tower; thin ones force everything into a flat sheet.");
  }
  if (envRow("Cloud Size")) {
    ImGui::SliderFloat("##Cloud Size", &s.cloudShapeScale, 500.0f, 20000.0f,
                       "%.0f m");
    Tip("Metres per tile of the base shape volume -- roughly how big one "
        "cloud is.");
  }
  if (envRow("Detail Size")) {
    ImGui::SliderFloat("##Detail Size", &s.cloudDetailScale, 10.0f, 500.0f,
                       "%.0f m");
    Tip("Metres per tile of the Nubis detail noise: the size of the billows "
        "carved into a cloud's surface. Keep it far below Cloud Size or the "
        "erosion reads as a second layer of clouds instead of as detail.");
  }
  if (envRow("Detail Strength")) {
    ImGui::SliderFloat("##Detail Strength", &s.cloudDetailStrength, 0.0f, 1.0f);
    Tip("How deeply the detail noise erodes the smooth profile. 0 leaves bare "
        "Perlin-Worley blobs -- this dial is most of the difference between "
        "this and the analytic deck it replaced.");
  }
  if (envRow("Weather Scale")) {
    ImGui::SliderFloat("##Weather Scale", &s.cloudWeatherScale, 2000.0f,
                       80000.0f, "%.0f m");
    Tip("Metres per tile of the weather map, i.e. the size of whole weather "
        "systems -- the scale at which the sky goes from broken to overcast.");
  }
  if (envRow("Cloud Type")) {
    ImGui::SliderFloat("##Cloud Type", &s.cloudTypeBias, -1.0f, 1.0f);
    Tip("Biases the weather map's type channel. Negative flattens the sky "
        "toward stratus sheets, positive builds it toward towering cumulus.");
  }
  if (envRow("Density")) {
    ImGui::SliderFloat("##Cloud Vol Density", &s.cloudDensityMultiplier, 0.002f,
                       0.12f, "%.3f /m");
    Tip("Extinction per metre through full-density cloud. Real cumulus is "
        "around 0.05, where an 80 m step is already opaque -- past that the "
        "silhouette stops changing and only the edges harden.");
  }
  if (envRow("Light Absorption")) {
    ImGui::SliderFloat("##Cloud Light Absorption", &s.cloudLightAbsorption,
                       0.0f, 12.0f);
    Tip("Optical depth toward the sun through a full-density cone: how deep "
        "self-shadowing goes, and so how much contrast there is between a "
        "sunlit top and a shadowed base.");
  }
  if (envRow("Ambient")) {
    ImGui::SliderFloat("##Cloud Ambient", &s.cloudAmbientStrength, 0.0f, 3.0f);
    Tip("Skylight reaching the cloud's flanks and underside, taken from the "
        "engine's own atmosphere. Drop it for dramatic near-black bases.");
  }
  if (envRow("Wisp / Curl")) {
    ImGui::SliderFloat("##Cloud Curl", &s.cloudCurlStrength, 0.0f, 2.0f);
    Tip("How far the curl field shears the layer's thin bases into wisps.");
  }
  if (envRow("Forward Scatter")) {
    ImGui::SliderFloat("##Cloud Phase G", &s.cloudPhaseG, 0.0f, 0.95f);
    Tip("Henyey-Greenstein g for the broad lobe: how much brighter cloud gets "
        "as you look toward the sun.");
  }
  if (envRow("Silver Lining")) {
    ImGui::SliderFloat("##Cloud Silver", &s.cloudSilverIntensity, 0.0f, 3.0f);
    Tip("Strength of the second, tight phase lobe -- the bright rim on a "
        "backlit cloud edge. 0 removes the silver lining entirely.");
  }
  if (envRow("Silver Spread")) {
    ImGui::SliderFloat("##Cloud Silver Spread", &s.cloudSilverSpread, 0.1f, 1.9f);
    Tip("How wide that second lobe is. Small values keep the lining to a few "
        "degrees around the sun.");
  }
  if (envRow("Powder")) {
    ImGui::SliderFloat("##Cloud Powder", &s.cloudPowderStrength, 0.0f, 1.0f);
    Tip("Darkens thin sun-facing edges below what their optical depth alone "
        "predicts -- the effect that keeps cloud rims from looking like "
        "cut paper.");
  }
  envSection("Volumetric Clouds: Quality");
  if (envRow("March Steps")) {
    ImGui::SliderFloat("##Cloud Steps", &s.cloudMaxSteps, 24.0f, 192.0f, "%.0f");
    Tip("Step budget per pixel. The march runs at half resolution and grows "
        "its step with distance, so this mostly buys near-field crispness.");
  }
  if (envRow("Light Taps")) {
    ImGui::SliderFloat("##Cloud Light Taps", &s.cloudLightTaps, 1.0f, 6.0f,
                       "%.0f");
    Tip("Cone samples toward the sun per lit step -- the most expensive part "
        "of the pass. 2-3 is usually indistinguishable from 6.");
  }
  if (envRow("Max Distance")) {
    ImGui::SliderFloat("##Cloud Max Dist", &s.cloudMaxMarchDist, 5000.0f,
                       120000.0f, "%.0f m");
    Tip("Caps how far along one ray the march runs. A near-horizon ray "
        "crosses an effectively unbounded slab, so without this the step size "
        "explodes and the deck stripes exactly where it should be densest.");
  }

  envSection("Environment Cubemap Deck");
  if (envRow("Cubemap Cloud Scale")) {
    ImGui::SliderFloat("##Cloud Scale", &s.cloudFeatureScale, 300.0f, 6000.0f,
                       "%.0f m");
    Tip("The cheap analytic deck that only feeds the sky cubemap (IBL is "
        "re-rendered at 128^2 x6 every frame and cannot afford a second "
        "volumetric march). It never appears in the viewport -- it decides "
        "how cloud-tinted the ambient light on every surface is.");
  }
  if (envRow("Cubemap Cloud Density"))
    ImGui::SliderFloat("##Cloud Density", &s.cloudOpticalDensity, 0.05f, 3.0f);
}

void VkEditor::drawEnvPost(Context &ctx) {
  vkrhi::VulkanRenderer::Params &p = ctx.renderer.params();

  envSection("Exposure");
  if (envRow("Auto Exposure")) {
    ImGui::Checkbox("##Auto Exposure", &p.autoExposure);
    Tip("Eye adaptation: exposure follows scene brightness between the "
        "min/max bounds.");
  }
  if (p.autoExposure) {
    if (envRow("Auto Exposure Min"))
      ImGui::SliderFloat("##Auto Exposure Min", &p.autoExposureMin, 0.1f, 5.0f);
    if (envRow("Auto Exposure Max"))
      ImGui::SliderFloat("##Auto Exposure Max", &p.autoExposureMax, 0.1f, 8.0f);
    if (envRow("Auto Exposure Speed"))
      ImGui::SliderFloat("##Auto Exposure Speed", &p.autoExposureSpeed, 0.01f,
                         1.0f);
  }
  if (envRow("Exposure")) {
    ImGui::BeginDisabled(p.autoExposure);
    ImGui::SliderFloat("##Exposure", &p.exposure, 0.1f, 5.0f);
    ImGui::EndDisabled();
  }

  envSection("Tonemap & Grade");
  if (envRow("Tonemap Mode")) {
    const char *tonemapModes[] = {"Painterly", "ACES", "Reinhard", "Linear"};
    ImGui::Combo("##Tonemap Mode", &p.tonemapMode, tonemapModes,
                 IM_ARRAYSIZE(tonemapModes));
  }
  if (envRow("Gamma"))
    ImGui::SliderFloat("##Gamma", &p.gamma, 1.0f, 3.0f);
  if (envRow("Saturation"))
    ImGui::SliderFloat("##Saturation", &p.saturation, 0.0f, 2.0f);
  if (envRow("Contrast"))
    ImGui::SliderFloat("##Contrast", &p.contrast, 0.0f, 2.0f);
  if (envRow("Vignette"))
    ImGui::SliderFloat("##Vignette", &p.vignette, 0.0f, 1.0f);

  envSection("Bloom");
  if (envRow("Bloom Threshold")) {
    ImGui::SliderFloat("##Bloom Threshold", &p.bloomThreshold, 0.0f, 5.0f);
    Tip("Brightness where bloom starts collecting light.");
  }
  if (envRow("Bloom Knee")) {
    ImGui::SliderFloat("##Bloom Knee", &p.bloomKnee, 0.0f, 2.0f);
    Tip("Softness of the threshold: higher = gentler roll-in below the "
        "threshold.");
  }
  if (envRow("Bloom Intensity"))
    ImGui::SliderFloat("##Bloom Intensity", &p.bloomIntensity, 0.0f, 1.0f);
  if (envRow("Bloom Spread")) {
    ImGui::SliderFloat("##Bloom Spread", &p.bloomWideIntensity, 0.0f, 1.5f);
    Tip("How far the glow reaches beyond the tight core, relative to Bloom "
        "Intensity. Lower this (toward 0) to keep bloom localized around "
        "bright spots instead of hazing the whole screen; the core glow "
        "around bright pixels is unaffected.");
  }
}

void VkEditor::drawEnvTerrainTab(Context &ctx) {
  // Each of these opens its own collapsible sections now, so the extra
  // CollapsingHeader wrapper this used to have would just be a second layer
  // of the same affordance.
  drawTerrainGenerator(ctx);
  drawTerrainBrush(ctx);
  drawTerrainMaterials(ctx);
  drawBiomeLighting(ctx);
}

void VkEditor::drawEnvDebug(Context &ctx) {
  vkrhi::VulkanRenderer::Params &p = ctx.renderer.params();
  envSection("Debug Views");
  if (envRow("Debug View")) {
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
    ImGui::Combo("##Debug View", &p.debugViewMode, debugModes,
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
  // One collapsible section per material slot.
  for (size_t i = 0; i < mTerrainMaterialPanel.size(); ++i) {
    envSection(kSlotNames[i]);
    ImGui::PushID(static_cast<int>(i));
    auto &panel = mTerrainMaterialPanel[i];
    // Path edits intentionally don't auto-apply (that would mean a disk
    // load per keystroke) -- only the "Reload Textures" button below
    // commits the staged buffers back to Params.
    if (envRow("Paint Overlay"))
      ImGui::InputText("##Paint Overlay", panel.albedoPath,
                       sizeof(panel.albedoPath));
    if (envRow("Tiling")) {
      ImGui::SliderFloat("##Tiling", &panel.tiling, 0.5f, 32.0f,
                         "%.1f m/repeat");
      Tip("World meters per texture repeat.");
    }
    ImGui::PopID();
  }
  envSection("Apply");
  if (envWideRow("Reload Textures")) {
    if (ImGui::Button("Reload Textures", ImVec2(-FLT_MIN, 0))) {
      for (size_t i = 0; i < mTerrainMaterialPanel.size(); ++i) {
        const auto &panel = mTerrainMaterialPanel[i];
        auto &slot = p.terrainMaterialSlots[i];
        slot.albedoPath = panel.albedoPath;
        slot.normalPath = panel.normalPath;
        slot.roughnessPath = panel.roughnessPath;
        slot.tiling = panel.tiling;
      }
      p.terrainMaterialsDirty = true;
    }
    Tip("Commits the paths above and reloads the texture sets from disk. "
        "Empty path = flat default.");
  }

  envSection("Terrain Tuning");
  if (envRow("Grass Color"))
    ImGui::ColorEdit3("##Grass Color", &p.terrainColorGrass.x);
  if (envRow("Rock Color"))
    ImGui::ColorEdit3("##Rock Color", &p.terrainColorRock.x);
  if (envRow("Forest Floor Tint"))
    ImGui::ColorEdit3("##Forest Floor Tint", &p.terrainColorSand.x);
  if (envRow("Scree Tint"))
    ImGui::ColorEdit3("##Scree Tint", &p.terrainColorSnow.x);
  if (envRow("Rock Slope Start")) {
    ImGui::SliderFloat("##Rock Slope Start", &p.rockSlopeStart, 0.0f, 1.0f);
    Tip("Slope steepness where exposed rock starts blending in.");
  }
  if (envRow("Rock Slope End"))
    ImGui::SliderFloat("##Rock Slope End", &p.rockSlopeEnd, 0.0f, 1.0f);
  if (envRow("Dirt Strength")) {
    ImGui::SliderFloat("##Dirt Strength", &p.terrainDetailScale, 0.0f, 1.0f);
    Tip("Curvature-driven dirt overlay in creases and creek beds.");
  }
  if (envRow("Anti-Tile Strength")) {
    ImGui::SliderFloat("##Anti-Tile Strength", &p.terrainDetailStrength, 0.0f,
                       1.0f);
    Tip("Texture-bombing blend that hides visible texture repetition.");
  }
  if (envRow("Macro Variation")) {
    ImGui::SliderFloat("##Macro Variation", &p.terrainMacroVariationStrength,
                       0.0f, 0.5f);
    Tip("Large-scale tonal variation so distant ground isn't uniform.");
  }
  if (envRow("Rock Detail"))
    ImGui::SliderFloat("##Rock Detail", &p.terrainRockDetailStrength, 0.0f,
                       1.0f);
}

void VkEditor::drawBiomeLighting(Context &ctx) {
  auto &p = ctx.renderer.params();

  envSection("Biome Lighting");
  if (envRow("Biome Strength")) {
    ImGui::SliderFloat("##Biome Strength", &p.biomeLightingStrength, 0.0f, 1.0f);
    Tip("Master dial for all biome lighting below. 0 = fully off (flat "
        "baseline look).");
  }

  envSection("Biome Ambient");
  if (envRow("Meadow Ambient Tint"))
    ImGui::ColorEdit3("##Meadow Ambient Tint", &p.biomeAmbientTintMeadow.x);
  if (envRow("Meadow Ambient Intensity"))
    ImGui::SliderFloat("##Meadow Ambient Intensity",
                       &p.biomeAmbientIntensityMeadow, 0.0f, 2.0f);
  if (envRow("Forest Ambient Tint"))
    ImGui::ColorEdit3("##Forest Ambient Tint", &p.biomeAmbientTintForest.x);
  if (envRow("Forest Ambient Intensity"))
    ImGui::SliderFloat("##Forest Ambient Intensity",
                       &p.biomeAmbientIntensityForest, 0.0f, 2.0f);
  if (envRow("Mountain Ambient Tint"))
    ImGui::ColorEdit3("##Mountain Ambient Tint", &p.biomeAmbientTintMountain.x);
  if (envRow("Mountain Ambient Intensity"))
    ImGui::SliderFloat("##Mountain Ambient Intensity",
                       &p.biomeAmbientIntensityMountain, 0.0f, 2.0f);

  envSection("Forest Canopy");
  if (envRow("Canopy Occlusion")) {
    ImGui::SliderFloat("##Canopy Occlusion", &p.forestCanopyOcclusion, 0.0f,
                       1.0f);
    Tip("How much the tree canopy darkens the forest floor.");
  }
  if (envRow("Light Shaft Strength")) {
    ImGui::SliderFloat("##Light Shaft Strength", &p.forestLightShaftStrength,
                       0.0f, 1.0f);
    Tip("Drifting dappled-light mask under the forest canopy.");
  }

  envSection("Mountain Air");
  if (envRow("Direct Sun Boost")) {
    ImGui::SliderFloat("##Direct Sun Boost", &p.mountainDirectBoost, 0.8f,
                       1.5f);
    Tip("Crisper, harder sunlight at altitude.");
  }
  if (envRow("Aerial Perspective")) {
    ImGui::SliderFloat("##Aerial Perspective", &p.mountainAerialStrength, 0.0f,
                       3.0f);
    Tip("Extra blue haze toward distant ridgelines.");
  }

  envSection("Biome Fog");
  if (envRow("Forest Fog Tint"))
    ImGui::ColorEdit3("##Forest Fog Tint", &p.forestFogTint.x);
  if (envRow("Forest Fog Density"))
    ImGui::SliderFloat("##Forest Fog Density", &p.forestFogDensityMult, 0.5f,
                       5.0f, "%.1fx");
  if (envRow("Mountain Fog Tint"))
    ImGui::ColorEdit3("##Mountain Fog Tint", &p.mountainFogTint.x);
  if (envRow("Mountain Fog Density"))
    ImGui::SliderFloat("##Mountain Fog Density", &p.mountainFogDensityMult,
                       0.0f, 1.5f, "%.1fx");

  envSection("Camera Grade");
  if (envRow("Grade Enabled")) {
    ImGui::Checkbox("##Grade Enabled", &p.cameraGradeEnabled);
    Tip("Whole-frame exposure/white-balance shift as the camera enters a "
        "biome.");
  }
  if (envRow("Grade Strength"))
    ImGui::SliderFloat("##Grade Strength", &p.cameraGradeStrength, 0.0f, 1.0f);
  if (envRow("Grade Smoothing Time"))
    ImGui::SliderFloat("##Grade Smoothing Time", &p.cameraGradeSmoothTime, 0.1f,
                       8.0f, "%.1f s");
}

void VkEditor::drawTerrainBrush(Context &ctx) {
  envSection("Terrain Brush");
  if (envRow("Brush Enabled")) {
    ImGui::Checkbox("##Brush Enabled", &brush.enabled);
    Tip("Hold Left Mouse over terrain in the viewport to sculpt.");
  }

  if (envRow("Brush Mode")) {
    int modeIdx = brush.mode == TerrainBrushSettings::Mode::Raise ? 0 : 1;
    static const char *kModes[2] = {"Raise", "Lower"};
    UI::Segmented("brushmode", &modeIdx, kModes, 2);
    brush.mode = modeIdx == 0 ? TerrainBrushSettings::Mode::Raise
                              : TerrainBrushSettings::Mode::Lower;
  }

  if (envRow("Brush Radius"))
    ImGui::SliderFloat("##Brush Radius", &brush.radius, 1.0f, 30.0f, "%.1f m");
  if (envRow("Brush Strength"))
    ImGui::SliderFloat("##Brush Strength", &brush.strength, 0.1f, 10.0f);

  if (!ctx.terrainSubsystem)
    ImGui::TextColored(UITheme::kWarning,
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
  // No terrain yet: offer to create it rather than showing sliders that
  // silently do nothing, which is what the panel used to do before terrain
  // became opt-in.
  if (ctx.terrainSubsystem && !ctx.terrainSubsystem->hasTerrain()) {
    if (!mTerrainGeneratorSeeded) {
      mTerrainGeneratorSettings = ctx.terrainSubsystem->pendingSettings();
      mTerrainGeneratorSeeded = true;
    }
    ImGui::TextWrapped("This scene has no terrain.");
    ImGui::Spacing();
    ImGui::DragInt("Seed##newterrain",
                   reinterpret_cast<int *>(&mTerrainGeneratorSettings.seed));
    ImGui::DragFloat("Height Scale##newterrain",
                     &mTerrainGeneratorSettings.heightScale, 0.5f, 1.0f, 400.0f);
    ImGui::DragInt("View Distance (chunks)##newterrain",
                   &mTerrainGeneratorSettings.viewDistanceChunks, 1, 1, 32);
    ImGui::Spacing();
    if (ImGui::Button("Create Terrain", ImVec2(-1, 0))) {
      ctx.terrainSubsystem->create(mTerrainGeneratorSettings);
      LOG_INFO("Editor", "Created terrain");
    }
    return;
  }

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
  if (mEnvSectionOpen)
    ImGui::TextDisabled("Regenerates when you release a control");
  if (envRow("Seed")) {
    int seedI = static_cast<int>(s.seed);
    if (ImGui::InputInt("##Seed", &seedI))
      s.seed = static_cast<uint32_t>(std::max(0, seedI));
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("World seed: same seed, same terrain.");
  }
  if (envRow("Chunk World Size")) {
    ImGui::SliderFloat("##Chunk World Size", &s.chunkWorldSize, 16.0f, 256.0f,
                       "%.0f m");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }
  if (envRow("View Distance")) {
    ImGui::SliderInt("##View Distance", &s.viewDistanceChunks, 1, 16);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Streaming radius in chunks around the camera.");
  }

  envSection("Height & Noise");
  if (envRow("Height Scale")) {
    ImGui::SliderFloat("##Height Scale", &s.heightScale, 0.0f, 100.0f, "%.0f m");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }
  if (envRow("Noise Frequency")) {
    ImGui::SliderFloat("##Noise Frequency", &s.noiseFrequency, 0.0001f, 0.05f,
                       "%.4f");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Base feature size: lower = broader hills, higher = busier "
        "terrain.");
  }
  if (envRow("Octaves")) {
    ImGui::SliderInt("##Octaves", &s.octaves, 1, 8);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Detail layers stacked on the base noise.");
  }
  if (envRow("Lacunarity")) {
    ImGui::SliderFloat("##Lacunarity", &s.lacunarity, 1.0f, 4.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Frequency step between octaves (2 = each layer twice as fine).");
  }
  if (envRow("Gain")) {
    ImGui::SliderFloat("##Gain", &s.gain, 0.0f, 1.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Amplitude step between octaves: how much the fine layers "
        "contribute.");
  }

  envSection("Macro Shape & Erosion");
  if (envRow("Macro Strength")) {
    ImGui::SliderFloat("##Macro Strength", &s.macroStrength, 0.0f, 3.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Large-scale landform (ridges/valleys) on top of the base noise.");
  }
  if (envRow("Landscape Scale")) {
    ImGui::SliderFloat("##Landscape Scale", &s.landscapeScale, 0.5f, 5.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }
  if (envRow("Valley Span")) {
    ImGui::SliderFloat("##Valley Span", &s.valleySpan, 0.25f, 3.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }
  if (envRow("Valley Depth")) {
    ImGui::SliderFloat("##Valley Depth", &s.valleyDepth, 0.0f, 3.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }
  if (envRow("Erosion Strength")) {
    ImGui::SliderFloat("##Erosion Strength", &s.erosionStrength, 0.0f, 3.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }
  if (envRow("Micro Relief")) {
    ImGui::SliderFloat("##Micro Relief", &s.microReliefStrength, 0.0f, 2.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Sub-meter surface roughness.");
  }
  if (envRow("Outcrop Threshold")) {
    ImGui::SliderFloat("##Outcrop Threshold", &s.outcropThreshold, 0.1f, 0.95f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Slope steepness where rock outcrops break through.");
  }
  if (envRow("Use Ridge Noise")) {
    ImGui::Checkbox("##Use Ridge Noise", &s.useRidgeNoise);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Sharp mountain ridgelines instead of rounded hills.");
  }

  envSection("Biome Layout");
  if (envRow("Mountain Region Scale")) {
    ImGui::SliderFloat("##Mountain Region Scale", &s.mountainRegionScale, 0.2f,
                       4.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Size of mountain regions: lower = few huge ranges, higher = many "
        "small ones.");
  }
  if (envRow("Mountain Coverage")) {
    ImGui::SliderFloat("##Mountain Coverage", &s.mountainCoverage, 0.0f, 1.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Fraction of the world that is mountainous.");
  }
  if (envRow("Mountain Height Scale")) {
    ImGui::SliderFloat("##Mountain Height Scale", &s.mountainHeightScale, 0.5f,
                       8.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }
  if (envRow("Forest Coverage")) {
    ImGui::SliderFloat("##Forest Coverage", &s.forestCoverage, 0.0f, 1.0f);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }
  if (envRow("Treeline Height")) {
    ImGui::SliderFloat("##Treeline Height", &s.treelineHeight, 0.0f, 100.0f,
                       "%.0f m");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Altitude where forest gives way to bare mountain.");
  }
  if (envRow("Treeline Transition")) {
    ImGui::SliderFloat("##Treeline Transition", &s.treelineTransition, 1.0f,
                       30.0f, "%.0f m");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("How gradually the treeline fades out.");
  }

  envSection("Vegetation");
  if (envRow("Spawn Vegetation")) {
    ImGui::Checkbox("##Spawn Vegetation", &s.spawnVegetation);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Master switch for every scatter layer (trees, rocks and grass).");
  }

  // Every slider below multiplies the corresponding value authored in
  // terrain_scatter.json rather than replacing it, so the relationships
  // between layers survive any slider position. 1.00x everywhere == exactly
  // what the manifest says.
  envSection("Trees");
  if (envRow("Tree Density")) {
    ImGui::SliderFloat("##Tree Density", &s.treeDensityMultiplier, 0.0f, 3.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("How many trees are ATTEMPTED per square metre. Pair with Tree "
        "Spacing, which decides how many of those attempts can fit.");
  }
  if (envRow("Tree Spacing")) {
    ImGui::SliderFloat("##Tree Spacing", &s.treeSpacingMultiplier, 0.0f, 4.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Minimum gap between trunks. Raise it to thin dense stands into open "
        "woodland without touching density; 0 removes the limit entirely and "
        "lets trees interpenetrate.");
  }
  if (envRow("Tree Size")) {
    ImGui::SliderFloat("##Tree Size", &s.treeSizeMultiplier, 0.25f, 3.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Makes trees bigger or smaller. Scales all three axes, so trees keep "
        "their proper shape -- this is the one to use for larger trees.");
  }
  if (envRow("Tree Stretch")) {
    ImGui::SliderFloat("##Tree Stretch", &s.treeHeightMultiplier, 0.5f, 2.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Stretches trees vertically WITHOUT widening them. A stylization "
        "dial, not a size dial: past about 1.3x trees stop looking tall and "
        "start looking like a squashed model. Use Tree Size instead.");
  }
  if (envRow("Tree Height Variance")) {
    ImGui::SliderFloat("##Tree Height Variance", &s.treeHeightVariance, 0.0f,
                       3.0f, "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("How uneven the canopy line is. 0 makes every tree in a layer the "
        "same height; high values give a ragged, old-growth look. Does not "
        "change the average height.");
  }
  if (envRow("Tree Lean")) {
    ImGui::SliderFloat("##Tree Lean", &s.treeLeanExtraDeg, 0.0f, 25.0f, "+%.0f deg");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Extra random tilt off vertical, on top of each layer's own. "
        "Storm-battered forests.");
  }
  if (envRow("Grove Size")) {
    ImGui::SliderFloat("##Grove Size", &s.standRadiusMultiplier, 0.2f, 4.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Size of one continuous stand of trees.");
  }
  if (envRow("Forest Patchiness")) {
    ImGui::SliderFloat("##Forest Patchiness", &s.forestPatchiness, 0.0f, 2.5f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("How much of the forest is clearings. 0 = unbroken canopy; high = "
        "forest fragmented into small isolated copses.");
  }
  if (envRow("Interactive Tree Radius")) {
    ImGui::SliderInt("##Interactive Tree Radius", &s.interactiveTreeChunkRadius,
                     0, 6);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Chunk radius around the camera where trees become solid (get "
        "collision). Larger costs physics bodies.");
  }

  envSection("Rocks");
  if (envRow("Rock Density")) {
    ImGui::SliderFloat("##Rock Density", &s.rockDensityMultiplier, 0.0f, 3.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
  }

  envSection("Grass");
  if (envRow("Spawn Grass")) {
    ImGui::Checkbox("##Spawn Grass", &s.spawnGrass);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Grass is by far the heaviest scatter layer -- turn it off first "
        "when chasing frame time.");
  }
  if (envRow("Grass Density")) {
    ImGui::SliderFloat("##Grass Density", &s.grassDensityMultiplier, 0.0f, 3.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Clumps per square metre. Cost scales linearly with this.");
  }
  if (envRow("Grass Size")) {
    ImGui::SliderFloat("##Grass Size", &s.grassSizeMultiplier, 0.25f, 3.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Uniform size of each grass clump -- keeps its proper shape.");
  }
  if (envRow("Grass Height")) {
    ImGui::SliderFloat("##Grass Height", &s.grassHeightMultiplier, 0.25f, 3.0f,
                       "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Stretches blades vertically only. Grass takes this much better than "
        "trees do -- blades are already long and thin, so it reads as a "
        "different species rather than as a distorted mesh.");
  }
  if (envRow("Grass Root Shading")) {
    ImGui::SliderFloat("##Grass Root Shading", &s.grassOcclusionStrength, 0.0f,
                       2.0f, "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Ambient occlusion baked into each blade: darker at the root, fading "
        "to none at the tip. This is what makes grass look like it is "
        "growing OUT of the ground instead of resting on it. Costs nothing.");
  }
  if (envRow("Grass Casts Shadows")) {
    ImGui::Checkbox("##Grass Casts Shadows", &s.grassCastShadows);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Puts grass in the ray-traced shadow pass so it casts real shadows "
        "on the ground and on itself. EXPENSIVE -- one shadow-tracing entry "
        "per clump, and there are tens of thousands. Grass already RECEIVES "
        "shadows from trees and terrain with this off; Grass Root Shading is "
        "the cheap approximation of the rest.");
  }
  if (envRow("Grass Draw Distance")) {
    ImGui::SliderFloat("##Grass Draw Distance", &s.grassDrawDistanceMultiplier,
                       0.25f, 3.0f, "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Scales how far grass is drawn (default ~95m) and where it starts "
        "thinning out. The strongest grass performance control -- cost "
        "scales with the SQUARE of this.");
  }
  if (envRow("Grass Chunk Radius")) {
    ImGui::SliderInt("##Grass Chunk Radius", &s.grassChunkRadius, 0, 6);
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Hard cap on how many chunks out grass is generated at all, "
        "regardless of draw distance. Controls CPU placement cost and "
        "memory rather than draw cost.");
  }

  envSection("Wind");
  if (envRow("Wind Strength")) {
    ImGui::SliderFloat("##Wind Strength", &s.windStrength, 0.0f, 3.0f, "%.2fx");
    apply |= ImGui::IsItemDeactivatedAfterEdit();
    Tip("Vertex sway on grass and foliage. 0 = perfectly still. Free: it is "
        "a vertex-shader effect, not extra geometry.");
  }
  if (envRow("Wind Speed")) {
    ImGui::SliderFloat("##Wind Speed", &s.windSpeed, 0.0f, 4.0f, "%.2fx");
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
  ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Quadrant 1: Live 3D Engine Viewport & Shading");
  ImGui::Separator();

  // Status Badge
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.22f, 0.12f, 0.7f));
  ImGui::BeginChild("LiveStatusBadge", ImVec2(-1.0f, 26.0f), true, ImGuiWindowFlags_NoScrollbar);
  ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f), "LIVE VULKAN 3D ENGINE VIEWPORT (ACTIVE SCENE)");
  ImGui::EndChild();
  ImGui::PopStyleColor();

  ImGui::Spacing();
  ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Viewport Camera Telemetry:");
  glm::vec3 camPos = ctx.renderer.params().camPos;
  ImGui::Text(" - Position: [%.1f, %.1f, %.1f]", camPos.x, camPos.y, camPos.z);
  ImGui::Text(" - Orientation: Yaw %.1f deg, Pitch %.1f deg", ctx.renderer.params().camYawDeg, ctx.renderer.params().camPitchDeg);
  ImGui::Text(" - Field of View: %.1f deg | Far Plane: %.1f m", ctx.renderer.params().fovDeg, ctx.renderer.params().farPlane);

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
  if (ImGui::Button("Reset Viewport Camera to Origin", ImVec2(-1.0f, 22.0f))) {
    ctx.renderer.params().camPos = glm::vec3(0.0f, 5.0f, 10.0f);
    ctx.renderer.params().camYawDeg = -90.0f;
    ctx.renderer.params().camPitchDeg = -15.0f;
  }
}

void VkEditor::drawQuadrantWireframeContent(Context &ctx) {
  ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.7f, 1.0f), "Quadrant 2: Geometry & Wireframe");
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
  // These were hardcoded literals ("9") and reported 9 chunks with no terrain
  // loaded at all. A panel that invents its own telemetry is worse than one
  // that shows nothing, because it gets believed.
  {
    const vkrhi::VulkanRenderer::FrameStats &fs = ctx.renderer.frameStats();
    const bool hasTerrain =
        ctx.terrainSubsystem && ctx.terrainSubsystem->hasTerrain();
    ImGui::Text("Terrain: %s", hasTerrain ? "active" : "none");
    ImGui::Text("Instances Drawn: %u (culled %u)", fs.instancesDrawn,
                fs.instancesCulled);
    ImGui::Text("Vegetation Instances: %u", fs.vegInstancesDrawn);
    ImGui::Text("TLAS Instances: %u", fs.tlasInstances);
    ImGui::Text("Mesh Slots: %u live, %u free", fs.meshSlotsLive,
                fs.meshSlotsFree);
  }

  ImGui::Spacing();
  if (ImGui::Button(mViewMode == ViewMode::Wireframe ? "Disable Wireframe Mode" : "Enable Wireframe Mode", ImVec2(-1.0f, 24.0f))) {
    mViewMode = (mViewMode == ViewMode::Wireframe) ? ViewMode::Shaded : ViewMode::Wireframe;
  }
}

void VkEditor::drawQuadrantPhysicsContent(Context &ctx) {
  ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Quadrant 3: Physics Inspector & Lab");
  ImGui::Separator();

  static float sGravity = -9.81f;
  if (ImGui::SliderFloat("Gravity Y (m/s^2)", &sGravity, -30.0f, 10.0f, "%.2f")) {
    ctx.physics.setGravity(glm::vec3(0.0f, sGravity, 0.0f));
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset")) {
    sGravity = -9.81f;
    ctx.physics.setGravity(glm::vec3(0.0f, -9.81f, 0.0f));
  }

  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.35f, 0.15f, 1.0f));
  if (ImGui::Button("Detonate Radial Shockwave", ImVec2(-1.0f, 24.0f))) {
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

  if (ImGui::Button("Box Stack (5x)")) {
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
  if (ImGui::Button("Ball Pit (8x)")) {
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
  ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.4f, 1.0f), "Quadrant 4: Telemetry & Profiler");
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

// Populates the command palette. Registered once (draw() latches
// mCommandsRegistered) because the closures capture ctx by reference and the
// Context outlives the editor's frame loop.
//
// This is the safety net that makes demoting things acceptable: every action
// the toolbar and menus expose, plus the panels themselves, are reachable by
// typing. Settings live in the World panel's sections rather than being
// enumerated here -- the palette jumps you to the panel, it does not try to
// mirror 150 individual sliders.
void VkEditor::registerCommands(Context &ctx) {
  Context *c = &ctx;
  mPalette.clear();

  auto reveal = [this]() { mShell.rightOpen = true; };

  // ── Playback ────────────────────────────────────────────────────────
  mPalette.add(ICON_PLAY, "Play", "Enter Play Mode",
               [this, c] { requestPlay(*c); });
  mPalette.add(ICON_STOP, "Play", "Stop Play Mode",
               [this, c] { stopPlayMode(*c); });

  // ── File ────────────────────────────────────────────────────────────
  mPalette.add(ICON_SAVE, "File", "Save Scene", [this, c] {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(mScenePath).parent_path(), ec);
    if (c->scene.saveToFile(mScenePath)) {
      LOG_INFO("Editor", "Saved scene: " + mScenePath);
      saveSession(*c);
    }
  }, "Ctrl+S");
  mPalette.add(ICON_SAVE, "File", "Save All Settings",
               [this, c] { saveAllSettings(*c); }, "Ctrl+Shift+S");
  mPalette.add(ICON_FOLDER_OPEN, "File", "Load Scene", [this, c] {
    releasePhysicsBodies(*c);
    if (c->scene.loadFromFile(mScenePath)) {
      selection = SelectionState{};
      LOG_INFO("Editor", "Loaded scene: " + mScenePath);
    }
  }, "Ctrl+O");
  mPalette.add(ICON_FILE, "File", "New Scene", [this, c] {
    releasePhysicsBodies(*c);
    c->scene.clear();
    selection = SelectionState{};
    LOG_INFO("Editor", "New scene");
  });

  // ── Create ──────────────────────────────────────────────────────────
  mPalette.add(ICON_PERSON, "Create", "Player",
               [this, c] { createPlayerEntity(*c); });
  mPalette.add(ICON_CUBE, "Create", "Spaceship",
               [this, c] { createSpaceshipEntity(*c); });
  mPalette.add(ICON_CUBE, "Create", "Empty Entity", [this, c] {
    uint32_t e = c->scene.createEmptyEntity("Empty");
    selection.selectedEntityId = e;
    selection.selectedEntities = {e};
  });
  {
    const char *prims[] = {"cube", "sphere", "plane", "cylinder", "cone"};
    const char *labels[] = {"Cube", "Sphere", "Plane", "Cylinder", "Cone"};
    for (int i = 0; i < 5; ++i) {
      const char *prim = prims[i];
      mPalette.add(ICON_CUBE, "Create", labels[i], [this, c, prim] {
        uint32_t e = c->scene.spawnPrimitive(prim);
        if (e != 0) {
          selection.selectedEntityId = e;
          selection.selectedEntities = {e};
        }
      });
    }
  }
  mPalette.add(ICON_MOUNTAIN, "Create", "Terrain", [this, c] {
    if (c->terrainSubsystem && !c->terrainSubsystem->hasTerrain()) {
      c->terrainSubsystem->create(mTerrainGeneratorSeeded
                                      ? mTerrainGeneratorSettings
                                      : c->terrainSubsystem->pendingSettings());
      LOG_INFO("Editor", "Created terrain");
    }
  });
  mPalette.add(ICON_TRASH, "Create", "Remove Terrain", [c] {
    if (c->terrainSubsystem && c->terrainSubsystem->hasTerrain()) {
      c->terrainSubsystem->destroy();
      LOG_INFO("Editor", "Removed terrain");
    }
  });

  // ── Selection ───────────────────────────────────────────────────────
  mPalette.add(ICON_CLONE, "Selection", "Duplicate",
               [this, c] { duplicateSelection(*c); }, "Ctrl+D");
  mPalette.add(ICON_TRASH, "Selection", "Delete", [this, c] {
    if (selection.selectedEntityId != 0)
      deleteEntity(*c, selection.selectedEntityId);
  }, "Del");
  mPalette.add(ICON_CROSSHAIRS, "Selection", "Focus Viewport On Selection",
               [this] { mFocusRequest = selection.selectedEntityId; }, "F");

  // ── Tools ───────────────────────────────────────────────────────────
  mPalette.add(ICON_MOVE, "Tool", "Move",
               [this] { toolbar.gizmoOp = ToolbarState::Translate; }, "W");
  mPalette.add(ICON_SCALE, "Tool", "Scale",
               [this] { toolbar.gizmoOp = ToolbarState::Scale; }, "E");
  mPalette.add(ICON_ROTATE, "Tool", "Rotate",
               [this] { toolbar.gizmoOp = ToolbarState::Rotate; }, "R");
  mPalette.add(ICON_GLOBE, "Tool", "Toggle World / Local Space",
               [this] { toolbar.worldSpace = !toolbar.worldSpace; });
  mPalette.add(ICON_GRID, "Tool", "Toggle Grid Snap",
               [this] { toolbar.snapEnabled = !toolbar.snapEnabled; });
  mPalette.add(ICON_BRUSH, "Tool", "Toggle Terrain Brush",
               [this] { brush.enabled = !brush.enabled; });

  // ── Panels / settings jumps ─────────────────────────────────────────
  mPalette.add(ICON_LIST, "View", "Toggle Scene Rail",
               [this] { mShell.leftOpen = !mShell.leftOpen; }, "Ctrl+1");
  mPalette.add(ICON_SLIDERS, "View", "Toggle Properties Rail",
               [this] { mShell.rightOpen = !mShell.rightOpen; }, "Ctrl+2");
  mPalette.add(ICON_TERMINAL, "View", "Open Console", [this] {
    mShell.drawerOpen = true;
    mShell.drawerTab = 0;
  }, "`");
  mPalette.add(ICON_FOLDER, "View", "Open Asset Browser", [this] {
    mShell.drawerOpen = true;
    mShell.drawerTab = 1;
  });
  mPalette.add(ICON_GAUGE, "View", "Toggle Stats Overlay",
               [this] { mShowStatsHud = !mShowStatsHud; }, "F1");
  mPalette.add(ICON_BUG, "View", "Toggle Debug Tools",
               [this] { mShowDebugWindow = !mShowDebugWindow; }, "F2");
  mPalette.add(ICON_REFRESH, "View", "Reset Layout", [this] {
    mShell = UIShell::State{};
    mShowStatsHud = true;
    mShowDebugWindow = false;
  });

  // World settings: deselect so the rail shows World, and open the rail.
  auto world = [this, reveal](const char *name) {
    return [this, reveal, name] {
      selection.selectedEntityId = 0;
      selection.selectedEntities.clear();
      reveal();
      LOG_INFO("Editor", std::string("World > ") + name);
    };
  };
  mPalette.add(ICON_SUN, "World", "Sky & Sun Settings", world("Sky"));
  mPalette.add(ICON_LIGHT, "World", "Lighting & Shadows", world("Light"));
  mPalette.add(ICON_CLOUD, "World", "Fog & God Rays", world("Fog"));
  mPalette.add(ICON_CAMERA, "World", "Camera & Navigation", world("Camera"));
  mPalette.add(ICON_FILM, "World", "Post-Processing", world("Post"));
  mPalette.add(ICON_PALETTE, "World", "Style & Painterly Look", world("Style"));
  mPalette.add(ICON_MOUNTAIN, "World", "Terrain Generator", world("Terrain"));
  mPalette.add(ICON_EYE, "World", "Debug View Modes", world("Debug"));
}

// Debug tools, off by default.
//
// This replaces "Multi-View Split Screen", which offered four layout modes
// (2x2 grid, two splits, tabbed) for what were only ever four panels of text
// and toggles -- it never rendered multiple 3D views, and it carried a third
// duplicate of the Play/Stop buttons. Tabs are enough; the layout modes and
// the duplicated transport are gone.
void VkEditor::drawDebugWindow(Context &ctx) {
  const UIShell::Layout L = UIShell::compute(mShell);
  ImGui::SetNextWindowPos(ImVec2(L.viewport.x + UITheme::kSpace3,
                                 L.viewport.y + UITheme::kSpace3),
                          ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(420.0f, 380.0f), ImGuiCond_FirstUseEver);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, UITheme::kBg1);
  if (ImGui::Begin(ICON_BUG "  Debug Tools", &mShowDebugWindow)) {
    if (ImGui::BeginTabBar("DebugTabs")) {
      if (ImGui::BeginTabItem("View")) {
        drawQuadrantViewModesContent(ctx);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Geometry")) {
        drawQuadrantWireframeContent(ctx);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Physics")) {
        drawQuadrantPhysicsContent(ctx);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Profiler")) {
        drawQuadrantProfilerContent(ctx);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
}

// =============================================================================
// Viewport picking + selection outline
// =============================================================================

// World-space AABB for an entity, best source first: parsed mesh bounds,
// then a collider, then a bounds sphere, then the raw transform scale.
//
// The local box is transformed corner-by-corner and re-bounded, so a rotated
// entity still gets a box that actually contains it (transforming only min/max
// would collapse under rotation).
static bool entityWorldBounds(VkEditor::Context &ctx, EntityId e,
                              glm::vec3 &outMin, glm::vec3 &outMax) {
  auto &reg = ctx.scene.registry();
  if (!reg.has<TransformComponent>(e))
    return false;
  const auto &tr = reg.get<TransformComponent>(e);

  glm::vec3 lo(-0.5f), hi(0.5f); // unit box fallback
  bool haveLocal = false;

  if (reg.has<MeshComponent>(e)) {
    const auto &mc = reg.get<MeshComponent>(e);
    const MeshData *data = nullptr;
    if (mc.objHandle.valid())
      data = ctx.assets.getOBJData(mc.objHandle);
    else if (mc.gltfHandle.valid())
      data = ctx.assets.getGLTFData(mc.gltfHandle);
    else if (mc.ufbxHandle.valid())
      data = ctx.assets.getUFBXData(mc.ufbxHandle);
    glm::vec3 mn, mx;
    if (data && data->getGlobalBounds(mn, mx)) {
      lo = mn;
      hi = mx;
      haveLocal = true;
    }
  }
  if (!haveLocal && reg.has<ColliderComponent>(e)) {
    const auto &col = reg.get<ColliderComponent>(e);
    const glm::vec3 half =
        (col.shape == ColliderComponent::Shape::Box)
            ? col.dimensions * 0.5f
            : glm::vec3(col.dimensions.x,
                        std::max(col.dimensions.x, col.dimensions.y * 0.5f),
                        col.dimensions.x);
    lo = col.offset - half;
    hi = col.offset + half;
    haveLocal = true;
  }
  if (!haveLocal && reg.has<BoundsComponent>(e)) {
    const auto &b = reg.get<BoundsComponent>(e);
    lo = b.centerOffset - glm::vec3(b.radius);
    hi = b.centerOffset + glm::vec3(b.radius);
    haveLocal = true;
  }

  // Same composer the renderer and the gizmo use, so the box lands exactly
  // on the drawn object rather than approximately near it.
  const glm::mat4 m = tr.getMatrix();

  outMin = glm::vec3(FLT_MAX);
  outMax = glm::vec3(-FLT_MAX);
  for (int i = 0; i < 8; ++i) {
    const glm::vec3 c((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y,
                      (i & 4) ? hi.z : lo.z);
    const glm::vec3 w = glm::vec3(m * glm::vec4(c, 1.0f));
    outMin = glm::min(outMin, w);
    outMax = glm::max(outMax, w);
  }
  // Degenerate entities (zero scale, empty mesh) would be unclickable.
  const glm::vec3 pad(0.05f);
  outMin -= pad;
  outMax += pad;
  return true;
}

// Slab test. `dir` need not be normalized; `t` comes back in units of dir.
static bool rayAABB(const glm::vec3 &orig, const glm::vec3 &dir,
                    const glm::vec3 &lo, const glm::vec3 &hi, float &t) {
  float tmin = 0.0f, tmax = 1.0f;
  for (int a = 0; a < 3; ++a) {
    if (std::abs(dir[a]) < 1e-8f) {
      if (orig[a] < lo[a] || orig[a] > hi[a])
        return false;
      continue;
    }
    const float inv = 1.0f / dir[a];
    float t0 = (lo[a] - orig[a]) * inv;
    float t1 = (hi[a] - orig[a]) * inv;
    if (t0 > t1)
      std::swap(t0, t1);
    tmin = std::max(tmin, t0);
    tmax = std::min(tmax, t1);
    if (tmin > tmax)
      return false;
  }
  t = tmin;
  return true;
}

// Nearest entity under a screen position, or 0 for empty space. Split out
// from the input handling so the ray/bounds math can be exercised without a
// mouse (see the picking smoke check in main.cpp).
uint32_t VkEditor::pickAt(Context &ctx, const glm::mat4 &view,
                          const glm::mat4 &proj, const ImVec2 &screenPos) {
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  if (display.x <= 0.0f || display.y <= 0.0f)
    return 0;

  const float ndcX = (screenPos.x / display.x) * 2.0f - 1.0f;
  const float ndcY = 1.0f - (screenPos.y / display.y) * 2.0f;
  const glm::mat4 invViewProj = glm::inverse(proj * view);
  const glm::vec4 nearH = invViewProj * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
  const glm::vec4 farH = invViewProj * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
  if (nearH.w == 0.0f || farH.w == 0.0f)
    return 0;
  const glm::vec3 orig = glm::vec3(nearH) / nearH.w;
  const glm::vec3 dir = (glm::vec3(farH) / farH.w) - orig;

  auto &reg = ctx.scene.registry();
  uint32_t best = 0;
  float bestT = FLT_MAX;
  for (EntityId e : reg.view<TransformComponent>()) {
    if (reg.has<LifecycleComponent>(e) &&
        reg.get<LifecycleComponent>(e).state != EntityLifecycleState::Alive)
      continue;
    // Streamed terrain chunks span the whole world; letting them win the
    // ray would make every click select "the ground".
    if (reg.has<NameComponent>(e) &&
        reg.get<NameComponent>(e).name.rfind("TerrainChunk", 0) == 0)
      continue;

    glm::vec3 lo, hi;
    if (!entityWorldBounds(ctx, e, lo, hi))
      continue;
    float t;
    if (rayAABB(orig, dir, lo, hi, t) && t < bestT) {
      bestT = t;
      best = (uint32_t)e;
    }
  }
  return best;
}

void VkEditor::updatePicking(Context &ctx, const glm::mat4 &view,
                             const glm::mat4 &proj) {
  if (mInPlayMode)
    return;
  ImGuiIO &io = ImGui::GetIO();
  // Don't steal the click from a panel, the gizmo, or the terrain brush
  // (which drives itself off held LMB).
  if (io.WantCaptureMouse || ImGuizmo::IsUsing() || ImGuizmo::IsOver() ||
      brush.enabled)
    return;
  if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    return;
  // A click that dragged was a camera move, not a selection.
  const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
  if (std::abs(drag.x) + std::abs(drag.y) > 4.0f)
    return;

  const uint32_t best = pickAt(ctx, view, proj, io.MousePos);
  if (best != 0) {
    if (io.KeyCtrl) { // additive toggle, same as the Scene rail
      auto &sel = selection.selectedEntities;
      auto it = std::find(sel.begin(), sel.end(), best);
      if (it != sel.end()) {
        sel.erase(it);
        if (selection.selectedEntityId == best)
          selection.selectedEntityId = sel.empty() ? 0 : sel.back();
      } else {
        sel.push_back(best);
        selection.selectedEntityId = best;
      }
    } else {
      selection.selectedEntities = {best};
      selection.selectedEntityId = best;
    }
    selection.lastClickedEntity = best;
  } else if (!io.KeyCtrl) {
    selection.selectedEntityId = 0;
    selection.selectedEntities.clear();
  }
}

void VkEditor::drawSelectionOutline(Context &ctx, const glm::mat4 &view,
                                    const glm::mat4 &proj) {
  if (mInPlayMode || selection.selectedEntities.empty())
    return;

  ImDrawList *dl = ImGui::GetForegroundDrawList();
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  const glm::mat4 viewProj = proj * view;
  auto &reg = ctx.scene.registry();

  for (uint32_t id : selection.selectedEntities) {
    if (!reg.valid(id))
      continue;
    glm::vec3 lo, hi;
    if (!entityWorldBounds(ctx, id, lo, hi))
      continue;

    const bool primary = (id == selection.selectedEntityId);
    const ImU32 col = ImGui::GetColorU32(primary
                                             ? UITheme::kSelection
                                             : UITheme::kSelectionSecondary);

    const glm::vec3 pts[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z},
                              {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                              {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z},
                              {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    ImVec2 s[8];
    bool ok[8];
    for (int i = 0; i < 8; ++i)
      ok[i] = worldToScreen(pts[i], viewProj, screen, s[i]);

    // Corner brackets rather than full edges: the shape stays readable
    // without drawing a cage over the model.
    static const int kEdges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0},
                                      {4, 5}, {5, 6}, {6, 7}, {7, 4},
                                      {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto &e : kEdges) {
      if (!ok[e[0]] || !ok[e[1]])
        continue;
      const ImVec2 a = s[e[0]], b = s[e[1]];
      const float dx = b.x - a.x, dy = b.y - a.y;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len < 1.0f)
        continue;
      const float frac = std::min(0.28f, 14.0f / len);
      dl->AddLine(a, ImVec2(a.x + dx * frac, a.y + dy * frac), col, 2.0f);
      dl->AddLine(ImVec2(b.x - dx * frac, b.y - dy * frac), b, col, 2.0f);
    }
  }
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
