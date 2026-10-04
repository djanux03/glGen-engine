#include "VkScriptBindings.h"

#include "GameHud.h"
#include "gameplay/PlayerArchetype.h"
#include "VkAppState.h"
#include "VulkanRenderer.h"
#include "subsystems/VkTerrainSubsystem.h"
#include "Terrain/TerrainWater.h"
#include "Terrain/TerrainNoise.h"
#include "Terrain/TerrainIslands.h"

#include <stb_image_write.h>
#include <vector>

#include "Logger.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include "Scripting/ScriptJson.h"

#include <algorithm>
#include <string>

namespace {

// Reads a {r,g,b} Lua table into `dst`, leaving it alone if absent. Colours
// are 1-based triples everywhere in this API, matching the recipe JSON.
void readColor(const sol::table &t, const char *key, glm::vec3 &dst) {
  sol::optional<sol::table> c = t[key];
  if (c && c->size() >= 3)
    dst = glm::vec3((*c)[1].get_or(dst.x), (*c)[2].get_or(dst.y),
                    (*c)[3].get_or(dst.z));
}

// --- JSON param mapping (shared by the Lua binding and the command port) ---

template <typename T>
void readField(const nlohmann::json &j, const char *key, T &dst) {
  const auto it = j.find(key);
  if (it != j.end() && !it->is_null()) {
    try {
      dst = it->get<T>();
    } catch (const std::exception &) {
      // Wrong type for this field: leave it alone rather than abort the whole
      // update. The caller still gets every other field applied.
    }
  }
}

void readVec3Field(const nlohmann::json &j, const char *key, glm::vec3 &dst) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_array() || it->size() < 3)
    return;
  for (int i = 0; i < 3; ++i)
    if (!(*it)[i].is_number())
      return;
  dst = glm::vec3((*it)[0].get<float>(), (*it)[1].get<float>(),
                  (*it)[2].get<float>());
}

} // namespace

