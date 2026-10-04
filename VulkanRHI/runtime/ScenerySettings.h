#pragma once
#include "Rendering/AtmosphereSettings.h"
// Scenery is runtime composition, not a new EngineCore biome. These codecs
// operate on typed settings without depending on the Vulkan renderer.
#include "TerrainSettings.h"
#include "json.hpp"
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <type_traits>
namespace scenery {
using Json = nlohmann::json;
// Old saved presets predate render LODs. Keep their authored placements and
// tuning, but inherit new detail assets when the underlying mesh still matches.
// An explicit meshLods: [] remains an intentional opt-out.
inline void mergeSavedProfile(Json &profile, const Json &saved) {
  const Json defaults = profile.value("scatter", Json::object()).value("layers", Json::array());
  profile.merge_patch(saved);
  if (!defaults.is_array() || !profile.contains("scatter") ||
      !profile["scatter"].is_object() || !profile["scatter"].contains("layers") ||
      !profile["scatter"]["layers"].is_array()) return;
  for (auto &layer : profile["scatter"]["layers"]) {
    if (!layer.is_object() || layer.contains("meshLods")) continue;
    for (const auto &base : defaults) {
      if (!base.is_object() || !base.contains("meshLods")) continue;
      if (layer.value("name", "") != base.value("name", "") ||
          layer.value("mesh", "") != base.value("mesh", "")) continue;
      layer["meshLods"] = base["meshLods"];
      // Coarse chunk bounds kept whole near/far forests at the near level.
      // Restore the cell size paired with these new distance thresholds.
      if (base.contains("cullCellSize")) layer["cullCellSize"] = base["cullCellSize"];
      break;
    }
  }
}

template <class T> void field(Json &j, const char *key, T &value, bool read) {
  if (!read) {
    j[key] = value;
    return;
  }
  auto it = j.find(key);
  if (it == j.end())
    return;
  if constexpr (std::is_same_v<T, bool>) {
    if (it->is_boolean())
      value = it->get<bool>();
  } else if (it->is_number() && std::isfinite(it->get<double>()))
    value = it->get<T>();
}
inline void field(Json &j, const char *key, glm::vec3 &v, bool read) {
  if (!read) {
    j[key] = {v.x, v.y, v.z};
    return;
  }
  auto it = j.find(key);
  if (it == j.end() || !it->is_array() || it->size() != 3)
    return;
  for (auto &x : *it)
    if (!x.is_number() || !std::isfinite(x.get<double>()))
      return;
  v = {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};
}
inline void terrain(Json &j, TerrainSettings &p, bool read) {
  field(j, "authoredWoodland", p.authoredWoodland, read);
  field(j, "seed", p.seed, read);
  field(j, "chunkWorldSize", p.chunkWorldSize, read);
  field(j, "chunkResolution", p.chunkResolution, read);
  field(j, "heightScale", p.heightScale, read);
  field(j, "heightOffset", p.heightOffset, read);
  field(j, "autoCenterHeight", p.autoCenterHeight, read);
  field(j, "noiseFrequency", p.noiseFrequency, read);
  field(j, "octaves", p.octaves, read);
  field(j, "lacunarity", p.lacunarity, read);
  field(j, "gain", p.gain, read);
  field(j, "macroStrength", p.macroStrength, read);
  field(j, "landscapeScale", p.landscapeScale, read);
  field(j, "valleySpan", p.valleySpan, read);
  field(j, "useRidgeNoise", p.useRidgeNoise, read);
  field(j, "ridgeBlend", p.ridgeBlend, read);
  field(j, "mountainRegionScale", p.mountainRegionScale, read);
  field(j, "mountainCoverage", p.mountainCoverage, read);
  field(j, "mountainHeightScale", p.mountainHeightScale, read);
  field(j, "valleyDepth", p.valleyDepth, read);
  field(j, "erosionStrength", p.erosionStrength, read);
  field(j, "microReliefStrength", p.microReliefStrength, read);
  field(j, "outcropThreshold", p.outcropThreshold, read);
  field(j, "forestCoverage", p.forestCoverage, read);
  field(j, "treelineHeight", p.treelineHeight, read);
  field(j, "treelineTransition", p.treelineTransition, read);
  field(j, "worldBounded", p.worldBounded, read);
  field(j, "worldRadius", p.worldRadius, read);
  field(j, "worldEdgeFalloff", p.worldEdgeFalloff, read);
  field(j, "continentScale", p.continentScale, read);
  field(j, "landCoverage", p.landCoverage, read);
  field(j, "oceanFloorDepth", p.oceanFloorDepth, read);
  field(j, "landBaseHeight", p.landBaseHeight, read);
  field(j, "spawnIslandRadius", p.spawnIslandRadius, read);
  field(j, "oceanEnabled", p.oceanEnabled, read);
  field(j, "seaLevel", p.seaLevel, read);
  field(j, "autoSeaLevel", p.autoSeaLevel, read);
  field(j, "oceanCoverage", p.oceanCoverage, read);
  field(j, "shoreFlatten", p.shoreFlatten, read);
  field(j, "lakesEnabled", p.lakesEnabled, read);
  field(j, "lakeScale", p.lakeScale, read);
  field(j, "lakeCoverage", p.lakeCoverage, read);
  field(j, "lakeDepth", p.lakeDepth, read);
  field(j, "shoreScatterMargin", p.shoreScatterMargin, read);
  field(j, "viewDistanceChunks", p.viewDistanceChunks, read);
  field(j, "workerThreads", p.workerThreads, read);
  field(j, "maxChunkLoadsPerUpdate", p.maxChunkLoadsPerUpdate, read);
  field(j, "maxCompletedChunksPerFrame", p.maxCompletedChunksPerFrame, read);
  field(j, "collisionChunkRadius", p.collisionChunkRadius, read);
  field(j, "collisionUpdatesPerFrame", p.collisionUpdatesPerFrame, read);
  field(j, "spawnVegetation", p.spawnVegetation, read);
  field(j, "treeDensityMultiplier", p.treeDensityMultiplier, read);
  field(j, "treeSizeMultiplier", p.treeSizeMultiplier, read);
  field(j, "treeHeightMultiplier", p.treeHeightMultiplier, read);
  field(j, "treeHeightVariance", p.treeHeightVariance, read);
  field(j, "treeSpacingMultiplier", p.treeSpacingMultiplier, read);
  field(j, "forestPatchiness", p.forestPatchiness, read);
  field(j, "standRadiusMultiplier", p.standRadiusMultiplier, read);
  field(j, "treeLeanExtraDeg", p.treeLeanExtraDeg, read);
  field(j, "interactiveTreeChunkRadius", p.interactiveTreeChunkRadius, read);
  field(j, "rockDensityMultiplier", p.rockDensityMultiplier, read);
  field(j, "spawnGrass", p.spawnGrass, read);
  field(j, "grassDensityMultiplier", p.grassDensityMultiplier, read);
  field(j, "grassSizeMultiplier", p.grassSizeMultiplier, read);
  field(j, "grassHeightMultiplier", p.grassHeightMultiplier, read);
  field(j, "grassOcclusionStrength", p.grassOcclusionStrength, read);
  field(j, "grassCastShadows", p.grassCastShadows, read);
  field(j, "grassDrawDistanceMultiplier", p.grassDrawDistanceMultiplier, read);
  field(j, "grassChunkRadius", p.grassChunkRadius, read);
  field(j, "windStrength", p.windStrength, read);
  field(j, "windSpeed", p.windSpeed, read);
}
template <class P> void renderer(Json &j, P &p, bool read) {
  field(j, "lightPitchDeg", p.lightPitchDeg, read);
  field(j, "lightYawDeg", p.lightYawDeg, read);
  field(j, "sunIntensity", p.sunIntensity, read);
  field(j, "shadowSoftness", p.shadowSoftness, read);
  field(j, "shadowSamples", p.shadowSamples, read);
  field(j, "aoRadius", p.aoRadius, read);
  field(j, "aoBias", p.aoBias, read);
  field(j, "aoStrength", p.aoStrength, read);
  field(j, "ambientIntensity", p.ambientIntensity, read);
  field(j, "exposure", p.exposure, read);
  field(j, "autoExposure", p.autoExposure, read);
  field(j, "autoExposureMin", p.autoExposureMin, read);
  field(j, "autoExposureMax", p.autoExposureMax, read);
  field(j, "bloomIntensity", p.bloomIntensity, read);
  field(j, "temporalAA", p.temporalAA, read);
  field(j, "temporalSharpness", p.temporalSharpness, read);
  p.temporalSharpness = std::clamp(p.temporalSharpness, 0.0f, .5f);
  field(j, "edgeSoftness", p.edgeSoftness, read);
  p.edgeSoftness = std::clamp(p.edgeSoftness, 0.0f, 1.0f);
  field(j, "cameraGradeEnabled", p.cameraGradeEnabled, read);
  field(j, "cameraGradeStrength", p.cameraGradeStrength, read);
  field(j, "terrainSkyReflectIntensity", p.terrainSkyReflectIntensity, read);
  field(j, "iblSpecularIntensity", p.iblSpecularIntensity, read);
  field(j, "volumetricIntensity", p.volumetricIntensity, read);
  field(j, "volumetricTintStrength", p.volumetricTintStrength, read);
  field(j, "fogDensity", p.fogDensity, read);
  field(j, "fogStart", p.fogStart, read);
  field(j, "fogMaxOpacity", p.fogMaxOpacity, read);
  field(j, "fogHeightFalloff", p.fogHeightFalloff, read);
  field(j, "fogHeightRef", p.fogHeightRef, read);
  field(j, "fogAerialStrength", p.fogAerialStrength, read);
  field(j, "fogSunInscatter", p.fogSunInscatter, read);
  field(j, "fogNoiseStrength", p.fogNoiseStrength, read);
  field(j, "fogNoiseScale", p.fogNoiseScale, read);
  field(j, "fogNoiseWindSpeed", p.fogNoiseWindSpeed, read);
  field(j, "fogSkyStrength", p.fogSkyStrength, read);
  field(j, "waterRoughness", p.waterRoughness, read);
  field(j, "waterClarity", p.waterClarity, read);
  field(j, "waveAmplitude", p.waveAmplitude, read);
  field(j, "waveSpeed", p.waveSpeed, read);
  field(j, "waterReflectionStrength", p.waterReflectionStrength, read);
  field(j, "waterFoamStrength", p.waterFoamStrength, read);
  field(j, "biomeAmbientIntensityMeadow", p.biomeAmbientIntensityMeadow, read);
  field(j, "biomeAmbientIntensityForest", p.biomeAmbientIntensityForest, read);
  field(j, "biomeAmbientIntensityMountain", p.biomeAmbientIntensityMountain,
        read);
  field(j, "forestCanopyOcclusion", p.forestCanopyOcclusion, read);
  field(j, "forestLightShaftStrength", p.forestLightShaftStrength, read);
  field(j, "mountainDirectBoost", p.mountainDirectBoost, read);
  field(j, "biomeLightingStrength", p.biomeLightingStrength, read);
  field(j, "forestFogDensityMult", p.forestFogDensityMult, read);
  field(j, "mountainFogDensityMult", p.mountainFogDensityMult, read);
  field(j, "mountainAerialStrength", p.mountainAerialStrength, read);
  field(j, "timeOfDayEnabled", p.timeOfDayEnabled, read);
  field(j, "timeOfDaySpeed", p.timeOfDaySpeed, read);
  field(j, "fogAnisotropy", p.fogAnisotropy, read);
  field(j, "volumetricTintColor", p.volumetricTintColor, read);
  field(j, "volumetricEnabled", p.volumetricEnabled, read);
  field(j, "volumetricDensityScale", p.volumetricDensityScale, read);
  field(j, "skyBrightness", p.skyBrightness, read);
  field(j, "atmosphereHaze", p.atmosphereHaze, read);
  field(j, "saturation", p.saturation, read);
  field(j, "contrast", p.contrast, read);
  field(j, "snowCoverage", p.snowCoverage, read);
  field(j, "snowPatchScale", p.snowPatchScale, read);
  field(j, "snowSlopeLimit", p.snowSlopeLimit, read);
  field(j, "snowAltitudeBoost", p.snowAltitudeBoost, read);
  field(j, "snowRoughness", p.snowRoughness, read);
  field(j, "fogDayColor", p.fogDayColor, read);
  field(j, "fogNightColor", p.fogNightColor, read);
  field(j, "waterShallowColor", p.waterShallowColor, read);
  field(j, "waterDeepColor", p.waterDeepColor, read);
  field(j, "biomeAmbientTintMeadow", p.biomeAmbientTintMeadow, read);
  field(j, "biomeAmbientTintForest", p.biomeAmbientTintForest, read);
  field(j, "biomeAmbientTintMountain", p.biomeAmbientTintMountain, read);
  field(j, "forestFogTint", p.forestFogTint, read);
  field(j, "mountainFogTint", p.mountainFogTint, read);
  field(j, "snowTint", p.snowTint, read);
  field(j, "snowSparkle", p.snowSparkle, read);
  field(j, "snowWindDrift", p.snowWindDrift, read);
  field(j, "snowSubsurface", p.snowSubsurface, read);
  field(j, "iceEnabled", p.iceEnabled, read);
  field(j, "iceRoughness", p.iceRoughness, read);
  field(j, "iceClarity", p.iceClarity, read);
  field(j, "iceCracksStrength", p.iceCracksStrength, read);
  field(j, "iceFrostCoverage", p.iceFrostCoverage, read);
  field(j, "iceTint", p.iceTint, read);
  field(j, "auroraEnabled", p.auroraEnabled, read);
  field(j, "auroraIntensity", p.auroraIntensity, read);
  field(j, "auroraSpeed", p.auroraSpeed, read);
  field(j, "auroraGroundGlow", p.auroraGroundGlow, read);
  field(j, "blizzardStrength", p.blizzardStrength, read);
  field(j, "frostVignetteStrength", p.frostVignetteStrength, read);
  field(j, "stylizedLightingRamp", p.stylizedLightingRamp, read);
  field(j, "shadowCoolBias", p.shadowCoolBias, read);
  field(j, "terrainPhotoAlbedo", p.terrainPhotoAlbedo, read);
  field(j, "foliageNormalSoften", p.foliageNormalSoften, read);
  field(j, "specularOcclusion", p.specularOcclusion, read);
  field(j, "terrainTriplanar", p.terrainTriplanar, read);
  auto &s = j["style"];
  field(s, "autumnAmount", p.style.autumnAmount, read);
  field(s, "facetStrength", p.style.facetStrength, read);
  field(s, "washEdgeDarkening", p.style.washEdgeDarkening, read);
  field(s, "worldPaperStrength", p.style.worldPaperStrength, read);
  field(s, "paintedClouds", p.style.paintedClouds, read);
  field(s, "vibrance", p.style.vibrance, read);
  field(s, "splitBalance", p.style.splitBalance, read);
  field(s, "outlineWidth", p.style.outlineWidth, read);
  field(s, "outlineStrength", p.style.outlineStrength, read);
  field(s, "outlineDepthThreshold", p.style.outlineDepthThreshold, read);
  field(s, "outlineNormalThreshold", p.style.outlineNormalThreshold, read);
  field(s, "outlineDistance", p.style.outlineDistance, read);
  field(s, "screenPaperStrength", p.style.screenPaperStrength, read);
  field(s, "fxaaEnabled", p.style.fxaaEnabled, read);
  field(s, "skyGradeStrength", p.style.skyGradeStrength, read);
  field(s, "skyBands", p.style.skyBands, read);
  field(s, "skyBandSoftness", p.style.skyBandSoftness, read);
  field(s, "sunSoftness", p.style.sunSoftness, read);
  field(s, "cloudCoverage", p.style.cloudCoverage, read);
  field(s, "cloudSoftness", p.style.cloudSoftness, read);
  field(s, "cloudDeckHeight", p.style.cloudDeckHeight, read);
  field(s, "cloudFeatureScale", p.style.cloudFeatureScale, read);
  field(s, "cloudOpticalDensity", p.style.cloudOpticalDensity, read);
  field(s, "cloudSunOcclusion", p.style.cloudSunOcclusion, read);
  field(s, "cloudVolumetricEnabled", p.style.cloudVolumetricEnabled, read);
  field(s, "cloudStrength", p.style.cloudStrength, read);
  field(s, "cloudLayerThickness", p.style.cloudLayerThickness, read);
  field(s, "cloudShapeScale", p.style.cloudShapeScale, read);
  field(s, "cloudDetailScale", p.style.cloudDetailScale, read);
  field(s, "cloudWeatherScale", p.style.cloudWeatherScale, read);
  field(s, "cloudDensityMultiplier", p.style.cloudDensityMultiplier, read);
  field(s, "cloudLightAbsorption", p.style.cloudLightAbsorption, read);
  field(s, "cloudAmbientStrength", p.style.cloudAmbientStrength, read);
  field(s, "cloudCurlStrength", p.style.cloudCurlStrength, read);
  field(s, "cloudPhaseG", p.style.cloudPhaseG, read);
  field(s, "cloudSilverIntensity", p.style.cloudSilverIntensity, read);
  field(s, "cloudSilverSpread", p.style.cloudSilverSpread, read);
  field(s, "cloudPowderStrength", p.style.cloudPowderStrength, read);
  field(s, "cloudMaxMarchDist", p.style.cloudMaxMarchDist, read);
  field(s, "cloudMaxSteps", p.style.cloudMaxSteps, read);
  field(s, "cloudLightTaps", p.style.cloudLightTaps, read);
  field(s, "cloudTypeBias", p.style.cloudTypeBias, read);
  field(s, "cloudDetailStrength", p.style.cloudDetailStrength, read);
  field(s, "splitShadow", p.style.splitShadow, read);
  field(s, "splitHighlight", p.style.splitHighlight, read);
  field(s, "outlineColor", p.style.outlineColor, read);
  field(s, "skyZenith", p.style.skyZenith, read);
  field(s, "skyHorizon", p.style.skyHorizon, read);
  field(s, "cloudLit", p.style.cloudLit, read);
  field(s, "cloudMid", p.style.cloudMid, read);
  field(s, "cloudBase", p.style.cloudBase, read);
  if (!read)
    s["cloudWind"] = {p.style.cloudWind.x, p.style.cloudWind.y};
  else if (s.contains("cloudWind") && s["cloudWind"].is_array() &&
           s["cloudWind"].size() == 2) {
    Json wind = {{"x", s["cloudWind"][0]}, {"y", s["cloudWind"][1]}};
    field(wind, "x", p.style.cloudWind.x, true);
    field(wind, "y", p.style.cloudWind.y, true);
  }
  for (size_t i = 0; i < 5; ++i) {
    auto &a = s["terrain"][std::to_string(i)];
    auto &v = p.style.terrain[i];
    field(a, "lit", v.lit, read);
    field(a, "shade", v.shade, read);
    field(a, "mottleScale", v.mottleScale, read);
    field(a, "overlayStrength", v.overlayStrength, read);
  }
  for (size_t i = 0; i < 6; ++i) {
    auto &a = j["materials"][std::to_string(i)];
    auto &v = i < 5 ? p.terrainMaterialSlots[i] : p.snowMaterial;
    for (auto pair : {std::make_pair("albedo", &v.albedoPath),
                      std::make_pair("normal", &v.normalPath),
                      std::make_pair("roughness", &v.roughnessPath),
                      std::make_pair("height", &v.heightPath)}) {
      if (!read)
        a[pair.first] = *pair.second;
      else if (a.contains(pair.first) && a[pair.first].is_string())
        *pair.second = a[pair.first].template get<std::string>();
    }
    field(a, "tiling", v.tiling, read);
    field(a, "reliefDepth", v.reliefDepth, read);
    v.tiling = std::max(.5f, v.tiling);
    v.reliefDepth = std::clamp(v.reliefDepth, 0.0f, .2f);
  }
  if (read) { atmosphere::read(j, p); atmosphere::readLights(j, p); }
  else {
    j["atmosphere"] = atmosphere::encode(atmosphere::fromLegacy(p));
    j["pointLights"] = atmosphere::encodeLights(p);
  }
  p.snowCoverage = std::clamp(p.snowCoverage, 0.0f, 1.0f);
  p.snowPatchScale = std::max(.5f, p.snowPatchScale);
  p.snowSlopeLimit = std::clamp(p.snowSlopeLimit, .01f, 1.0f);
  p.snowRoughness = std::clamp(p.snowRoughness, .35f, 1.0f);
  if (read)
    p.terrainMaterialsDirty = true;
}
} // namespace scenery
