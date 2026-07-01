#include "ProjectConfig.h"

#include "json.hpp"

#include <filesystem>
#include <fstream>

namespace {
using json = nlohmann::json;

std::string joinPath(const std::string &a, const std::string &b) {
  namespace fs = std::filesystem;
  return (fs::path(a) / fs::path(b)).lexically_normal().string();
}

void loadString(const json &j, const char *key, std::string &out) {
  if (j.contains(key) && j[key].is_string())
    out = j[key].get<std::string>();
}

void loadFloat(const json &j, const char *key, float &out) {
  if (j.contains(key) && j[key].is_number())
    out = j[key].get<float>();
}

void loadInt(const json &j, const char *key, int &out) {
  if (j.contains(key) && j[key].is_number_integer())
    out = j[key].get<int>();
}

void loadColor(const json &j, const char *key, std::array<float, 3> &out) {
  if (!j.contains(key) || !j[key].is_array() || j[key].size() < 3)
    return;
  for (size_t i = 0; i < 3; ++i) {
    if (j[key][i].is_number())
      out[i] = j[key][i].get<float>();
  }
}

void loadBlackHoleDefaults(const json &j, BlackHolePresetDefaults &out) {
  if (!j.contains("blackHoleDefaults") || !j["blackHoleDefaults"].is_object())
    return;

  const json &bh = j["blackHoleDefaults"];
  out.valid = true;
  if (bh.contains("worldMode") && bh["worldMode"].is_boolean())
    out.worldMode = bh["worldMode"].get<bool>();
  loadFloat(bh, "azimuth", out.azimuth);
  loadFloat(bh, "elevation", out.elevation);
  loadColor(bh, "worldPosition", out.worldPosition);
  loadFloat(bh, "worldRadius", out.worldRadius);
  loadFloat(bh, "viewPitchDeg", out.viewPitchDeg);
  loadFloat(bh, "sizeDeg", out.sizeDeg);
  loadFloat(bh, "diskTiltDeg", out.diskTiltDeg);
  loadFloat(bh, "diskInclinationDeg", out.diskInclinationDeg);
  loadColor(bh, "color", out.color);
  loadFloat(bh, "ringIntensity", out.ringIntensity);
  loadFloat(bh, "ringWidth", out.ringWidth);
  loadFloat(bh, "distortion", out.distortion);
  loadFloat(bh, "haloIntensity", out.haloIntensity);
  loadFloat(bh, "diskSpinSpeed", out.diskSpinSpeed);
  loadFloat(bh, "diskFlowShear", out.diskFlowShear);
  loadFloat(bh, "diskTurbulence", out.diskTurbulence);
  loadFloat(bh, "chromaticAberration", out.chromaticAberration);
  loadFloat(bh, "eclipseStrength", out.eclipseStrength);
  loadFloat(bh, "photonRingIntensity", out.photonRingIntensity);
  loadFloat(bh, "dopplerBoost", out.dopplerBoost);
  loadFloat(bh, "jetIntensity", out.jetIntensity);
  loadFloat(bh, "coronaIntensity", out.coronaIntensity);
  loadFloat(bh, "starLensIntensity", out.starLensIntensity);
  loadFloat(bh, "shadowStrength", out.shadowStrength);
  loadFloat(bh, "innerDiskRadius", out.innerDiskRadius);
  loadFloat(bh, "outerDiskRadius", out.outerDiskRadius);
  loadFloat(bh, "diskTemperature", out.diskTemperature);
  loadFloat(bh, "diskDensity", out.diskDensity);
  loadFloat(bh, "lensingStrength", out.lensingStrength);
  loadFloat(bh, "backgroundStarIntensity", out.backgroundStarIntensity);
  loadFloat(bh, "exposure", out.exposure);
  loadInt(bh, "quality", out.quality);
}

json saveBlackHoleDefaults(const BlackHolePresetDefaults &bh) {
  return json{{"worldMode", bh.worldMode},
              {"azimuth", bh.azimuth},
              {"elevation", bh.elevation},
              {"worldPosition",
               {bh.worldPosition[0], bh.worldPosition[1], bh.worldPosition[2]}},
              {"worldRadius", bh.worldRadius},
              {"viewPitchDeg", bh.viewPitchDeg},
              {"sizeDeg", bh.sizeDeg},
              {"diskTiltDeg", bh.diskTiltDeg},
              {"diskInclinationDeg", bh.diskInclinationDeg},
              {"color", {bh.color[0], bh.color[1], bh.color[2]}},
              {"ringIntensity", bh.ringIntensity},
              {"ringWidth", bh.ringWidth},
              {"distortion", bh.distortion},
              {"haloIntensity", bh.haloIntensity},
              {"diskSpinSpeed", bh.diskSpinSpeed},
              {"diskFlowShear", bh.diskFlowShear},
              {"diskTurbulence", bh.diskTurbulence},
              {"chromaticAberration", bh.chromaticAberration},
              {"eclipseStrength", bh.eclipseStrength},
              {"photonRingIntensity", bh.photonRingIntensity},
              {"dopplerBoost", bh.dopplerBoost},
              {"jetIntensity", bh.jetIntensity},
              {"coronaIntensity", bh.coronaIntensity},
              {"starLensIntensity", bh.starLensIntensity},
              {"shadowStrength", bh.shadowStrength},
              {"innerDiskRadius", bh.innerDiskRadius},
              {"outerDiskRadius", bh.outerDiskRadius},
              {"diskTemperature", bh.diskTemperature},
              {"diskDensity", bh.diskDensity},
              {"lensingStrength", bh.lensingStrength},
              {"backgroundStarIntensity", bh.backgroundStarIntensity},
              {"exposure", bh.exposure},
              {"quality", bh.quality}};
}
} // namespace