void applyRenderParamsJson(VkAppState &state, const nlohmann::json &params) {
  if (!state.renderer || !params.is_object())
    return;
  vkrhi::VulkanRenderer::Params &p = state.renderer->params();
  const bool writesCamera = params.contains("camPos") ||
                            params.contains("camYaw") ||
                            params.contains("camPitch");

  readField(params, "exposure", p.exposure);
  readField(params, "gamma", p.gamma);
  readField(params, "saturation", p.saturation);
  readField(params, "contrast", p.contrast);
  readField(params, "vignette", p.vignette);
  readField(params, "autoExposure", p.autoExposure);
  readField(params, "autoExposureSpeed", p.autoExposureSpeed);
  readField(params, "autoExposureMin", p.autoExposureMin);
  readField(params, "autoExposureMax", p.autoExposureMax);
  readField(params, "tonemapMode", p.tonemapMode);
  readField(params, "temporalAA", p.temporalAA);
  readField(params, "fxaaEnabled", p.style.fxaaEnabled);
  readField(params, "fixedTime", p.fixedTimeSeconds);
  readField(params, "temporalSharpness", p.temporalSharpness);
  p.temporalSharpness = std::clamp(p.temporalSharpness, 0.0f, .5f);
  readField(params, "edgeSoftness", p.edgeSoftness);
  p.edgeSoftness = std::clamp(p.edgeSoftness, 0.0f, 1.0f);
  readField(params, "grassGroundDrawDistance", p.grassGroundDrawDistance);
  p.grassGroundDrawDistance = std::max(0.0f, p.grassGroundDrawDistance);
  readField(params, "terrainPhotoAlbedo", p.terrainPhotoAlbedo);
  readField(params, "foliageNormalSoften", p.foliageNormalSoften);
  readField(params, "specularOcclusion", p.specularOcclusion);
  readField(params, "terrainTriplanar", p.terrainTriplanar);
  readField(params, "stylizedLightingRamp", p.stylizedLightingRamp);
  readField(params, "shadowCoolBias", p.shadowCoolBias);
  readField(params, "snowCoverage", p.snowCoverage);

  readField(params, "fov", p.fovDeg);
  readField(params, "farPlane", p.farPlane);

  readField(params, "sunYaw", p.lightYawDeg);
  readField(params, "sunPitch", p.lightPitchDeg);
  readField(params, "sunIntensity", p.sunIntensity);
  readField(params, "ambientIntensity", p.ambientIntensity);
  readField(params, "timeOfDay", p.timeOfDayEnabled);
  readField(params, "timeOfDaySpeed", p.timeOfDaySpeed);

  readField(params, "shadowStrength", p.shadowStrength);
  readField(params, "shadowSoftness", p.shadowSoftness);

  atmosphere::readLights(params, p);

  readField(params, "fogDensity", p.fogDensity);
  readField(params, "fogStart", p.fogStart);
  readField(params, "fogMaxOpacity", p.fogMaxOpacity);
  readField(params, "fogHeightFalloff", p.fogHeightFalloff);
  readField(params, "fogHeightRef", p.fogHeightRef);
  readField(params, "fogAerialStrength", p.fogAerialStrength);
  readField(params, "fogSunInscatter", p.fogSunInscatter);
  readField(params, "fogAnisotropy", p.fogAnisotropy);
  readField(params, "fogNoiseStrength", p.fogNoiseStrength);
  readField(params, "fogNoiseScale", p.fogNoiseScale);
  readField(params, "fogNoiseWindSpeed", p.fogNoiseWindSpeed);
  readField(params, "fogSkyStrength", p.fogSkyStrength);
  readField(params, "water", p.waterEnabled);
  readField(params, "waterLevel", p.waterLevel);
  readField(params, "waterClarity", p.waterClarity);
  readField(params, "waterRoughness", p.waterRoughness);
  readField(params, "waveAmplitude", p.waveAmplitude);
  readField(params, "waveScale", p.waveScale);
  readField(params, "waveSpeed", p.waveSpeed);
  readField(params, "waterReflectionStrength", p.waterReflectionStrength);
  readField(params, "waterSsrSteps", p.waterSsrSteps);
  readField(params, "waterSsrThickness", p.waterSsrThickness);
  readField(params, "waterFoamDepth", p.waterFoamDepth);
  readField(params, "waterFoamStrength", p.waterFoamStrength);
  readVec3Field(params, "waterShallowColor", p.waterShallowColor);
  readVec3Field(params, "waterDeepColor", p.waterDeepColor);

  readField(params, "volumetric", p.volumetricEnabled);
  readField(params, "volumetricIntensity", p.volumetricIntensity);
  readField(params, "volumetricEnabled", p.volumetricEnabled);
  readField(params, "volumetricDensityScale", p.volumetricDensityScale);
  readField(params, "volumetricHeightFalloffScale", p.volumetricHeightFalloffScale);
  readField(params, "volumetricAnisotropy", p.volumetricAnisotropy);
  readVec3Field(params, "volumetricTintColor", p.volumetricTintColor);
  readField(params, "volumetricTintStrength", p.volumetricTintStrength);
  readField(params, "bloomIntensity", p.bloomIntensity);
  readField(params, "aoStrength", p.aoStrength);
  readField(params, "aoRadius", p.aoRadius);
  readField(params, "aoBias", p.aoBias);
  readField(params, "debugView", p.debugViewMode);

  // Style parameters used to be editor-only, which left authored cinematic
  // captures stuck with the default illustrative ink/paper treatment. Keep
  // these optional so existing scripts retain the established house style.
  readField(params, "outlineStrength", p.style.outlineStrength);
  readField(params, "outlineWidth", p.style.outlineWidth);
  readField(params, "worldPaperStrength", p.style.worldPaperStrength);
  readField(params, "screenPaperStrength", p.style.screenPaperStrength);
  readField(params, "skyGradeStrength", p.style.skyGradeStrength);
  readField(params, "cloudCoverage", p.style.cloudCoverage);
  readField(params, "cloudSoftness", p.style.cloudSoftness);
  readField(params, "cloudDeckHeight", p.style.cloudDeckHeight);
  readField(params, "cloudFeatureScale", p.style.cloudFeatureScale);
  readField(params, "cloudOpticalDensity", p.style.cloudOpticalDensity);
  readField(params, "cloudSunOcclusion", p.style.cloudSunOcclusion);
  readField(params, "paintedClouds", p.style.paintedClouds);
  readField(params, "cloudStrength", p.style.cloudStrength);
  readField(params, "cloudVolumetricEnabled", p.style.cloudVolumetricEnabled);
  readField(params, "cloudLayerThickness", p.style.cloudLayerThickness);
  readField(params, "cloudDensityMultiplier", p.style.cloudDensityMultiplier);
  readField(params, "cloudShapeScale", p.style.cloudShapeScale);
  readField(params, "cloudDetailScale", p.style.cloudDetailScale);
  readField(params, "cloudWeatherScale", p.style.cloudWeatherScale);
  readField(params, "cloudLightAbsorption", p.style.cloudLightAbsorption);
  readField(params, "cloudAmbientStrength", p.style.cloudAmbientStrength);
  readField(params, "cloudCurlStrength", p.style.cloudCurlStrength);
  readField(params, "cloudPhaseG", p.style.cloudPhaseG);
  readField(params, "cloudSilverIntensity", p.style.cloudSilverIntensity);
  readField(params, "cloudSilverSpread", p.style.cloudSilverSpread);
  readField(params, "cloudPowderStrength", p.style.cloudPowderStrength);
  readField(params, "cloudMaxMarchDist", p.style.cloudMaxMarchDist);
  readField(params, "cloudMaxSteps", p.style.cloudMaxSteps);
  readField(params, "cloudLightTaps", p.style.cloudLightTaps);
  readField(params, "cloudTypeBias", p.style.cloudTypeBias);
  readField(params, "cloudDetailStrength", p.style.cloudDetailStrength);
  readField(params, "sunDiscIntensity", p.sunDiscIntensity);
  readField(params, "moonGlowIntensity", p.moonGlowIntensity);
  readField(params, "starIntensity", p.starIntensity);
  readField(params, "atmosphereHaze", p.atmosphereHaze);
  readField(params, "skyBrightness", p.skyBrightness);
  readVec3Field(params, "outlineColor", p.style.outlineColor);
  readVec3Field(params, "skyZenith", p.style.skyZenith);
  readVec3Field(params, "skyHorizon", p.style.skyHorizon);
  readVec3Field(params, "cloudLit", p.style.cloudLit);
  readVec3Field(params, "cloudMid", p.style.cloudMid);
  readVec3Field(params, "cloudBase", p.style.cloudBase);

  readVec3Field(params, "fogDayColor", p.fogDayColor);
  readVec3Field(params, "fogNightColor", p.fogNightColor);
  readVec3Field(params, "camPos", p.camPos);
  readField(params, "camYaw", p.camYawDeg);
  readField(params, "camPitch", p.camPitchDeg);
  atmosphere::read(params, p);
  if (writesCamera)
    ++p.externalCameraRevision;
}

