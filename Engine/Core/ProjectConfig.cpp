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