bool ProjectConfig::loadFromFile(const std::string &path) {
  std::ifstream f(path);
  if (!f.is_open())
    return false;

  json j;
  f >> j;

  loadString(j, "projectRoot", projectRoot);
  loadString(j, "shaderRoot", shaderRoot);
  loadString(j, "assetRoot", assetRoot);
  loadString(j, "startupScene", startupScene);
  loadBlackHoleDefaults(j, blackHoleDefaults);

  loadString(j, "mainVertexShader", mainVertexShader);
  loadString(j, "mainFragmentShader", mainFragmentShader);
  loadString(j, "shadowVertexShader", shadowVertexShader);
  loadString(j, "shadowFragmentShader", shadowFragmentShader);
  loadString(j, "hdrSkyVertexShader", hdrSkyVertexShader);
  loadString(j, "hdrSkyFragmentShader", hdrSkyFragmentShader);
  loadString(j, "fireBillboardVertexShader", fireBillboardVertexShader);
  loadString(j, "fireBillboardFragmentShader", fireBillboardFragmentShader);
  loadString(j, "smokeBillboardFragmentShader", smokeBillboardFragmentShader);
  loadString(j, "projectileVertexShader", projectileVertexShader);
  loadString(j, "projectileFragmentShader", projectileFragmentShader);
  loadString(j, "screenQuadVertexShader", screenQuadVertexShader);
  loadString(j, "bloomExtractFragmentShader", bloomExtractFragmentShader);
  loadString(j, "bloomBlurFragmentShader", bloomBlurFragmentShader);
  loadString(j, "ssaoFragmentShader", ssaoFragmentShader);
  loadString(j, "ssaoBlurFragmentShader", ssaoBlurFragmentShader);
  loadString(j, "volumetricFogFragmentShader", volumetricFogFragmentShader);
  loadString(j, "bloomCompositeFragmentShader", bloomCompositeFragmentShader);

  loadString(j, "grassSideTexture", grassSideTexture);
  loadString(j, "grassTopTexture", grassTopTexture);
  loadString(j, "skyHDR", skyHDR);
  loadString(j, "fireTexture", fireTexture);

  return true;
}

bool ProjectConfig::saveToFile(const std::string &path) const {
  json j;
  j["projectRoot"] = projectRoot;
  j["shaderRoot"] = shaderRoot;
  j["assetRoot"] = assetRoot;
  j["startupScene"] = startupScene;
  if (blackHoleDefaults.valid)
    j["blackHoleDefaults"] = saveBlackHoleDefaults(blackHoleDefaults);

  j["mainVertexShader"] = mainVertexShader;
  j["mainFragmentShader"] = mainFragmentShader;
  j["shadowVertexShader"] = shadowVertexShader;
  j["shadowFragmentShader"] = shadowFragmentShader;
  j["hdrSkyVertexShader"] = hdrSkyVertexShader;
  j["hdrSkyFragmentShader"] = hdrSkyFragmentShader;
  j["fireBillboardVertexShader"] = fireBillboardVertexShader;
  j["fireBillboardFragmentShader"] = fireBillboardFragmentShader;
  j["smokeBillboardFragmentShader"] = smokeBillboardFragmentShader;
  j["projectileVertexShader"] = projectileVertexShader;
  j["projectileFragmentShader"] = projectileFragmentShader;
  j["screenQuadVertexShader"] = screenQuadVertexShader;
  j["bloomExtractFragmentShader"] = bloomExtractFragmentShader;
  j["bloomBlurFragmentShader"] = bloomBlurFragmentShader;
  j["ssaoFragmentShader"] = ssaoFragmentShader;
  j["ssaoBlurFragmentShader"] = ssaoBlurFragmentShader;
  j["volumetricFogFragmentShader"] = volumetricFogFragmentShader;
  j["bloomCompositeFragmentShader"] = bloomCompositeFragmentShader;

  j["grassSideTexture"] = grassSideTexture;
  j["grassTopTexture"] = grassTopTexture;
  j["skyHDR"] = skyHDR;
  j["fireTexture"] = fireTexture;

  std::ofstream f(path);
  if (!f.is_open())
    return false;

  f << j.dump(2);
  return true;
}

std::string ProjectConfig::shaderPath(const std::string &rel) const {
  return joinPath(projectRoot, joinPath(shaderRoot, rel));
}

std::string ProjectConfig::assetPath(const std::string &rel) const {
  return joinPath(projectRoot, joinPath(assetRoot, rel));
}

std::string ProjectConfig::projectPath(const std::string &rel) const {
  return joinPath(projectRoot, rel);
}