nlohmann::json renderParamsToJson(const VkAppState &state) {
  nlohmann::json j = nlohmann::json::object();
  if (!state.renderer)
    return j;
  const vkrhi::VulkanRenderer::Params &p = state.renderer->params();
  j["atmosphere"] = atmosphere::encode(atmosphere::fromLegacy(p));
  j["exposure"] = p.exposure;
  j["gamma"] = p.gamma;
  j["saturation"] = p.saturation;
  j["contrast"] = p.contrast;
  j["vignette"] = p.vignette;
  j["autoExposure"] = p.autoExposure;
  j["autoExposureSpeed"] = p.autoExposureSpeed;
  j["autoExposureMin"] = p.autoExposureMin;
  j["autoExposureMax"] = p.autoExposureMax;
  j["tonemapMode"] = p.tonemapMode;
  j["temporalAA"] = p.temporalAA;
  j["fxaaEnabled"] = p.style.fxaaEnabled;
  j["fixedTime"] = p.fixedTimeSeconds;
  j["temporalSharpness"] = p.temporalSharpness;
  j["edgeSoftness"] = p.edgeSoftness;
  j["grassGroundDrawDistance"] = p.grassGroundDrawDistance;
  j["terrainPhotoAlbedo"] = p.terrainPhotoAlbedo;
  j["foliageNormalSoften"] = p.foliageNormalSoften;
  j["specularOcclusion"] = p.specularOcclusion;
  j["terrainTriplanar"] = p.terrainTriplanar;
  j["stylizedLightingRamp"] = p.stylizedLightingRamp;
  j["shadowCoolBias"] = p.shadowCoolBias;
  j["snowCoverage"] = p.snowCoverage;

  j["fov"] = p.fovDeg;
  j["farPlane"] = p.farPlane;

  j["sunYaw"] = p.lightYawDeg;
  j["sunPitch"] = p.lightPitchDeg;
  j["sunIntensity"] = p.sunIntensity;
  j["ambientIntensity"] = p.ambientIntensity;
  j["timeOfDay"] = p.timeOfDayEnabled;
  j["timeOfDaySpeed"] = p.timeOfDaySpeed;

  j["shadowStrength"] = p.shadowStrength;
  j["shadowSoftness"] = p.shadowSoftness;

  j["pointLights"] = atmosphere::encodeLights(p);

  j["fogDensity"] = p.fogDensity;
  j["fogStart"] = p.fogStart;
  j["fogMaxOpacity"] = p.fogMaxOpacity;
  j["fogHeightFalloff"] = p.fogHeightFalloff;
  j["fogHeightRef"] = p.fogHeightRef;
  j["fogAerialStrength"] = p.fogAerialStrength;
  j["fogSunInscatter"] = p.fogSunInscatter;
  j["fogAnisotropy"] = p.fogAnisotropy;
  j["fogNoiseStrength"] = p.fogNoiseStrength;
  j["fogNoiseScale"] = p.fogNoiseScale;
  j["fogNoiseWindSpeed"] = p.fogNoiseWindSpeed;
  j["fogSkyStrength"] = p.fogSkyStrength;
  j["fogDayColor"] = {p.fogDayColor.r, p.fogDayColor.g, p.fogDayColor.b};
  j["fogNightColor"] = {p.fogNightColor.r, p.fogNightColor.g, p.fogNightColor.b};

  j["water"] = p.waterEnabled;
  j["waterLevel"] = p.waterLevel;
  j["waterClarity"] = p.waterClarity;
  j["waterRoughness"] = p.waterRoughness;
  j["waveAmplitude"] = p.waveAmplitude;
  j["waveScale"] = p.waveScale;
  j["waveSpeed"] = p.waveSpeed;
  j["waterReflectionStrength"] = p.waterReflectionStrength;
  j["waterSsrSteps"] = p.waterSsrSteps;
  j["waterSsrThickness"] = p.waterSsrThickness;
  j["waterFoamDepth"] = p.waterFoamDepth;
  j["waterFoamStrength"] = p.waterFoamStrength;
  j["waterShallowColor"] = {p.waterShallowColor.r, p.waterShallowColor.g, p.waterShallowColor.b};
  j["waterDeepColor"] = {p.waterDeepColor.r, p.waterDeepColor.g, p.waterDeepColor.b};

  j["volumetric"] = p.volumetricEnabled;
  j["volumetricIntensity"] = p.volumetricIntensity;
  j["volumetricDensityScale"] = p.volumetricDensityScale;
  j["volumetricAnisotropy"] = p.volumetricAnisotropy;
  j["bloomIntensity"] = p.bloomIntensity;
  j["aoStrength"] = p.aoStrength;
  j["aoRadius"] = p.aoRadius;
  j["aoBias"] = p.aoBias;
  j["debugView"] = p.debugViewMode;

  j["outlineStrength"] = p.style.outlineStrength;
  j["outlineWidth"] = p.style.outlineWidth;
  j["worldPaperStrength"] = p.style.worldPaperStrength;
  j["screenPaperStrength"] = p.style.screenPaperStrength;
  j["skyGradeStrength"] = p.style.skyGradeStrength;
  j["cloudCoverage"] = p.style.cloudCoverage;
  j["cloudSoftness"] = p.style.cloudSoftness;
  j["cloudDeckHeight"] = p.style.cloudDeckHeight;
  j["cloudFeatureScale"] = p.style.cloudFeatureScale;
  j["cloudOpticalDensity"] = p.style.cloudOpticalDensity;
  j["cloudSunOcclusion"] = p.style.cloudSunOcclusion;
  j["paintedClouds"] = p.style.paintedClouds;
  j["cloudStrength"] = p.style.cloudStrength;
  j["cloudVolumetricEnabled"] = p.style.cloudVolumetricEnabled;
  j["cloudLayerThickness"] = p.style.cloudLayerThickness;
  j["cloudDensityMultiplier"] = p.style.cloudDensityMultiplier;
  j["cloudShapeScale"] = p.style.cloudShapeScale;
  j["cloudDetailScale"] = p.style.cloudDetailScale;
  j["cloudWeatherScale"] = p.style.cloudWeatherScale;
  j["cloudLightAbsorption"] = p.style.cloudLightAbsorption;
  j["cloudAmbientStrength"] = p.style.cloudAmbientStrength;
  j["cloudCurlStrength"] = p.style.cloudCurlStrength;
  j["cloudPhaseG"] = p.style.cloudPhaseG;
  j["cloudSilverIntensity"] = p.style.cloudSilverIntensity;
  j["cloudSilverSpread"] = p.style.cloudSilverSpread;
  j["cloudPowderStrength"] = p.style.cloudPowderStrength;
  j["cloudMaxMarchDist"] = p.style.cloudMaxMarchDist;
  j["cloudMaxSteps"] = p.style.cloudMaxSteps;
  j["cloudLightTaps"] = p.style.cloudLightTaps;
  j["cloudTypeBias"] = p.style.cloudTypeBias;
  j["cloudDetailStrength"] = p.style.cloudDetailStrength;
  j["sunDiscIntensity"] = p.sunDiscIntensity;
  j["moonGlowIntensity"] = p.moonGlowIntensity;
  j["starIntensity"] = p.starIntensity;
  j["atmosphereHaze"] = p.atmosphereHaze;
  j["skyBrightness"] = p.skyBrightness;

  j["outlineColor"] = {p.style.outlineColor.r, p.style.outlineColor.g, p.style.outlineColor.b};
  j["skyZenith"] = {p.style.skyZenith.r, p.style.skyZenith.g, p.style.skyZenith.b};
  j["skyHorizon"] = {p.style.skyHorizon.r, p.style.skyHorizon.g, p.style.skyHorizon.b};
  j["cloudLit"] = {p.style.cloudLit.r, p.style.cloudLit.g, p.style.cloudLit.b};
  j["cloudMid"] = {p.style.cloudMid.r, p.style.cloudMid.g, p.style.cloudMid.b};
  j["cloudBase"] = {p.style.cloudBase.r, p.style.cloudBase.g, p.style.cloudBase.b};

  j["camPos"] = nlohmann::json::array({p.camPos.x, p.camPos.y, p.camPos.z});
  j["camYaw"] = p.camYawDeg;
  j["camPitch"] = p.camPitchDeg;
  return j;
}

void registerVkScriptBindings(sol::state &lua, VkAppState &state) {
  VkAppState *st = &state;

  // ── render ─────────────────────────────────────────────────────────
  auto renderTable = lua.create_named_table("render");
  // Runtime owns scenery composition; keep these out of EngineCore bindings.
  // Commands and scripts execute on the main thread, where regeneration is safe.
  renderTable["scenery"] = [st](const std::string &id) {
    return st->terrainSubsystem && st->terrainSubsystem->applyScenery(id);
  };
  renderTable["scenery_id"] = [st]() -> std::string {
    return st->terrainSubsystem ? st->terrainSubsystem->sceneryId() : std::string{};
  };

  // render.params{ exposure=, sunPitch=, ... } -- every field optional, and
  // anything omitted keeps its current value. Deliberately a keyword table
  // rather than positional arguments: this set grows, and a script written
  // today should not break when it does.
  // Converts the Lua table to JSON and applies it through the shared mapping,
  // so this binding and the command port's render.setParams cannot drift.
  // sol::object rather than sol::table: it binds a table argument just as
  // well and converts straight through ScriptJson without a cast.
  renderTable["params"] = [st](sol::object opts) {
    applyRenderParamsJson(*st, scriptjson::toJson(opts));
  };

  renderTable["get_params"] = [st, &lua]() -> sol::object {
    return scriptjson::toLua(lua, renderParamsToJson(*st));
  };

  // render.capture(path) -- the PNG is written by the NEXT drawFrame, so it
  // is not on disk when this returns. Anything waiting on the file has to
  // let at least one frame pass; the eventual command port turns this into a
  // deferred reply for exactly that reason.
  renderTable["capture"] = [st](const std::string &path) -> bool {
    if (!st->renderer || path.empty())
      return false;
    st->renderer->requestCapture(path);
    return true;
  };

  renderTable["stats"] = [st, &lua]() -> sol::table {
    sol::table t = lua.create_table();
    if (!st->renderer)
      return t;
    const vkrhi::VulkanRenderer::FrameStats &s = st->renderer->frameStats();
    t["instancesDrawn"] = s.instancesDrawn;
    t["instancesCulled"] = s.instancesCulled;
    t["vegInstancesDrawn"] = s.vegInstancesDrawn;
    t["vegTrianglesDrawn"] = s.vegTrianglesDrawn;
    t["grassTrianglesDrawn"] = s.grassTrianglesDrawn;
    t["treeTrianglesDrawn"] = s.treeTrianglesDrawn;
    t["tlasRebuiltThisFrame"] = s.tlasRebuiltThisFrame;
    t["tlasInstances"] = s.tlasInstances;
    t["meshSlotsLive"] = s.meshSlotsLive;
    t["meshSlotsFree"] = s.meshSlotsFree;
    t["gpuTotalMs"] = s.gpuTotalMs;
    t["gpuPrepareMs"] = s.gpuPrepareMs;
    t["gpuDepthMs"] = s.gpuDepthMs;
    t["gpuAtmosphereMs"] = s.gpuAtmosphereMs;
    t["gpuSceneMs"] = s.gpuSceneMs;
    t["gpuWaterMs"] = s.gpuWaterMs;
    t["gpuPostMs"] = s.gpuPostMs;
    t["gpuBloomMs"] = s.gpuBloomMs;
    t["cpuFogMs"] = st->renderer->cpuFogMilliseconds();
    t["cpuBloomMs"] = st->renderer->cpuBloomMilliseconds();
    t["gpuCloudsMs"] = s.gpuCloudsMs;
    t["gpuSkyEnvironmentMs"] = s.gpuSkyEnvironmentMs;
    t["gpuSkyLutsMs"] = s.gpuSkyLutsMs;
    t["gpuCloudShadowMs"] = s.gpuCloudShadowMs;
    t["gpuCloudHistoryMs"] = s.gpuCloudHistoryMs;
    t["cloudHistoryAllocatedBytes"] = s.cloudHistoryAllocatedBytes;
    t["cloudDetailAllocatedBytes"] = s.cloudDetailAllocatedBytes;
    t["gpuExposureMeterMs"] = s.gpuExposureMeterMs;
    t["meteredLuminance"] = s.meteredLuminance;
    t["gpuSSAOMs"] = s.gpuSSAOMs;
    t["gpuFogVisibilityMs"] = s.gpuFogVisibilityMs;
    t["gpuFogInjectionMs"] = s.gpuFogInjectionMs;
    t["gpuFogHistoryMs"] = s.gpuFogHistoryMs;
    t["gpuFogIntegrationMs"] = s.gpuFogIntegrationMs;
    t["atmosphereAllocatedBytes"] = s.atmosphereAllocatedBytes;

    return t;
  };

  // ── game ───────────────────────────────────────────────────────────
  // The HUD a gameplay script draws through. Same keyword-table shape as
  // render.params: every field optional, omitted fields keep their value,
  // so a script can update just the score without restating the whole HUD.
  //
  // Scripts declare content, not pixels -- see GameHud.h for why.
  auto gameTable = lua.create_named_table("game");

  gameTable["hud"] = [](sol::object opts) {
    GameHud::State &h = GameHud::get();
    h.active = true;
    if (opts.get_type() != sol::type::table)
      return;
    const sol::table t = opts.as<sol::table>();
    h.title = t.get_or("title", h.title);
    h.score = t.get_or("score", h.score);
    h.total = t.get_or("total", h.total);
    h.time = t.get_or("time", h.time);
    h.timeMax = t.get_or("timeMax", h.timeMax);
    h.hint = t.get_or("hint", h.hint);
    // Banner is per-frame: absent means "no banner right now", which is what
    // lets a script flash "+4s" for a moment without scheduling a clear.
    h.banner = t.get_or("banner", std::string());
    h.bannerTone = t.get_or("bannerTone", std::string("neutral"));
  };

  gameTable["clear_hud"] = []() { GameHud::get().reset(); };

  // Reads back what the HUD is currently showing. The game's own score and
  // clock live in script locals, so without this there is no way to assert
  // on them from outside -- which is what makes the game testable over the
  // command port instead of only by looking at it.
  gameTable["get_hud"] = [&lua]() -> sol::table {
    const GameHud::State &h = GameHud::get();
    sol::table t = lua.create_table();
    t["active"] = h.active;
    t["title"] = h.title;
    t["score"] = h.score;
    t["total"] = h.total;
    t["time"] = h.time;
    t["timeMax"] = h.timeMax;
    t["banner"] = h.banner;
    t["bannerTone"] = h.bannerTone;
    t["hint"] = h.hint;
    return t;
  };

  // game.play() / game.stop() -- deferred by one frame (see VkAppState).
  // With these, an agent can assemble a scene over the command port and
  // start it playing without anyone touching the editor.
  gameTable["play"] = [st]() { st->requestPlayMode = true; };
  gameTable["stop"] = [st]() { st->requestStopMode = true; };
  gameTable["is_playing"] = [st]() -> bool {
    return st->playState == VkAppState::PlayState::Playing;
  };

  // game.spawn_player{ pos={x,y,z}, yaw=, pitch=, onGround=true }
  //
  // The same entity Create > Player builds (see PlayerArchetype.h), so a
  // scene assembled entirely over the command port can still be played.
  // Returns the entity id, or 0 if a Player already exists -- spawning a
  // second one would give the controller two cameras to choose between.
  gameTable["spawn_player"] = [st](sol::optional<sol::table> optsOpt) -> uint32_t {
    Registry &reg = st->scene.registry();
    for (EntityId e : reg.view<NameComponent>())
      if (reg.get<NameComponent>(e).name == "Player")
        return 0;

    glm::vec3 pos(0.0f, 2.0f, 0.0f);
    float yaw = 0.0f, pitch = 0.0f;
    bool onGround = true;
    if (optsOpt) {
      const sol::table t = *optsOpt;
      sol::optional<sol::table> p = t["pos"];
      if (p && p->size() >= 3)
        pos = glm::vec3((*p)[1].get_or(0.0f), (*p)[2].get_or(2.0f),
                        (*p)[3].get_or(0.0f));
      yaw = t.get_or("yaw", yaw);
      pitch = t.get_or("pitch", pitch);
      onGround = t.get_or("onGround", onGround);
    }
    if (onGround && st->terrainSubsystem)
      pos.y = st->terrainSubsystem->heightAt(glm::vec2(pos.x, pos.z)) + 1.62f;

    const EntityId id = gameplay::spawnPlayer(reg, pos, yaw, pitch, "");
    const EntityId bodyId = st->scene.spawnPrimitive("cube");
    if (bodyId != 0) {
      reg.get<NameComponent>(bodyId).name = "Player Body";
      auto &body = reg.get<TransformComponent>(bodyId);
      body.position = pos + glm::vec3(0.0f, -0.55f, 0.0f);
      body.scale = glm::vec3(0.38f, 0.62f, 0.24f);
    }
    LOG_INFO("Game", "spawned Player entity " + std::to_string(id));
    return static_cast<uint32_t>(id);
  };

  // ── terrain ────────────────────────────────────────────────────────
  auto weaponTable=lua.create_named_table("weapon");
  weaponTable["equip"]=[st](uint32_t id) {
    auto &reg=st->scene.registry();
    if(!reg.valid(id)||!reg.has<CameraComponent>(id))return false;
    if(!reg.has<RifleComponent>(id))reg.emplace<RifleComponent>(id);
    reg.get<RifleComponent>(id).state.enabled=true;
    st->gameplay.playerId=id;
    return true;
  };
  weaponTable["control"]=[st](sol::table t) {
    auto &r=st->rifleSystem;r.manualInput=t.get_or("manual",true);
    r.trigger=t.get_or("trigger",false);r.aim=t.get_or("aim",false);
    r.reloadRequested=t.get_or("reload",false);
  };
  weaponTable["mode"]=[st](bool automatic) {
    auto &reg=st->scene.registry();const auto id=st->gameplay.playerId;
    if(reg.has<RifleComponent>(id))reg.get<RifleComponent>(id).state.automatic=automatic;
  };
  weaponTable["health"]=[st](uint32_t id) {
    auto &reg=st->scene.registry();
    return reg.has<DestructibleComponent>(id)?reg.get<DestructibleComponent>(id).health:-1.f;
  };
  weaponTable["target"]=[st](sol::table t) {
    auto &reg=st->scene.registry();auto id=st->scene.spawnPrimitive("cube");
    glm::vec3 pos(0,1.62f,-12);readColor(t,"pos",pos);
    reg.get<TransformComponent>(id).position=pos;
    reg.get<TransformComponent>(id).scale={.7f,1.4f,.2f};
    reg.get<NameComponent>(id).name="Shooting target";
    reg.emplace<DestructibleComponent>(id).health=std::clamp(t.get_or("health",100.f),1.f,10000.f);
    auto &col=reg.emplace<ColliderComponent>(id);col.shape=ColliderComponent::Shape::Box;
    auto &rb=reg.emplace<RigidbodyComponent>(id);rb.type=RigidbodyComponent::Type::Static;
    return uint32_t(id);
  };
  weaponTable["reload"]=[st]() {st->rifleSystem.reloadRequested=true;};
  weaponTable["state"]=[st,&lua]() {
    sol::table t=lua.create_table();auto &reg=st->scene.registry();
    const auto id=st->gameplay.playerId;
    if(reg.has<RifleComponent>(id)) {
      const auto &w=reg.get<RifleComponent>(id).state;
      t["magazine"]=w.magazine;t["reserve"]=w.reserve;t["shots"]=w.shotsFired;
      t["reloading"]=w.reloading();t["reloadProgress"]=w.reloadProgress();
      t["automatic"]=w.automatic;
      t["shotAge"]=st->rifleSystem.shotAge();t["aim"]=st->rifleSystem.aimBlend();
      t["hit"]=st->gameplay.debug.debugGameplayHit;t["hitEntity"]=st->gameplay.debug.debugGameplayHitId;
    }
    return t;
  };

  auto terrainTable = lua.create_named_table("terrain");

  // Water surface altitude at a world XZ (sea level, or a lake's own level),
  // or a large negative where there is none. Exposed because "is there water
  // here and how high is it" is otherwise only answerable by eye, and the
  // lake placement rules are exactly the kind of thing you need to probe.
  // terrain.world_map(path, pixels, extent) -- top-down PNG of the whole
  // world: ocean shaded by depth, land by altitude, shoreline picked out.
  // A bounded world's shape is a global property, and no in-engine camera can
  // see it; without this, "are these islands or is it noise dipping below the
  // waterline" is unanswerable except by flying around.
  terrainTable["world_map"] = [st](const std::string &path, int pixels,
                                   float extent) -> bool {
    if (!st->terrainSubsystem || !st->terrainSubsystem->hasTerrain())
      return false;
    const int n = std::clamp(pixels, 32, 2048);
    const TerrainSettings &ts = st->terrain.settings();
    const TerrainNoiseSet &ns = st->terrain.noiseSet();
    const float span = extent > 1.0f ? extent : ts.worldRadius * 2.2f;
    std::vector<unsigned char> img(static_cast<size_t>(n) * n * 3);
    WaterCellCache cache;
    for (int j = 0; j < n; ++j) {
      for (int i = 0; i < n; ++i) {
        const glm::vec2 xz((static_cast<float>(i) / (n - 1) - 0.5f) * span,
                           (static_cast<float>(j) / (n - 1) - 0.5f) * span);
        TerrainMacroSample macro = sampleMacro(ns, xz, ts);
        const float h = computeHeight(macro, ns, xz, ts, nullptr, &cache);
        const float w = waterSurfaceAt(ns, ts, xz, nullptr, &cache);
        unsigned char r, g, b;
        if (h < w) {
          // Ocean/lake, shaded by depth.
          const float d = std::clamp((w - h) / 45.0f, 0.0f, 1.0f);
          r = static_cast<unsigned char>(40 - 28 * d);
          g = static_cast<unsigned char>(110 - 70 * d);
          b = static_cast<unsigned char>(170 - 70 * d);
        } else if (h - w < 2.0f) {
          r = 226; g = 212; b = 160;                          // beach
        } else {
          // Land tinted by its island's ARCHETYPE, shaded by altitude. The
          // whole point of the island pass is that landmasses differ, and a
          // pure height ramp cannot show that -- two islands of equal height
          // would look identical whatever biome they carry.
          const IslandInfo &isle = ns.islands.infoAt(xz);
          glm::vec3 base(0.42f, 0.62f, 0.30f);
          switch (isle.archetype) {
          case IslandArchetype::Meadows:  base = glm::vec3(0.55f, 0.72f, 0.33f); break;
          case IslandArchetype::Forest:   base = glm::vec3(0.19f, 0.44f, 0.22f); break;
          case IslandArchetype::Highland: base = glm::vec3(0.52f, 0.50f, 0.48f); break;
          case IslandArchetype::Marsh:    base = glm::vec3(0.40f, 0.44f, 0.26f); break;
          default: break;
          }
          const float a = std::clamp(h / 90.0f, 0.0f, 1.0f);
          glm::vec3 c = base * (0.72f + 0.55f * a);
          if (a > 0.55f) // snow line
            c = glm::mix(c, glm::vec3(0.95f), (a - 0.55f) / 0.45f);
          r = static_cast<unsigned char>(std::clamp(c.x, 0.0f, 1.0f) * 255.0f);
          g = static_cast<unsigned char>(std::clamp(c.y, 0.0f, 1.0f) * 255.0f);
          b = static_cast<unsigned char>(std::clamp(c.z, 0.0f, 1.0f) * 255.0f);
        }
        const size_t o = (static_cast<size_t>(j) * n + i) * 3;
        img[o] = r; img[o + 1] = g; img[o + 2] = b;
      }
    }
    const bool ok = stbi_write_png(path.c_str(), n, n, 3, img.data(), n * 3) != 0;
    LOG_INFO("Terrain", std::string(ok ? "wrote " : "FAILED to write ") + path);
    return ok;
  };

  terrainTable["water_at"] = [st](float x, float z) -> float {
    if (!st->terrainSubsystem || !st->terrainSubsystem->hasTerrain())
      return -1.0e9f;
    WaterCellCache cache;
    return waterSurfaceAt(st->terrain.noiseSet(), st->terrain.settings(),
                          glm::vec2(x, z), &st->terrain.editsGrid(), &cache);
  };

  terrainTable["height_at"] = [st](float x, float z) -> float {
    return st->terrainSubsystem ? st->terrainSubsystem->heightAt(glm::vec2(x, z))
                                : 0.0f;
  };
  // Main-thread authoring, shared with the editor brush. Fog-field workers
  // receive an immutable snapshot and reject any older edit generation.
  terrainTable["brush_height"] = [st](float x,float z,float radius,float strength,bool lower) -> bool {
    if(!st->terrainSubsystem || !st->terrainSubsystem->hasTerrain() ||
       !std::isfinite(x) || !std::isfinite(z) || !std::isfinite(radius) ||
       !std::isfinite(strength) || radius<=0) return false;
    st->terrainSubsystem->applyHeightBrush(glm::vec2(x,z),radius,strength,lower);
    return true;
  };

  // terrain.regenerate{...} rebuilds terrain that already exists, and
  // CREATES it when there is none.
  //
  // It used to silently return false in the second case, because
  // VkTerrainSubsystem::regenerate() early-outs on !mReady -- and since no
  // binding exposed create(), a script or an agent driving the command port
  // had no way to make terrain at all. The world could only be given ground
  // by clicking Create > Terrain in the editor, which makes an
  // agent-authored scene impossible to set up.
  terrainTable["regenerate"] = [st](sol::optional<sol::table> optsOpt) -> bool {
    if (!st->terrainSubsystem)
      return false;
    const bool exists = st->terrainSubsystem->hasTerrain();
    TerrainSettings s = exists ? st->terrainSubsystem->settings()
                               : st->terrainSubsystem->pendingSettings();
    if (optsOpt) {
      const sol::table opts = *optsOpt;
      s.seed = opts.get_or("seed", s.seed);
      s.heightScale = opts.get_or("heightScale", s.heightScale);
      s.noiseFrequency = opts.get_or("noiseFrequency", s.noiseFrequency);
      s.octaves = opts.get_or("octaves", s.octaves);
      s.viewDistanceChunks =
          opts.get_or("viewDistanceChunks", s.viewDistanceChunks);
      s.spawnVegetation = opts.get_or("vegetation", s.spawnVegetation);
      s.spawnGrass = opts.get_or("grass", s.spawnGrass);
      s.treeDensityMultiplier =
          opts.get_or("treeDensity", s.treeDensityMultiplier);
      s.grassDensityMultiplier =
          opts.get_or("grassDensity", s.grassDensityMultiplier);
      s.grassCastShadows = opts.get_or("grassCastShadows",s.grassCastShadows);
      s.rockDensityMultiplier =
          opts.get_or("rockDensity", s.rockDensityMultiplier);
    }
    // Safe from script context: the frame loop is strictly sequential, and
    // scripts run before the terrain's own per-frame work (see
    // VkTerrainSubsystem's header note on regenerate()).
    if (exists) {
      st->terrainSubsystem->regenerate(s);
      return true;
    }
    return st->terrainSubsystem->create(s);
  };

  terrainTable["exists"] = [st]() -> bool {
    return st->terrainSubsystem && st->terrainSubsystem->hasTerrain();
  };

  terrainTable["destroy"] = [st]() -> bool {
    if (!st->terrainSubsystem || !st->terrainSubsystem->hasTerrain())
      return false;
    st->terrainSubsystem->destroy();
    return true;
  };

  terrainTable["settings"] = [st, &lua]() -> sol::table {
    sol::table t = lua.create_table();
    if (!st->terrainSubsystem)
      return t;
    const TerrainSettings &s = st->terrainSubsystem->settings();
    t["seed"] = s.seed;
    t["heightScale"] = s.heightScale;
    t["noiseFrequency"] = s.noiseFrequency;
    t["octaves"] = s.octaves;
    t["viewDistanceChunks"] = s.viewDistanceChunks;
    t["vegetation"] = s.spawnVegetation;
    t["grass"] = s.spawnGrass;
    t["treeDensity"] = s.treeDensityMultiplier;
    t["grassDensity"] = s.grassDensityMultiplier;
    t["grassCastShadows"] = s.grassCastShadows;
    t["rockDensity"] = s.rockDensityMultiplier;
    return t;
  };

  // terrain.scatter_layer{ name=, mesh=, density=, ... }
  // Adds a layer, or replaces the existing one with the same name. `mesh`
  // takes any asset id the renderer can resolve, including a "gen://" id from
  // assets.define -- which is the whole point: an AI can generate a tree and
  // scatter it across the world in two calls.
  terrainTable["scatter_layer"] = [st](sol::table opts) -> bool {
    if (!st->terrainSubsystem)
      return false;
    const std::string name = opts.get_or("name", std::string{});
    const std::string mesh = opts.get_or("mesh", std::string{});
    if (name.empty() || mesh.empty()) {
      LOG_ERROR("Script", "terrain.scatter_layer needs 'name' and 'mesh'");
      return false;
    }

    ScatterManifest &manifest = st->terrainSubsystem->manifest();
    auto it = std::find_if(manifest.layers.begin(), manifest.layers.end(),
                           [&](const ScatterLayer &l) { return l.name == name; });
    ScatterLayer &layer =
        (it != manifest.layers.end())
            ? *it
            : (manifest.layers.push_back(ScatterLayer{}), manifest.layers.back());

    layer.name = name;
    layer.meshPath = mesh;
    const std::string type = opts.get_or("type", std::string("tree"));
    layer.type = (type == "grass")  ? ScatterLayerType::Grass
                 : (type == "rock") ? ScatterLayerType::Rock
                                    : ScatterLayerType::Tree;
    layer.density = opts.get_or("density", layer.density);
    layer.biomeMeadow = opts.get_or("biomeMeadow", layer.biomeMeadow);
    layer.biomeForest = opts.get_or("biomeForest", layer.biomeForest);
    layer.biomeMountain = opts.get_or("biomeMountain", layer.biomeMountain);
    layer.minSpacing = opts.get_or("minSpacing", layer.minSpacing);
    layer.scaleMin = opts.get_or("scaleMin", layer.scaleMin);
    layer.scaleMax = opts.get_or("scaleMax", layer.scaleMax);
    layer.slopeMax = opts.get_or("slopeMax", layer.slopeMax);
    layer.randomYaw = opts.get_or("randomYaw", layer.randomYaw);
    layer.alignToNormal = opts.get_or("alignToNormal", layer.alignToNormal);
    layer.leanMaxDeg = opts.get_or("leanMaxDeg", layer.leanMaxDeg);
    layer.sinkIntoGround = opts.get_or("sinkIntoGround", layer.sinkIntoGround);
    layer.castRayShadow = opts.get_or("castRayShadow", layer.castRayShadow);
    layer.maxDrawDistance = opts.get_or("maxDrawDistance", layer.maxDrawDistance);
    layer.cullCellSize = opts.get_or("cullCellSize", layer.cullCellSize);
    layer.windStrength = opts.get_or("windStrength", layer.windStrength);
    layer.wind = layer.windStrength > 0.0f;
    layer.groundOcclusion = opts.get_or("groundOcclusion", layer.groundOcclusion);
    readColor(opts, "tint", layer.tint);

    // Grass placed by the ten-thousand must stay out of the TLAS; letting a
    // script opt a dense layer in is a frame-time cliff, not a look choice.
    if (layer.type == ScatterLayerType::Grass && layer.castRayShadow &&
        !opts.get_or("forceRayShadow", false))
      LOG_WARN("Script", "scatter layer '" + name +
                             "' is grass with ray-traced shadows on; the TLAS "
                             "takes one instance per placement. Set "
                             "castRayShadow=false unless density is very low.");

    // Rebuild with the current settings so the new layer takes effect.
    st->terrainSubsystem->regenerate(st->terrainSubsystem->settings());
    return true;
  };

  terrainTable["scatter_layers"] = [st, &lua]() -> sol::table {
    sol::table out = lua.create_table();
    if (!st->terrainSubsystem)
      return out;
    const ScatterManifest &m = st->terrainSubsystem->manifest();
    for (size_t i = 0; i < m.layers.size(); ++i) {
      sol::table l = lua.create_table();
      l["name"] = m.layers[i].name;
      l["mesh"] = m.layers[i].meshPath;
      l["density"] = m.layers[i].density;
      out[i + 1] = l;
    }
    return out;
  };
}
