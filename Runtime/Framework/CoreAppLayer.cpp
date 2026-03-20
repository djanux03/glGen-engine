#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "AppState.h"
#include "AudioSubsystem.h"
#include "CoreAppLayer.h"
#include "EditorSubsystem.h"
#include "RenderLoopSubsystem.h"

#include "ECS/Components.h"
#include "ECS/Systems/CameraSystem.h"
#include "ECS/Systems/RenderSystem.h"

#include "EngineEvents.h"
#include "GLStateCache.h"
#include "Keyboard.h"
#include "Logger.h"
#include "Mouse.h"
#include "MousePicking.h"
#include "Core/FrameProfiler.h"

#include "imgui.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "ImGuizmo.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cctype>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "json.hpp"

namespace {
using json = nlohmann::json;

void saveConfig(const AppState &s, const char *filename) {}
void loadConfig(AppState &s, const char *filename) {}
constexpr float kTerrainEdgeMargin = 0.2f;
constexpr float kTerrainMinClearance = 0.15f;

bool raycastTerrain(const TerrainSystem &ts, const Ray &ray, float maxDist,
                    glm::vec3 &outHit) {
  const float step = 0.5f;
  for (float t = 0.0f; t <= maxDist; t += step) {
    glm::vec3 p = ray.origin + ray.direction * t;
    if (!ts.isChunkLoadedAt(p.x, p.z))
      continue;
    float h = ts.getHeightAt(p.x, p.z);
    if (p.y <= h) {
      outHit = glm::vec3(p.x, h, p.z);
      return true;
    }
  }
  return false;
}

bool projectToScreen(const glm::vec3 &world, const glm::mat4 &view,
                     const glm::mat4 &projection, float w, float h,
                     ImVec2 &out) {
  glm::vec4 clip = projection * view * glm::vec4(world, 1.0f);
  if (clip.w <= 0.0001f)
    return false;
  glm::vec3 ndc = glm::vec3(clip) / clip.w;
  if (ndc.z < -1.0f || ndc.z > 1.0f)
    return false;
  out.x = (ndc.x * 0.5f + 0.5f) * w;
  out.y = (1.0f - (ndc.y * 0.5f + 0.5f)) * h;
  return true;
}

std::vector<std::string> splitCommand(const std::string &line) {
  std::vector<std::string> out;
  std::string cur;
  bool inQuotes = false;
  for (char c : line) {
    if (c == '"') {
      inQuotes = !inQuotes;
      continue;
    }
    if (!inQuotes && std::isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty())
    out.push_back(cur);
  return out;
}

bool parseBool(const std::string &s, bool &out) {
  std::string v = s;
  std::transform(v.begin(), v.end(), v.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (v == "1" || v == "true" || v == "on" || v == "yes") {
    out = true;
    return true;
  }
  if (v == "0" || v == "false" || v == "off" || v == "no") {
    out = false;
    return true;
  }
  return false;
}

bool executeConsoleCommand(AppState &s, const std::string &line) {
  auto args = splitCommand(line);
  if (args.empty())
    return false;

  std::string cmd = args[0];
  std::transform(cmd.begin(), cmd.end(), cmd.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  LOG_INFO("Console", "> " + line);

  if (cmd == "help") {
    LOG_INFO("Console",
             "Commands: help, echo <text>, get <key>, set <key> <value>, "
             "spawn <path>, teleport <x> <y> <z>, regen_terrain, play, pause, "
             "stop");
    LOG_INFO("Console",
             "Keys: time, daynight, fog, exposure, gamma, tree_density, "
             "rock_density, grass_density");
    return false;
  }

  if (cmd == "echo") {
    if (args.size() > 1) {
      std::string msg = line.substr(line.find(' ') + 1);
      LOG_INFO("Console", msg);
    }
    return false;
  }

  if (cmd == "get") {
    if (args.size() < 2) {
      LOG_WARN("Console", "Usage: get <key>");
      return false;
    }
    const std::string &key = args[1];
    if (key == "time") {
      LOG_INFO("Console", "time = " + std::to_string(s.skyUI.timeOfDay));
    } else if (key == "daynight") {
      LOG_INFO("Console",
               std::string("daynight = ") +
                   (s.skyUI.dayNightEnabled ? "true" : "false"));
    } else if (key == "fog") {
      LOG_INFO("Console", "fog = " + std::to_string(s.render.fogDensity));
    } else if (key == "exposure") {
      LOG_INFO("Console", "exposure = " + std::to_string(s.render.exposure));
    } else if (key == "gamma") {
      LOG_INFO("Console", "gamma = " + std::to_string(s.render.gamma));
    } else if (key == "tree_density") {
      LOG_INFO("Console",
               "tree_density = " + std::to_string(s.terrainSettings.treeDensity));
    } else if (key == "rock_density") {
      LOG_INFO("Console",
               "rock_density = " + std::to_string(s.terrainSettings.rockDensity));
    } else if (key == "grass_density") {
      LOG_INFO("Console", "grass_density = " +
                               std::to_string(s.terrainSettings.grassDensity));
    } else {
      LOG_WARN("Console", "Unknown key: " + key);
    }
    return false;
  }

  if (cmd == "set") {
    if (args.size() < 3) {
      LOG_WARN("Console", "Usage: set <key> <value>");
      return false;
    }
    const std::string &key = args[1];
    const std::string &val = args[2];
    bool mutated = false;

    if (key == "time") {
      s.skyUI.timeOfDay = std::clamp(std::stof(val), 0.0f, 1.0f);
    } else if (key == "daynight") {
      bool b = false;
      if (!parseBool(val, b)) {
        LOG_WARN("Console", "Invalid bool: " + val);
        return false;
      }
      s.skyUI.dayNightEnabled = b;
    } else if (key == "fog") {
      s.render.fogDensity = std::max(0.0f, std::stof(val));
    } else if (key == "exposure") {
      s.render.exposure = std::max(0.0f, std::stof(val));
    } else if (key == "gamma") {
      s.render.gamma = std::max(0.1f, std::stof(val));
    } else if (key == "tree_density") {
      s.terrainSettings.treeDensity =
          std::clamp(std::stof(val), 0.0f, 1.0f);
      s.terrainSystem.applySettings(s.terrainSettings);
      s.terrainSystem.regenerate();
      mutated = true;
    } else if (key == "rock_density") {
      s.terrainSettings.rockDensity =
          std::clamp(std::stof(val), 0.0f, 1.0f);
      s.terrainSystem.applySettings(s.terrainSettings);
      s.terrainSystem.regenerate();
      mutated = true;
    } else if (key == "grass_density") {
      s.terrainSettings.grassDensity =
          std::clamp(std::stof(val), 0.0f, 1.0f);
      s.terrainSystem.applySettings(s.terrainSettings);
      s.terrainSystem.regenerate();
      mutated = true;
    } else {
      LOG_WARN("Console", "Unknown key: " + key);
    }
    return mutated;
  }

  if (cmd == "spawn") {
    if (args.size() < 2) {
      LOG_WARN("Console", "Usage: spawn <path>");
      return false;
    }
    s.pending.pendingSpawnPaths.push_back(args[1]);
    return true;
  }

  if (cmd == "teleport") {
    if (args.size() < 4) {
      LOG_WARN("Console", "Usage: teleport <x> <y> <z>");
      return false;
    }
    if (s.playerId == 0 ||
        !s.scene.registry().has<TransformComponent>(s.playerId)) {
      LOG_WARN("Console", "No player entity to teleport.");
      return false;
    }
    glm::vec3 pos(std::stof(args[1]), std::stof(args[2]),
                  std::stof(args[3]));
    auto &tr = s.scene.registry().get<TransformComponent>(s.playerId);
    tr.position = pos;
    if (s.scene.registry().has<RigidbodyComponent>(s.playerId)) {
      auto &rb = s.scene.registry().get<RigidbodyComponent>(s.playerId);
      rb.lastPosition = pos;
    }
    return true;
  }

  if (cmd == "regen_terrain") {
    s.terrainSystem.applySettings(s.terrainSettings);
    s.terrainSystem.regenerate();
    return true;
  }

  if (cmd == "play") {
    s.playState = AppState::PlayState::Playing;
    return false;
  }
  if (cmd == "audio_test_footstep") {
    s.pending.requestTestFootstepAudio = true;
    return false;
  }
  if (cmd == "pause") {
    s.playState = AppState::PlayState::Paused;
    return false;
  }
  if (cmd == "stop") {
    s.playState = AppState::PlayState::Stopped;
    return false;
  }

  LOG_WARN("Console", "Unknown command: " + cmd);
  return false;
}

json vec3ToJson(const glm::vec3 &v) { return {v.x, v.y, v.z}; }

void loadVec3(const json &j, const char *key, glm::vec3 &out) {
  if (!j.contains(key) || !j[key].is_array() || j[key].size() != 3)
    return;
  out = glm::vec3(j[key][0].get<float>(), j[key][1].get<float>(),
                  j[key][2].get<float>());
}

json serializeSunSettings(const SunFX &s) {
  json j;
  j["sunDir"] = vec3ToJson(s.sunDir);
  j["sunColor"] = vec3ToJson(s.sunColor);
  j["sunSize"] = s.sunSize;
  j["sunAzimuth"] = s.sunAzimuth;
  j["sunElevation"] = s.sunElevation;
  j["lightIntensity"] = s.lightIntensity;
  j["ambientStrength"] = s.ambientStrength;
  j["glowStrength"] = s.glowStrength;
  return j;
}

void applySunSettings(const json &j, SunFX &s) {
  loadVec3(j, "sunDir", s.sunDir);
  loadVec3(j, "sunColor", s.sunColor);
  if (j.contains("sunSize"))
    s.sunSize = j["sunSize"].get<float>();
  if (j.contains("sunAzimuth"))
    s.sunAzimuth = j["sunAzimuth"].get<float>();
  if (j.contains("sunElevation"))
    s.sunElevation = j["sunElevation"].get<float>();
  if (j.contains("lightIntensity"))
    s.lightIntensity = j["lightIntensity"].get<float>();
  if (j.contains("ambientStrength"))
    s.ambientStrength = j["ambientStrength"].get<float>();
  if (j.contains("glowStrength"))
    s.glowStrength = j["glowStrength"].get<float>();
}

json serializeSkySettings(const SkySettings &s) {
  json j;
  j["solidSky"] = s.solidSky;
  j["skyHorizon"] = {s.skyHorizon[0], s.skyHorizon[1], s.skyHorizon[2]};
  j["skyTop"] = {s.skyTop[0], s.skyTop[1], s.skyTop[2]};
  j["dayNightEnabled"] = s.dayNightEnabled;
  j["timeOfDay"] = s.timeOfDay;
  j["cycleSpeed"] = s.cycleSpeed;
  j["dayHorizon"] = {s.dayHorizon[0], s.dayHorizon[1], s.dayHorizon[2]};
  j["dayTop"] = {s.dayTop[0], s.dayTop[1], s.dayTop[2]};
  j["nightHorizon"] = {s.nightHorizon[0], s.nightHorizon[1], s.nightHorizon[2]};
  j["nightTop"] = {s.nightTop[0], s.nightTop[1], s.nightTop[2]};
  j["sunDayColor"] = vec3ToJson(s.sunDayColor);
  j["sunDuskColor"] = vec3ToJson(s.sunDuskColor);
  j["sunNightColor"] = vec3ToJson(s.sunNightColor);
  j["minimalSky"] = s.minimalSky;
  j["skyBackdropBlend"] = s.skyBackdropBlend;
  j["skyFeatureVisibility"] = s.skyFeatureVisibility;
  j["firefliesEnabled"] = s.firefliesEnabled;
  j["fireflyCount"] = s.fireflyCount;
  j["fireflyRadius"] = s.fireflyRadius;
  j["fireflyHeightMin"] = s.fireflyHeightMin;
  j["fireflyHeightMax"] = s.fireflyHeightMax;
  j["fireflySize"] = s.fireflySize;
  j["fireflyIntensity"] = s.fireflyIntensity;
  j["fireflyColor"] = vec3ToJson(s.fireflyColor);
  return j;
}

void applySkySettings(const json &j, SkySettings &s) {
  if (j.contains("solidSky"))
    s.solidSky = j["solidSky"].get<bool>();
  if (j.contains("skyHorizon") && j["skyHorizon"].is_array() &&
      j["skyHorizon"].size() == 3) {
    s.skyHorizon[0] = j["skyHorizon"][0].get<float>();
    s.skyHorizon[1] = j["skyHorizon"][1].get<float>();
    s.skyHorizon[2] = j["skyHorizon"][2].get<float>();
  }
  if (j.contains("skyTop") && j["skyTop"].is_array() &&
      j["skyTop"].size() == 3) {
    s.skyTop[0] = j["skyTop"][0].get<float>();
    s.skyTop[1] = j["skyTop"][1].get<float>();
    s.skyTop[2] = j["skyTop"][2].get<float>();
  }
  if (j.contains("dayNightEnabled"))
    s.dayNightEnabled = j["dayNightEnabled"].get<bool>();
  if (j.contains("timeOfDay"))
    s.timeOfDay = j["timeOfDay"].get<float>();
  if (j.contains("cycleSpeed"))
    s.cycleSpeed = j["cycleSpeed"].get<float>();
  if (j.contains("dayHorizon") && j["dayHorizon"].is_array() &&
      j["dayHorizon"].size() == 3) {
    s.dayHorizon[0] = j["dayHorizon"][0].get<float>();
    s.dayHorizon[1] = j["dayHorizon"][1].get<float>();
    s.dayHorizon[2] = j["dayHorizon"][2].get<float>();
  }
  if (j.contains("dayTop") && j["dayTop"].is_array() &&
      j["dayTop"].size() == 3) {
    s.dayTop[0] = j["dayTop"][0].get<float>();
    s.dayTop[1] = j["dayTop"][1].get<float>();
    s.dayTop[2] = j["dayTop"][2].get<float>();
  }
  if (j.contains("nightHorizon") && j["nightHorizon"].is_array() &&
      j["nightHorizon"].size() == 3) {
    s.nightHorizon[0] = j["nightHorizon"][0].get<float>();
    s.nightHorizon[1] = j["nightHorizon"][1].get<float>();
    s.nightHorizon[2] = j["nightHorizon"][2].get<float>();
  }
  if (j.contains("nightTop") && j["nightTop"].is_array() &&
      j["nightTop"].size() == 3) {
    s.nightTop[0] = j["nightTop"][0].get<float>();
    s.nightTop[1] = j["nightTop"][1].get<float>();
    s.nightTop[2] = j["nightTop"][2].get<float>();
  }
  loadVec3(j, "sunDayColor", s.sunDayColor);
  loadVec3(j, "sunDuskColor", s.sunDuskColor);
  loadVec3(j, "sunNightColor", s.sunNightColor);
  if (j.contains("minimalSky"))
    s.minimalSky = j["minimalSky"].get<bool>();
  if (j.contains("skyBackdropBlend"))
    s.skyBackdropBlend = j["skyBackdropBlend"].get<float>();
  if (j.contains("skyFeatureVisibility"))
    s.skyFeatureVisibility = j["skyFeatureVisibility"].get<float>();
  if (j.contains("firefliesEnabled"))
    s.firefliesEnabled = j["firefliesEnabled"].get<bool>();
  if (j.contains("fireflyCount"))
    s.fireflyCount = j["fireflyCount"].get<int>();
  if (j.contains("fireflyRadius"))
    s.fireflyRadius = j["fireflyRadius"].get<float>();
  if (j.contains("fireflyHeightMin"))
    s.fireflyHeightMin = j["fireflyHeightMin"].get<float>();
  if (j.contains("fireflyHeightMax"))
    s.fireflyHeightMax = j["fireflyHeightMax"].get<float>();
  if (j.contains("fireflySize"))
    s.fireflySize = j["fireflySize"].get<float>();
  if (j.contains("fireflyIntensity"))
    s.fireflyIntensity = j["fireflyIntensity"].get<float>();
  loadVec3(j, "fireflyColor", s.fireflyColor);
}

json serializeTerrainSettings(const TerrainSettings &s) {
  json j;
  j["enabled"] = s.enabled;
  j["seed"] = s.seed;
  j["chunkSize"] = s.chunkSize;
  j["heightScale"] = s.heightScale;
  j["noiseFrequency"] = s.noiseFrequency;
  j["viewDistance"] = s.viewDistance;
  j["chunkWorldSize"] = s.chunkWorldSize;
  j["useRidgeNoise"] = s.useRidgeNoise;
  j["singleBiomeOnly"] = s.singleBiomeOnly;
  j["octaves"] = s.octaves;
  j["lacunarity"] = s.lacunarity;
  j["gain"] = s.gain;
  j["treeDensity"] = s.treeDensity;
  j["biomeScale"] = s.biomeScale;
  j["seaLevel"] = s.seaLevel;
  j["rockDensity"] = s.rockDensity;
  j["grassDensity"] = s.grassDensity;
  j["rockScale"] = s.rockScale;
  j["grassScale"] = s.grassScale;
  j["spawnWater"] = s.spawnWater;
  j["spawnRocks"] = s.spawnRocks;
  j["spawnVegetation"] = s.spawnVegetation;
  j["customTreeModelPath"] = s.customTreeModelPath;
  j["customRockModelPath"] = s.customRockModelPath;
  j["customGrassModelPath"] = s.customGrassModelPath;
  j["flipCustomGrass"] = s.flipCustomGrass;
  j["customFlowerModelPath"] = s.customFlowerModelPath;
  j["customCactusModelPath"] = s.customCactusModelPath;
  j["customDeadTreeModelPath"] = s.customDeadTreeModelPath;
  return j;
}

void applyTerrainSettings(const json &j, TerrainSettings &s) {
  if (j.contains("enabled"))
    s.enabled = j["enabled"].get<bool>();
  if (j.contains("seed"))
    s.seed = j["seed"].get<uint32_t>();
  if (j.contains("chunkSize"))
    s.chunkSize = j["chunkSize"].get<int>();
  if (j.contains("heightScale"))
    s.heightScale = j["heightScale"].get<float>();
  if (j.contains("noiseFrequency"))
    s.noiseFrequency = j["noiseFrequency"].get<float>();
  if (j.contains("viewDistance"))
    s.viewDistance = j["viewDistance"].get<int>();
  if (j.contains("chunkWorldSize"))
    s.chunkWorldSize = j["chunkWorldSize"].get<float>();
  if (j.contains("useRidgeNoise"))
    s.useRidgeNoise = j["useRidgeNoise"].get<bool>();
  if (j.contains("singleBiomeOnly"))
    s.singleBiomeOnly = j["singleBiomeOnly"].get<bool>();
  if (j.contains("octaves"))
    s.octaves = j["octaves"].get<int>();
  if (j.contains("lacunarity"))
    s.lacunarity = j["lacunarity"].get<float>();
  if (j.contains("gain"))
    s.gain = j["gain"].get<float>();
  if (j.contains("treeDensity"))
    s.treeDensity = j["treeDensity"].get<float>();
  if (j.contains("biomeScale"))
    s.biomeScale = j["biomeScale"].get<float>();
  if (j.contains("seaLevel"))
    s.seaLevel = j["seaLevel"].get<float>();
  if (j.contains("rockDensity"))
    s.rockDensity = j["rockDensity"].get<float>();
  if (j.contains("grassDensity"))
    s.grassDensity = j["grassDensity"].get<float>();
  if (j.contains("rockScale"))
    s.rockScale = j["rockScale"].get<float>();
  if (j.contains("grassScale"))
    s.grassScale = j["grassScale"].get<float>();
  if (j.contains("spawnWater"))
    s.spawnWater = j["spawnWater"].get<bool>();
  if (j.contains("spawnRocks"))
    s.spawnRocks = j["spawnRocks"].get<bool>();
  if (j.contains("spawnVegetation"))
    s.spawnVegetation = j["spawnVegetation"].get<bool>();
  if (j.contains("customTreeModelPath"))
    s.customTreeModelPath = j["customTreeModelPath"].get<std::string>();
  if (j.contains("customRockModelPath"))
    s.customRockModelPath = j["customRockModelPath"].get<std::string>();
  if (j.contains("customGrassModelPath"))
    s.customGrassModelPath = j["customGrassModelPath"].get<std::string>();
  if (j.contains("flipCustomGrass"))
    s.flipCustomGrass = j["flipCustomGrass"].get<bool>();
  if (j.contains("customFlowerModelPath"))
    s.customFlowerModelPath = j["customFlowerModelPath"].get<std::string>();
  if (j.contains("customCactusModelPath"))
    s.customCactusModelPath = j["customCactusModelPath"].get<std::string>();
  if (j.contains("customDeadTreeModelPath"))
    s.customDeadTreeModelPath = j["customDeadTreeModelPath"].get<std::string>();
}

json serializeTerrainMaterial(const TerrainMaterialSettings &s) {
  json j;
  j["enableCustom"] = s.enableCustom;
  j["macroScale"] = s.macroScale;
  j["detailScale"] = s.detailScale;
  j["normalDetailScale"] = s.normalDetailScale;
  j["normalStrength"] = s.normalStrength;
  j["cliffStart"] = s.cliffStart;
  j["cliffEnd"] = s.cliffEnd;
  j["snowStartHeight"] = s.snowStartHeight;
  j["snowEndHeight"] = s.snowEndHeight;
  j["lowStartHeight"] = s.lowStartHeight;
  j["lowEndHeight"] = s.lowEndHeight;
  j["macroVariationStrength"] = s.macroVariationStrength;
  j["cliffDesatStrength"] = s.cliffDesatStrength;
  j["grassA"] = vec3ToJson(s.grassA);
  j["grassB"] = vec3ToJson(s.grassB);
  j["dirtA"] = vec3ToJson(s.dirtA);
  j["dirtB"] = vec3ToJson(s.dirtB);
  j["rockA"] = vec3ToJson(s.rockA);
  j["rockB"] = vec3ToJson(s.rockB);
  j["sandA"] = vec3ToJson(s.sandA);
  j["sandB"] = vec3ToJson(s.sandB);
  j["snowA"] = vec3ToJson(s.snowA);
  j["snowB"] = vec3ToJson(s.snowB);
  j["roughGrass"] = s.roughGrass;
  j["roughDirt"] = s.roughDirt;
  j["roughRock"] = s.roughRock;
  j["roughSand"] = s.roughSand;
  j["roughSnow"] = s.roughSnow;
  j["useGroundTextures"] = s.useGroundTextures;
  j["groundAlbedoPath"] = s.groundAlbedoPath;
  j["groundNormalPath"] = s.groundNormalPath;
  j["groundRoughnessPath"] = s.groundRoughnessPath;
  j["groundTiling"] = s.groundTiling;
  j["groundBlendStrength"] = s.groundBlendStrength;
  j["groundRoughness"] = s.groundRoughness;
  j["flatGreenEnabled"] = s.flatGreenEnabled;
  j["flatGreenColor"] = vec3ToJson(s.flatGreenColor);
  return j;
}

void applyTerrainMaterial(const json &j, TerrainMaterialSettings &s) {
  if (j.contains("enableCustom"))
    s.enableCustom = j["enableCustom"].get<bool>();
  if (j.contains("macroScale"))
    s.macroScale = j["macroScale"].get<float>();
  if (j.contains("detailScale"))
    s.detailScale = j["detailScale"].get<float>();
  if (j.contains("normalDetailScale"))
    s.normalDetailScale = j["normalDetailScale"].get<float>();
  if (j.contains("normalStrength"))
    s.normalStrength = j["normalStrength"].get<float>();
  if (j.contains("cliffStart"))
    s.cliffStart = j["cliffStart"].get<float>();
  if (j.contains("cliffEnd"))
    s.cliffEnd = j["cliffEnd"].get<float>();
  if (j.contains("snowStartHeight"))
    s.snowStartHeight = j["snowStartHeight"].get<float>();
  if (j.contains("snowEndHeight"))
    s.snowEndHeight = j["snowEndHeight"].get<float>();
  if (j.contains("lowStartHeight"))
    s.lowStartHeight = j["lowStartHeight"].get<float>();
  if (j.contains("lowEndHeight"))
    s.lowEndHeight = j["lowEndHeight"].get<float>();
  if (j.contains("macroVariationStrength"))
    s.macroVariationStrength = j["macroVariationStrength"].get<float>();
  if (j.contains("cliffDesatStrength"))
    s.cliffDesatStrength = j["cliffDesatStrength"].get<float>();
  loadVec3(j, "grassA", s.grassA);
  loadVec3(j, "grassB", s.grassB);
  loadVec3(j, "dirtA", s.dirtA);
  loadVec3(j, "dirtB", s.dirtB);
  loadVec3(j, "rockA", s.rockA);
  loadVec3(j, "rockB", s.rockB);
  loadVec3(j, "sandA", s.sandA);
  loadVec3(j, "sandB", s.sandB);
  loadVec3(j, "snowA", s.snowA);
  loadVec3(j, "snowB", s.snowB);
  if (j.contains("roughGrass"))
    s.roughGrass = j["roughGrass"].get<float>();
  if (j.contains("roughDirt"))
    s.roughDirt = j["roughDirt"].get<float>();
  if (j.contains("roughRock"))
    s.roughRock = j["roughRock"].get<float>();
  if (j.contains("roughSand"))
    s.roughSand = j["roughSand"].get<float>();
  if (j.contains("roughSnow"))
    s.roughSnow = j["roughSnow"].get<float>();
  if (j.contains("useGroundTextures"))
    s.useGroundTextures = j["useGroundTextures"].get<bool>();
  if (j.contains("groundAlbedoPath"))
    s.groundAlbedoPath = j["groundAlbedoPath"].get<std::string>();
  if (j.contains("groundNormalPath"))
    s.groundNormalPath = j["groundNormalPath"].get<std::string>();
  if (j.contains("groundRoughnessPath"))
    s.groundRoughnessPath = j["groundRoughnessPath"].get<std::string>();
  if (j.contains("groundTiling"))
    s.groundTiling = j["groundTiling"].get<float>();
  if (j.contains("groundBlendStrength"))
    s.groundBlendStrength = j["groundBlendStrength"].get<float>();
  if (j.contains("groundRoughness"))
    s.groundRoughness = j["groundRoughness"].get<float>();
  if (j.contains("flatGreenEnabled"))
    s.flatGreenEnabled = j["flatGreenEnabled"].get<bool>();
  loadVec3(j, "flatGreenColor", s.flatGreenColor);
}

json serializeRenderSettings(const RenderSettings &s) {
  json j;
  j["mixVal"] = s.mixVal;
  j["shadowStrength"] = s.shadowStrength;
  j["shadowFarPlane"] = s.shadowFarPlane;
  j["shadowUpdateInterval"] = s.shadowUpdateInterval;
  j["shadowUpdateDistance"] = s.shadowUpdateDistance;
  j["shadowUpdateAngle"] = s.shadowUpdateAngle;
  j["exposure"] = s.exposure;
  j["gamma"] = s.gamma;
  j["fogDensity"] = s.fogDensity;
  j["fogColor"] = vec3ToJson(s.fogColor);
  j["toonEnabled"] = s.toonEnabled;
  j["toonSteps"] = s.toonSteps;
  j["toonMin"] = s.toonMin;
  j["shadowBandEnabled"] = s.shadowBandEnabled;
  j["shadowBandSteps"] = s.shadowBandSteps;
  j["shadowBandSoftness"] = s.shadowBandSoftness;
  j["ambientRampEnabled"] = s.ambientRampEnabled;
  j["ambientRampStrength"] = s.ambientRampStrength;
  j["ambientRampTop"] = vec3ToJson(s.ambientRampTop);
  j["ambientRampBottom"] = vec3ToJson(s.ambientRampBottom);
  j["rimEnabled"] = s.rimEnabled;
  j["rimPower"] = s.rimPower;
  j["rimStrength"] = s.rimStrength;
  j["rimColor"] = vec3ToJson(s.rimColor);
  j["wireframe"] = s.wireframe;
  j["disableShadows"] = s.disableShadows;
  j["disableClouds"] = s.disableClouds;
  j["disableHDR"] = s.disableHDR;
  j["freezeTime"] = s.freezeTime;
  j["frozenTime"] = s.frozenTime;
  j["frustumCulling"] = s.frustumCulling;
  j["shadowCameraCulling"] = s.shadowCameraCulling;
  return j;
}

void applyRenderSettings(const json &j, RenderSettings &s) {
  if (j.contains("mixVal"))
    s.mixVal = j["mixVal"].get<float>();
  if (j.contains("shadowStrength"))
    s.shadowStrength = j["shadowStrength"].get<float>();
  if (j.contains("shadowFarPlane"))
    s.shadowFarPlane = j["shadowFarPlane"].get<float>();
  if (j.contains("shadowUpdateInterval"))
    s.shadowUpdateInterval = j["shadowUpdateInterval"].get<int>();
  if (j.contains("shadowUpdateDistance"))
    s.shadowUpdateDistance = j["shadowUpdateDistance"].get<float>();
  if (j.contains("shadowUpdateAngle"))
    s.shadowUpdateAngle = j["shadowUpdateAngle"].get<float>();
  if (j.contains("exposure"))
    s.exposure = j["exposure"].get<float>();
  if (j.contains("gamma"))
    s.gamma = j["gamma"].get<float>();
  if (j.contains("fogDensity"))
    s.fogDensity = j["fogDensity"].get<float>();
  loadVec3(j, "fogColor", s.fogColor);
  if (j.contains("toonEnabled"))
    s.toonEnabled = j["toonEnabled"].get<bool>();
  if (j.contains("toonSteps"))
    s.toonSteps = j["toonSteps"].get<int>();
  if (j.contains("toonMin"))
    s.toonMin = j["toonMin"].get<float>();
  if (j.contains("shadowBandEnabled"))
    s.shadowBandEnabled = j["shadowBandEnabled"].get<bool>();
  if (j.contains("shadowBandSteps"))
    s.shadowBandSteps = j["shadowBandSteps"].get<int>();
  if (j.contains("shadowBandSoftness"))
    s.shadowBandSoftness = j["shadowBandSoftness"].get<float>();
  if (j.contains("ambientRampEnabled"))
    s.ambientRampEnabled = j["ambientRampEnabled"].get<bool>();
  if (j.contains("ambientRampStrength"))
    s.ambientRampStrength = j["ambientRampStrength"].get<float>();
  loadVec3(j, "ambientRampTop", s.ambientRampTop);
  loadVec3(j, "ambientRampBottom", s.ambientRampBottom);
  if (j.contains("rimEnabled"))
    s.rimEnabled = j["rimEnabled"].get<bool>();
  if (j.contains("rimPower"))
    s.rimPower = j["rimPower"].get<float>();
  if (j.contains("rimStrength"))
    s.rimStrength = j["rimStrength"].get<float>();
  loadVec3(j, "rimColor", s.rimColor);
  if (j.contains("wireframe"))
    s.wireframe = j["wireframe"].get<bool>();
  if (j.contains("disableShadows"))
    s.disableShadows = j["disableShadows"].get<bool>();
  if (j.contains("disableClouds"))
    s.disableClouds = j["disableClouds"].get<bool>();
  if (j.contains("disableHDR"))
    s.disableHDR = j["disableHDR"].get<bool>();
  if (j.contains("freezeTime"))
    s.freezeTime = j["freezeTime"].get<bool>();
  if (j.contains("frozenTime"))
    s.frozenTime = j["frozenTime"].get<float>();
  if (j.contains("frustumCulling"))
    s.frustumCulling = j["frustumCulling"].get<bool>();
  if (j.contains("shadowCameraCulling"))
    s.shadowCameraCulling = j["shadowCameraCulling"].get<bool>();
}

json serializeAudioSettings(const AudioSettings &s) {
  json j;
  j["enabled"] = s.enabled;
  j["mute"] = s.mute;
  j["masterVolume"] = s.masterVolume;
  j["ambientEnabled"] = s.ambientEnabled;
  j["ambientPath"] = s.ambientPath;
  j["ambientVolume"] = s.ambientVolume;
  j["footstepsEnabled"] = s.footstepsEnabled;
  j["footstepPath"] = s.footstepPath;
  j["footstepVolume"] = s.footstepVolume;
  j["footstepWalkCadence"] = s.footstepWalkCadence;
  j["footstepRunCadence"] = s.footstepRunCadence;
  return j;
}

void applyAudioSettings(const json &j, AudioSettings &s) {
  if (j.contains("enabled"))
    s.enabled = j["enabled"].get<bool>();
  if (j.contains("mute"))
    s.mute = j["mute"].get<bool>();
  if (j.contains("masterVolume"))
    s.masterVolume = j["masterVolume"].get<float>();
  if (j.contains("ambientEnabled"))
    s.ambientEnabled = j["ambientEnabled"].get<bool>();
  if (j.contains("ambientPath"))
    s.ambientPath = j["ambientPath"].get<std::string>();
  if (j.contains("ambientVolume"))
    s.ambientVolume = j["ambientVolume"].get<float>();
  if (j.contains("footstepsEnabled"))
    s.footstepsEnabled = j["footstepsEnabled"].get<bool>();
  if (j.contains("footstepPath"))
    s.footstepPath = j["footstepPath"].get<std::string>();
  if (j.contains("footstepVolume"))
    s.footstepVolume = j["footstepVolume"].get<float>();
  if (j.contains("footstepWalkCadence"))
    s.footstepWalkCadence = j["footstepWalkCadence"].get<float>();
  if (j.contains("footstepRunCadence"))
    s.footstepRunCadence = j["footstepRunCadence"].get<float>();
}

json serializePostProcess(const PostProcessor &s) {
  json j;
  j["bloomThreshold"] = s.bloomThreshold;
  j["blurIterations"] = s.blurIterations;
  j["bloomScale"] = s.bloomScale;
  j["bloomIntensity"] = s.bloomIntensity;
  j["brightness"] = s.brightness;
  j["enableSSAO"] = s.enableSSAO;
  j["ssaoQuality"] = s.ssaoQuality;
  j["ssaoRadius"] = s.ssaoRadius;
  j["ssaoBias"] = s.ssaoBias;
  j["ssaoPower"] = s.ssaoPower;
  j["ssaoSamples"] = s.ssaoSamples;
  j["ssaoScale"] = s.ssaoScale;
  j["ssaoScaleRadius"] = s.ssaoScaleRadius;
  j["ssaoFullResTerrain"] = s.ssaoFullResTerrain;
  j["enableVolumetricFog"] = s.enableVolumetricFog;
  j["volumetricQuality"] = s.volumetricQuality;
  j["volumetricFogDensity"] = s.volumetricFogDensity;
  j["volumetricLightExposure"] = s.volumetricLightExposure;
  j["volumetricLightDecay"] = s.volumetricLightDecay;
  j["volumetricLightWeight"] = s.volumetricLightWeight;
  j["volumetricSamples"] = s.volumetricSamples;
  j["volumetricScale"] = s.volumetricScale;
  j["enableTAA"] = s.enableTAA;
  j["taaHistoryBlend"] = s.taaHistoryBlend;
  j["taaJitterScale"] = s.taaJitterScale;
  j["taaMotionReset"] = s.taaMotionReset;
  j["enableFXAA"] = s.enableFXAA;
  j["fxaaSpanMax"] = s.fxaaSpanMax;
  j["fxaaReduceMin"] = s.fxaaReduceMin;
  j["fxaaReduceMul"] = s.fxaaReduceMul;
  j["enableOutline"] = s.enableOutline;
  j["outlineStrength"] = s.outlineStrength;
  j["outlineThreshold"] = s.outlineThreshold;
  j["outlineThickness"] = s.outlineThickness;
  j["outlineColor"] = vec3ToJson(s.outlineColor);
  j["enableDistanceTint"] = s.enableDistanceTint;
  j["distanceTintStart"] = s.distanceTintStart;
  j["distanceTintEnd"] = s.distanceTintEnd;
  j["distanceTintColor"] = vec3ToJson(s.distanceTintColor);
  j["enableColorGrade"] = s.enableColorGrade;
  j["gradeSaturation"] = s.gradeSaturation;
  j["gradeContrast"] = s.gradeContrast;
  j["gradeLift"] = s.gradeLift;
  j["gradeGamma"] = s.gradeGamma;
  j["gradeGain"] = s.gradeGain;
  j["gradeTint"] = vec3ToJson(s.gradeTint);
  j["enablePaletteQuantize"] = s.enablePaletteQuantize;
  j["paletteSteps"] = s.paletteSteps;
  return j;
}

void applyPostProcess(const json &j, PostProcessor &s) {
  if (j.contains("bloomThreshold"))
    s.bloomThreshold = j["bloomThreshold"].get<float>();
  if (j.contains("blurIterations"))
    s.blurIterations = j["blurIterations"].get<int>();
  if (j.contains("bloomScale"))
    s.bloomScale = j["bloomScale"].get<float>();
  if (j.contains("bloomIntensity"))
    s.bloomIntensity = j["bloomIntensity"].get<float>();
  if (j.contains("brightness"))
    s.brightness = j["brightness"].get<float>();
  if (j.contains("enableSSAO"))
    s.enableSSAO = j["enableSSAO"].get<bool>();
  if (j.contains("ssaoQuality"))
    s.ssaoQuality = j["ssaoQuality"].get<int>();
  if (j.contains("ssaoRadius"))
    s.ssaoRadius = j["ssaoRadius"].get<float>();
  if (j.contains("ssaoBias"))
    s.ssaoBias = j["ssaoBias"].get<float>();
  if (j.contains("ssaoPower"))
    s.ssaoPower = j["ssaoPower"].get<float>();
  if (j.contains("ssaoSamples"))
    s.ssaoSamples = j["ssaoSamples"].get<int>();
  if (j.contains("ssaoScale"))
    s.ssaoScale = j["ssaoScale"].get<float>();
  if (j.contains("ssaoScaleRadius"))
    s.ssaoScaleRadius = j["ssaoScaleRadius"].get<bool>();
  if (j.contains("ssaoFullResTerrain"))
    s.ssaoFullResTerrain = j["ssaoFullResTerrain"].get<bool>();
  if (j.contains("enableVolumetricFog"))
    s.enableVolumetricFog = j["enableVolumetricFog"].get<bool>();
  if (j.contains("volumetricQuality"))
    s.volumetricQuality = j["volumetricQuality"].get<int>();
  if (j.contains("volumetricFogDensity"))
    s.volumetricFogDensity = j["volumetricFogDensity"].get<float>();
  if (j.contains("volumetricLightExposure"))
    s.volumetricLightExposure = j["volumetricLightExposure"].get<float>();
  if (j.contains("volumetricLightDecay"))
    s.volumetricLightDecay = j["volumetricLightDecay"].get<float>();
  if (j.contains("volumetricLightWeight"))
    s.volumetricLightWeight = j["volumetricLightWeight"].get<float>();
  if (j.contains("volumetricSamples"))
    s.volumetricSamples = j["volumetricSamples"].get<int>();
  if (j.contains("volumetricScale"))
    s.volumetricScale = j["volumetricScale"].get<float>();
  if (j.contains("enableTAA"))
    s.enableTAA = j["enableTAA"].get<bool>();
  if (j.contains("taaHistoryBlend"))
    s.taaHistoryBlend = j["taaHistoryBlend"].get<float>();
  if (j.contains("taaJitterScale"))
    s.taaJitterScale = j["taaJitterScale"].get<float>();
  if (j.contains("taaMotionReset"))
    s.taaMotionReset = j["taaMotionReset"].get<float>();
  if (j.contains("enableFXAA"))
    s.enableFXAA = j["enableFXAA"].get<bool>();
  if (j.contains("fxaaSpanMax"))
    s.fxaaSpanMax = j["fxaaSpanMax"].get<float>();
  if (j.contains("fxaaReduceMin"))
    s.fxaaReduceMin = j["fxaaReduceMin"].get<float>();
  if (j.contains("fxaaReduceMul"))
    s.fxaaReduceMul = j["fxaaReduceMul"].get<float>();
  if (j.contains("enableOutline"))
    s.enableOutline = j["enableOutline"].get<bool>();
  if (j.contains("outlineStrength"))
    s.outlineStrength = j["outlineStrength"].get<float>();
  if (j.contains("outlineThreshold"))
    s.outlineThreshold = j["outlineThreshold"].get<float>();
  if (j.contains("outlineThickness"))
    s.outlineThickness = j["outlineThickness"].get<float>();
  loadVec3(j, "outlineColor", s.outlineColor);
  if (j.contains("enableDistanceTint"))
    s.enableDistanceTint = j["enableDistanceTint"].get<bool>();
  if (j.contains("distanceTintStart"))
    s.distanceTintStart = j["distanceTintStart"].get<float>();
  if (j.contains("distanceTintEnd"))
    s.distanceTintEnd = j["distanceTintEnd"].get<float>();
  loadVec3(j, "distanceTintColor", s.distanceTintColor);
  if (j.contains("enableColorGrade"))
    s.enableColorGrade = j["enableColorGrade"].get<bool>();
  if (j.contains("gradeSaturation"))
    s.gradeSaturation = j["gradeSaturation"].get<float>();
  if (j.contains("gradeContrast"))
    s.gradeContrast = j["gradeContrast"].get<float>();
  if (j.contains("gradeLift"))
    s.gradeLift = j["gradeLift"].get<float>();
  if (j.contains("gradeGamma"))
    s.gradeGamma = j["gradeGamma"].get<float>();
  if (j.contains("gradeGain"))
    s.gradeGain = j["gradeGain"].get<float>();
  loadVec3(j, "gradeTint", s.gradeTint);
  if (j.contains("enablePaletteQuantize"))
    s.enablePaletteQuantize = j["enablePaletteQuantize"].get<bool>();
  if (j.contains("paletteSteps"))
    s.paletteSteps = j["paletteSteps"].get<int>();
}

bool isEntityAlive(Registry &reg, EntityId e) {
  if (!reg.has<LifecycleComponent>(e))
    return true;
  return reg.get<LifecycleComponent>(e).state == EntityLifecycleState::Alive;
}

float terrainClearanceForEntity(Registry &reg, EntityId e) {
  float clearance = kTerrainMinClearance;
  if (!reg.has<ColliderComponent>(e))
    return clearance;

  const auto &coll = reg.get<ColliderComponent>(e);
  switch (coll.shape) {
  case ColliderComponent::Shape::Box:
    clearance = std::max(clearance, coll.dimensions.y * 0.5f + 0.05f);
    break;
  case ColliderComponent::Shape::Sphere:
    clearance = std::max(clearance, coll.dimensions.x + 0.05f);
    break;
  case ColliderComponent::Shape::Capsule:
    clearance =
        std::max(clearance, coll.dimensions.y * 0.5f + coll.dimensions.x + 0.05f);
    break;
  }
  return clearance;
}

float terrainSupportRadiusForEntity(Registry &reg, EntityId e) {
  if (!reg.has<ColliderComponent>(e))
    return 0.35f;

  const auto &coll = reg.get<ColliderComponent>(e);
  switch (coll.shape) {
  case ColliderComponent::Shape::Box:
    return std::max(0.25f, std::max(coll.dimensions.x, coll.dimensions.z) * 0.5f);
  case ColliderComponent::Shape::Sphere:
    return std::max(0.25f, coll.dimensions.x);
  case ColliderComponent::Shape::Capsule:
    return std::max(0.25f, coll.dimensions.x);
  }
  return 0.35f;
}

float terrainSupportHeightForEntity(AppState &state, Registry &reg, EntityId e,
                                    float x, float z) {
  float h = state.terrainSystem.getHeightAt(x, z);
  const float r = terrainSupportRadiusForEntity(reg, e);
  if (r <= 0.0f)
    return h;

  const float diag = r * 0.70710678f;
  h = std::max(h, state.terrainSystem.getHeightAt(x + r, z));
  h = std::max(h, state.terrainSystem.getHeightAt(x - r, z));
  h = std::max(h, state.terrainSystem.getHeightAt(x, z + r));
  h = std::max(h, state.terrainSystem.getHeightAt(x, z - r));
  h = std::max(h, state.terrainSystem.getHeightAt(x + diag, z + diag));
  h = std::max(h, state.terrainSystem.getHeightAt(x - diag, z + diag));
  h = std::max(h, state.terrainSystem.getHeightAt(x + diag, z - diag));
  h = std::max(h, state.terrainSystem.getHeightAt(x - diag, z - diag));
  return h;
}

void clampEntityToTerrain(AppState &state, Registry &reg, EntityId e) {
  if (!reg.has<TransformComponent>(e) || !isEntityAlive(reg, e))
    return;

  auto &tr = reg.get<TransformComponent>(e);
  const glm::vec2 clampedXZ = state.terrainSystem.clampXZToLoadedRegion(
      tr.position.x, tr.position.z, kTerrainEdgeMargin);
  tr.position.x = clampedXZ.x;
  tr.position.z = clampedXZ.y;

  const float terrainY =
      terrainSupportHeightForEntity(state, reg, e, tr.position.x, tr.position.z);
  const float minY = terrainY + terrainClearanceForEntity(reg, e);
  if (tr.position.y < minY)
    tr.position.y = minY;
}
} // namespace

bool CoreAppLayer::initialize() {
  mState.events.subscribe<SaveConfigRequestedEvent>(
      [this](const SaveConfigRequestedEvent &) {
        mState.pending.requestSaveConfig = true;
      });
  mState.events.subscribe<LoadConfigRequestedEvent>(
      [this](const LoadConfigRequestedEvent &) {
        mState.pending.requestLoadConfig = true;
      });
  mState.events.subscribe<SaveProjectConfigRequestedEvent>(
      [this](const SaveProjectConfigRequestedEvent &) {
        mState.pending.requestSaveProjectConfig = true;
      });
  mState.events.subscribe<SpawnEntityRequestedEvent>(
      [this](const SpawnEntityRequestedEvent &e) {
        mState.pending.pendingSpawnPaths.push_back(e.path);
      });
  mState.events.subscribe<CreateEmptyEntityRequestedEvent>(
      [this](const CreateEmptyEntityRequestedEvent &e) {
        mState.pending.pendingEmptyEntityNames.push_back(e.name);
      });
  mState.events.subscribe<DeleteEntityRequestedEvent>(
      [this](const DeleteEntityRequestedEvent &e) {
        mState.pending.pendingDeleteEntityIds.push_back(e.entityId);
      });
  mState.events.subscribe<SaveSceneRequestedEvent>(
      [this](const SaveSceneRequestedEvent &e) {
        mState.pending.pendingSceneSavePath = e.path;
      });
  mState.events.subscribe<LoadSceneRequestedEvent>(
      [this](const LoadSceneRequestedEvent &e) {
        mState.pending.pendingSceneLoadPath = e.path;
      });
  mState.events.subscribe<UndoRequestedEvent>(
      [this](const UndoRequestedEvent &) {
        mState.history.requestUndo = true;
      });
  mState.events.subscribe<RedoRequestedEvent>(
      [this](const RedoRequestedEvent &) {
        mState.history.requestRedo = true;
      });
  mState.events.subscribe<SceneHistoryJumpRequestedEvent>(
      [this](const SceneHistoryJumpRequestedEvent &e) {
        mState.history.requestHistoryJump = e.index;
      });

  commitHistorySnapshot("Initial");

  return true;
}

void CoreAppLayer::shutdown() {}

void CoreAppLayer::applyHistorySnapshot(int idx) {
  if (idx < 0 || idx >= (int)mState.history.historySnapshots.size())
    return;
  if (!mState.scene.loadFromString(mState.history.historySnapshots[idx]))
    return;
  mState.history.historyCursor = idx;
  mState.selection.selectedEntityId = 0;
  mState.selection.selectedEntities.clear();
  mState.selection.lastClickedEntity = 0;
  mState.playerId = 0;
  for (auto e : mState.scene.registry().view<CameraComponent>()) {
    if (!mState.scene.registry().has<LifecycleComponent>(e) ||
        mState.scene.registry().get<LifecycleComponent>(e).state ==
            EntityLifecycleState::Alive) {
      mState.playerId = e;
      break;
    }
  }
}

void CoreAppLayer::commitHistorySnapshot(const std::string &label) {
  const std::string snap = mState.scene.serializeToString();
  if (mState.history.historyCursor >= 0 &&
      mState.history.historyCursor <
          (int)mState.history.historySnapshots.size() &&
      mState.history.historySnapshots[mState.history.historyCursor] == snap)
    return;

  if (mState.history.historyCursor + 1 <
      (int)mState.history.historySnapshots.size()) {
    mState.history.historySnapshots.erase(
        mState.history.historySnapshots.begin() + mState.history.historyCursor +
            1,
        mState.history.historySnapshots.end());
    mState.history.historyLabels.erase(mState.history.historyLabels.begin() +
                                           mState.history.historyCursor + 1,
                                       mState.history.historyLabels.end());
  }

  mState.history.historySnapshots.push_back(snap);
  mState.history.historyLabels.push_back(label);
  mState.history.historyCursor =
      (int)mState.history.historySnapshots.size() - 1;

  const int maxHistory = 128;
  if ((int)mState.history.historySnapshots.size() > maxHistory) {
    const int trim = (int)mState.history.historySnapshots.size() - maxHistory;
    mState.history.historySnapshots.erase(
        mState.history.historySnapshots.begin(),
        mState.history.historySnapshots.begin() + trim);
    mState.history.historyLabels.erase(mState.history.historyLabels.begin(),
                                       mState.history.historyLabels.begin() +
                                           trim);
    mState.history.historyCursor -= trim;
    if (mState.history.historyCursor < 0)
      mState.history.historyCursor = 0;
  }
}

void CoreAppLayer::update(float dt, float nowT) {
  // Always in editor mode — cursor always visible
  mState.uiMode = true;
  mState.profiler.beginFrame();
  mState.profiler.setFrameMs(dt * 1000.0f);
  GLStateCache::instance().resetCounters();

  if (mState.editorSubsystem)
    mState.editorSubsystem->beginFrame();

  // Tick the Laravel Network poll
  mState.networkSystem.update(dt, mState);

  float renderTime = mState.render.freezeTime ? mState.render.frozenTime : nowT;
  if (!mState.render.freezeTime)
    mState.render.frozenTime = nowT;
  if (mState.skyUI.dayNightEnabled && !mState.render.freezeTime &&
      mState.skyUI.cycleSpeed > 0.0f) {
    mState.skyUI.timeOfDay += (dt * mState.skyUI.cycleSpeed) / 60.0f;
    if (mState.skyUI.timeOfDay > 1.0f)
      mState.skyUI.timeOfDay -= 1.0f;
    if (mState.skyUI.timeOfDay < 0.0f)
      mState.skyUI.timeOfDay += 1.0f;
  }

  EditorSelectionState selState = {mState.selection.selectedEntityId,
                                   mState.selection.selectedEntities,
                                   mState.selection.lastClickedEntity,
                                   mState.selection.editObjPart,
                                   mState.selection.selectedObjPartName,
                                   mState.selection.editColliderBounds,
                                   (int &)mState.selection.gizmoOp,
                                   (int &)mState.selection.gizmoMode,
                                   mState.selection.renaming,
                                   mState.selection.renameBuf,
                                   mState.selection.outlinerFilter,
                                   mState.selection.focusDistance};

  EditorContext ctx{
      mState.uiMode,
      mState.input.walkStep,
      mState.input.runMult,
      mState.input.jumpStrength,
      mState.input.gravity,
      mState.input.freezePhysics,
      mState.input.mouseSensitivity,
      mState.input.fov,
      mState.sun,
      mState.fire,
      mState.cloud,
      mState.sky,
      mState.projectiles,
      mState.postProcessor,
      mState.scene,
      mState.events,
      mState.projectConfig,
      mState.assets,
      mState.terrainSize,
      mState.terrainSpacing,
      mState.terrainSettings,
      mState.terrainMaterial,
      mState.terrainSystem,
      mState.editorCamera,
      mState.terrainBrush.enabled,
      mState.terrainBrush.mode,
      mState.terrainBrush.target,
      mState.terrainBrush.radius,
      mState.terrainBrush.strength,
      mState.terrainBrush.scatterCount,
      mState.skyUI.solidSky,
      mState.skyUI.skyHorizon,
      mState.skyUI.skyTop,
      mState.skyUI.dayNightEnabled,
      mState.skyUI.timeOfDay,
      mState.skyUI.cycleSpeed,
      mState.skyUI.dayHorizon,
      mState.skyUI.dayTop,
      mState.skyUI.nightHorizon,
      mState.skyUI.nightTop,
      mState.skyUI.sunDayColor,
      mState.skyUI.sunDuskColor,
      mState.skyUI.sunNightColor,
      mState.skyUI.minimalSky,
      mState.skyUI.skyBackdropBlend,
      mState.skyUI.skyFeatureVisibility,
      mState.skyUI.firefliesEnabled,
      mState.skyUI.fireflyCount,
      mState.skyUI.fireflyRadius,
      mState.skyUI.fireflyHeightMin,
      mState.skyUI.fireflyHeightMax,
      mState.skyUI.fireflySize,
      mState.skyUI.fireflyIntensity,
      mState.skyUI.fireflyColor,
      mState.render.shadowStrength,
      mState.render.shadowFarPlane,
      mState.render.shadowUpdateInterval,
      mState.render.shadowUpdateDistance,
      mState.render.shadowUpdateAngle,
      mState.render.shadowCameraCulling,
      mState.render.exposure,
      mState.render.gamma,
      mState.render.fogDensity,
      mState.render.fogColor,
      mState.render.toonEnabled,
      mState.render.toonSteps,
      mState.render.toonMin,
      mState.render.shadowBandEnabled,
      mState.render.shadowBandSteps,
      mState.render.shadowBandSoftness,
      mState.render.ambientRampEnabled,
      mState.render.ambientRampStrength,
      mState.render.ambientRampTop,
      mState.render.ambientRampBottom,
      mState.render.rimEnabled,
      mState.render.rimPower,
      mState.render.rimStrength,
      mState.render.rimColor,
      mState.render.wireframe,
      mState.render.disableShadows,
      mState.render.disableClouds,
      mState.render.disableHDR,
      mState.render.freezeTime,
      mState.woodCount,
      (int &)mState.activeSlot,
      mState.axeEnabled,
      mState.axeOffset,
      mState.axeRotation,
      mState.axeScale,
      mState.torchEnabled,
      mState.torchOffset,
      mState.torchRotation,
      mState.torchScale,
      mState.usePlayerCameraInEdit,
      mState.audio.enabled,
      mState.audio.mute,
      mState.audio.masterVolume,
      mState.audio.ambientEnabled,
      mState.audio.ambientPath,
      mState.audio.ambientVolume,
      mState.audio.footstepsEnabled,
      mState.audio.footstepPath,
      mState.audio.footstepVolume,
      mState.audio.footstepWalkCadence,
      mState.audio.footstepRunCadence,
      mState.audioBackendAvailable,
      mState.audioStatus,
      dt,
      (int)mState.scene.registry().view<TransformComponent>().size(),
      (int)(mState.projectiles.count()),
      mState.renderSystem.stats().drawn,
      mState.renderSystem.stats().culled,
      mState.renderSystem.stats().drawCallsMain,
      mState.renderSystem.stats().drawCallsShadow,
      mState.renderSystem.stats().instancedDrawCallsMain,
      mState.renderSystem.stats().instancedDrawCallsShadow,
      mState.render.frustumCulling,
      &mState.lastRenderPassOrder,
      mState.hotReloadEnabled,
      mState.autoProcessImportQueue,
      mState.iconFontLoaded,
      &mState.hotReloadMessages,
      &mState.history.historyLabels,
      mState.history.historyCursor,
      &mState.profiler.samples(),
      mState.gpuFrameMs,
      mState.gpuShadowMs,
      mState.gpuMainMs,
      mState.glProgramBinds,
      mState.glTextureBinds,
      mState.glVaoBinds,
      mState.glStateChanges,
      selState,
      (int &)mState.playState};

  EditorUIOutput uiOut{};
  {
    ScopedCpuTimer timer(mState.profiler, "Editor UI");
    uiOut = mState.editor.draw(ctx);
  }
  if (!uiOut.consoleCommands.empty()) {
    for (auto &cmd : uiOut.consoleCommands) {
      mState.pending.pendingConsoleCommands.push_back(cmd);
    }
  }
  if (Keyboard::keyWentDown(GLFW_KEY_GRAVE_ACCENT)) {
    mState.editor.toggleConsole();
  }
  if (!uiOut.wantCaptureKeyboard) {
    if (Keyboard::keyWentDown(GLFW_KEY_1))
      mState.activeSlot = AppState::HotbarSlot::Axe;
    if (Keyboard::keyWentDown(GLFW_KEY_2))
      mState.activeSlot = AppState::HotbarSlot::Torch;
  }
  if (uiOut.sceneModified) {
    mState.history.pendingHistoryCommit = true;
    mState.history.pendingHistoryLabel = "Edit Scene";
  }
  // Debug overlay for mouse/camera in play mode
  if (mState.playState == AppState::PlayState::Playing) {
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
    ImGui::Begin("MouseDebug", nullptr,
                 ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav);
    ImGui::Text("dx: %.2f  dy: %.2f", mState.debugMouseDX,
                mState.debugMouseDY);
    ImGui::Text("yaw: %.2f  pitch: %.2f", mState.debugYaw,
                mState.debugPitch);
    ImGui::Text("front: %.2f %.2f %.2f", mState.debugCamFront.x,
                mState.debugCamFront.y, mState.debugCamFront.z);
    ImGui::Text("up: %.2f %.2f %.2f", mState.debugCamUp.x, mState.debugCamUp.y,
                mState.debugCamUp.z);
    ImGui::Text("grabHit: %u  dist: %.2f", mState.debugGrabHitId,
                mState.debugGrabHitDist);
    if (!mState.debugGrabHitName.empty())
      ImGui::Text("hitName: %s", mState.debugGrabHitName.c_str());
    ImGui::Text("grabbed: %u", mState.grabbedEntityId);
    if (!mState.debugGrabPrefab.empty())
      ImGui::Text("prefab: %s  idx: %d  moved: %s",
                  mState.debugGrabPrefab.c_str(), mState.debugGrabInstance,
                  mState.debugGrabMoved ? "yes" : "no");
    ImGui::End();
  }

  // Handle Play mode cursor locking and ESC to pause
  if (mState.playState == AppState::PlayState::Playing &&
      Keyboard::keyWentDown(GLFW_KEY_ESCAPE)) {
    mState.playState = AppState::PlayState::Paused;
  }

  static AppState::PlayState lastPlayState = AppState::PlayState::Stopped;
  static bool usingRawMouse = false;
  if (mState.playState == AppState::PlayState::Playing &&
      lastPlayState != AppState::PlayState::Playing) {
    usingRawMouse = glfwRawMouseMotionSupported();
    if (usingRawMouse) {
      glfwSetInputMode(mState.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
      glfwSetInputMode(mState.window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
      Mouse::setManualMode(false);
    } else {
      // Fallback: hide cursor and compute deltas via warp-to-center.
      glfwSetInputMode(mState.window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
      Mouse::setManualMode(true);
    }
    double mx, my;
    glfwGetCursorPos(mState.window, &mx, &my);
    Mouse::resetPosition(mx, my);
    mState.woodCount = 0;
    // Lock player rotation only during play to avoid physics overwrites.
    auto &reg = mState.scene.registry();
    if (mState.playerId != 0 && reg.has<RigidbodyComponent>(mState.playerId)) {
      reg.get<RigidbodyComponent>(mState.playerId).lockRotation = true;
    }
  } else if (mState.playState != AppState::PlayState::Playing &&
             lastPlayState == AppState::PlayState::Playing) {
    glfwSetInputMode(mState.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    if (usingRawMouse)
      glfwSetInputMode(mState.window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
    Mouse::setManualMode(false);
    Mouse::resetDeltas();
    auto &reg = mState.scene.registry();
    if (mState.playerId != 0 && reg.has<RigidbodyComponent>(mState.playerId)) {
      reg.get<RigidbodyComponent>(mState.playerId).lockRotation = false;
    }
  }

  lastPlayState = mState.playState;

  bool sceneMutatedByCommands = false;
  {
    ScopedCpuTimer timer(mState.profiler, "Commands/Scene");
    if (!mState.pending.pendingConsoleCommands.empty()) {
      for (const auto &cmd : mState.pending.pendingConsoleCommands) {
        if (executeConsoleCommand(mState, cmd))
          sceneMutatedByCommands = true;
      }
      mState.pending.pendingConsoleCommands.clear();
    }
    if (mState.pending.requestSaveConfig) {
      saveConfig(mState, "editor_state.bin");
      mState.pending.requestSaveConfig = false;
    }
    if (mState.pending.requestLoadConfig) {
      loadConfig(mState, "editor_state.bin");
      mState.pending.requestLoadConfig = false;
    }
    if (mState.pending.requestSaveProjectConfig) {
      if (!mState.projectConfig.saveToFile("project_config.json")) {
        LOG_ERROR("Runtime", "Failed to save project_config.json");
      }
      mState.pending.requestSaveProjectConfig = false;
    }
    if (!mState.pending.pendingSceneSavePath.empty()) {
      json root = json::parse(mState.scene.serializeToString(), nullptr, false);
      if (root.is_discarded())
        root = json::object();
      if (!root.contains("entities"))
        root["entities"] = json::array();
      root["terrainSettings"] = serializeTerrainSettings(mState.terrainSettings);
      root["terrainMaterial"] = serializeTerrainMaterial(mState.terrainMaterial);
      root["renderSettings"] = serializeRenderSettings(mState.render);
      root["audioSettings"] = serializeAudioSettings(mState.audio);
      root["sunSettings"] = serializeSunSettings(mState.sun);
      root["skySettings"] = serializeSkySettings(mState.skyUI);
      root["postProcess"] = serializePostProcess(mState.postProcessor);

      std::ofstream out(mState.pending.pendingSceneSavePath);
      if (!out.is_open()) {
        LOG_ERROR(
            "Runtime",
            "Failed to save scene: " + mState.pending.pendingSceneSavePath);
      } else {
        out << root.dump(2);
      }
      mState.pending.pendingSceneSavePath.clear();
    }
    if (!mState.pending.pendingSceneLoadPath.empty()) {
      std::ifstream in(mState.pending.pendingSceneLoadPath);
      json root = json::parse(in, nullptr, false);
      if (root.is_discarded()) {
        LOG_ERROR(
            "Runtime",
            "Failed to load scene: " + mState.pending.pendingSceneLoadPath);
        mState.pending.pendingSceneLoadPath.clear();
        return;
      }

      mState.terrainSystem.shutdown();

      if (!mState.scene.loadFromString(root.dump()))
        LOG_ERROR(
            "Runtime",
            "Failed to load scene: " + mState.pending.pendingSceneLoadPath);
      else {
        if (root.contains("terrainSettings"))
          applyTerrainSettings(root["terrainSettings"], mState.terrainSettings);
        if (root.contains("terrainMaterial"))
          applyTerrainMaterial(root["terrainMaterial"], mState.terrainMaterial);
        if (root.contains("renderSettings"))
          applyRenderSettings(root["renderSettings"], mState.render);
        if (root.contains("audioSettings"))
          applyAudioSettings(root["audioSettings"], mState.audio);
        if (root.contains("sunSettings"))
          applySunSettings(root["sunSettings"], mState.sun);
        if (root.contains("skySettings"))
          applySkySettings(root["skySettings"], mState.skyUI);
        if (root.contains("postProcess"))
          applyPostProcess(root["postProcess"], mState.postProcessor);

        if (mState.terrainSettings.enabled) {
          mState.terrainSystem.init(mState.terrainSettings, mState.scene,
                                    &mState.assets);
        }

        mState.history.pendingHistoryCommit = true;
        mState.history.pendingHistoryLabel = "Load Scene";
        sceneMutatedByCommands = true;
        mState.playerId = 0;
        for (auto e : mState.scene.registry().view<CameraComponent>()) {
          if (!mState.scene.registry().has<LifecycleComponent>(e) ||
              mState.scene.registry().get<LifecycleComponent>(e).state ==
                  EntityLifecycleState::Alive) {
            mState.playerId = e;
            break;
          }
        }
      }
      mState.pending.pendingSceneLoadPath.clear();
    }

    if (mState.history.requestHistoryJump >= 0) {
      applyHistorySnapshot(mState.history.requestHistoryJump);
      mState.history.requestHistoryJump = -1;
      sceneMutatedByCommands = true;
    } else if (mState.history.requestUndo) {
      applyHistorySnapshot(mState.history.historyCursor - 1);
      sceneMutatedByCommands = true;
    } else if (mState.history.requestRedo) {
      applyHistorySnapshot(mState.history.historyCursor + 1);
      sceneMutatedByCommands = true;
    }
    mState.history.requestUndo = false;
    mState.history.requestRedo = false;

    if (mState.autoProcessImportQueue)
      mState.assets.processImportQueue();
    if (mState.hotReloadEnabled) {
      mState.hotReloadMessages = mState.assets.pollHotReload();
    } else {
      mState.hotReloadMessages.clear();
    }

    for (const std::string &emptyName :
         mState.pending.pendingEmptyEntityNames) {
      (void)mState.scene.createEmptyEntity(emptyName.empty() ? "Empty"
                                                             : emptyName);
      mState.history.pendingHistoryCommit = true;
      mState.history.pendingHistoryLabel = "Create Entity";
      sceneMutatedByCommands = true;
    }
    mState.pending.pendingEmptyEntityNames.clear();

    for (uint32_t entityId : mState.pending.pendingDeleteEntityIds) {
      if (entityId != 0) {
        mState.scene.deleteEntity(entityId);
        mState.history.pendingHistoryCommit = true;
        mState.history.pendingHistoryLabel = "Delete Entity";
        sceneMutatedByCommands = true;
      }
    }
    mState.pending.pendingDeleteEntityIds.clear();

    for (const std::string &path : mState.pending.pendingSpawnPaths) {
      uint32_t spawnedId = 0;

      // Intercept procedural primitives (__primitive_cube, etc.)
      const std::string prefix = "__primitive_";
      if (path.substr(0, prefix.size()) == prefix) {
        std::string shape = path.substr(prefix.size());
        spawnedId = mState.scene.spawnPrimitive(shape);
      } else {
        spawnedId = mState.scene.spawnFromFile(path);
      }

      if (spawnedId == 0) {
        LOG_ERROR("Runtime", "Failed to spawn: " + path);
      } else {
        mState.history.pendingHistoryCommit = true;
        mState.history.pendingHistoryLabel = "Spawn Asset";
        sceneMutatedByCommands = true;
      }
    }
    mState.pending.pendingSpawnPaths.clear();

    if (!mState.pending.pendingDropPaths.empty()) {
      for (const std::string &path : mState.pending.pendingDropPaths) {
        const uint32_t spawnedId = mState.scene.spawnFromFile(path);
        if (spawnedId == 0)
          LOG_ERROR("Runtime", "Failed to load dropped model: " + path);
        else {
          mState.history.pendingHistoryCommit = true;
          mState.history.pendingHistoryLabel = "Spawn Asset";
          sceneMutatedByCommands = true;
        }
      }
      mState.pending.pendingDropPaths.clear();
    }

    const size_t entityCountBeforeFlush =
        mState.scene.registry().view<TransformComponent>().size();
    mState.scene.flushPendingDestroy();
    const size_t entityCountAfterFlush =
        mState.scene.registry().view<TransformComponent>().size();
    if (entityCountAfterFlush != entityCountBeforeFlush) {
      mState.history.pendingHistoryCommit = true;
      mState.history.pendingHistoryLabel = "Destroy Entity";
      sceneMutatedByCommands = true;
    }

    // Keep raw mesh pointers synchronized with authoritative asset handles.
    mState.scene.refreshMeshReferences();
  }

  if (mState.render.wireframe)
    GLStateCache::instance().setPolygonMode(GL_LINE);
  else
    GLStateCache::instance().setPolygonMode(GL_FILL);

  // Run Lua scripts only when Playing
  if (mState.playState == AppState::PlayState::Playing) {
    if (!usingRawMouse) {
      // Manual relative mouse fallback (warp-to-center).
      int winW, winH;
      glfwGetWindowSize(mState.window, &winW, &winH);
      const double cx = winW * 0.5;
      const double cy = winH * 0.5;
      double mx, my;
      glfwGetCursorPos(mState.window, &mx, &my);
      const double dx = mx - cx;
      const double dy = cy - my;
      glfwSetCursorPos(mState.window, cx, cy);
      Mouse::resetPosition(cx, cy);
      Mouse::setDeltas(dx, dy);
    }
    // Apply mouselook or drag-grab (play mode) directly in C++.
    {
      auto &reg = mState.scene.registry();
      if (mState.playerId == 0 || !reg.has<CameraComponent>(mState.playerId)) {
        mState.playerId = 0;
        for (auto e : reg.view<CameraComponent>()) {
          if (!reg.has<LifecycleComponent>(e) ||
              reg.get<LifecycleComponent>(e).state ==
                  EntityLifecycleState::Alive) {
            mState.playerId = e;
            break;
          }
        }
      }
      const float dx = (float)Mouse::getDX();
      const float dy = (float)Mouse::getDY();
      mState.debugMouseDX = dx;
      mState.debugMouseDY = dy;

      // Start grab on LMB down.
      if (Mouse::buttonWentDown(GLFW_MOUSE_BUTTON_LEFT) &&
          mState.grabbedEntityId == 0 &&
          mState.playerId != 0 &&
          reg.has<TransformComponent>(mState.playerId)) {
        const auto &camTr = reg.get<TransformComponent>(mState.playerId);
        const glm::vec3 camPos = camTr.position;
        glm::vec3 camFront;
        camFront.x = -sin(glm::radians(camTr.rotation.y)) *
                     cos(glm::radians(camTr.rotation.x));
        camFront.y = sin(glm::radians(camTr.rotation.x));
        camFront.z = -cos(glm::radians(camTr.rotation.y)) *
                     cos(glm::radians(camTr.rotation.x));
        camFront = glm::normalize(camFront);
        PhysicsRaycastResult hit = mState.physicsSystem.raycast(
            camPos, camFront, 20.0f, mState.playerId);
        mState.debugGrabHitId = hit.entityId;
        mState.debugGrabHitDist = hit.distance;
        mState.debugGrabHitName.clear();
        if (hit.entityId != 0 && reg.has<NameComponent>(hit.entityId)) {
          mState.debugGrabHitName =
              reg.get<NameComponent>(hit.entityId).name;
        }
        if (hit.hit && hit.entityId != 0) {
          if (reg.has<TransformComponent>(hit.entityId)) {
            bool canGrab = true;
            if (reg.has<MeshComponent>(hit.entityId)) {
              const auto &mc = reg.get<MeshComponent>(hit.entityId);
              if (mc.isTerrain || mc.isViewModel)
                canGrab = false;
            }
            if (hit.entityId == mState.playerId)
              canGrab = false;
            if (canGrab) {
              mState.grabbedEntityId = hit.entityId;
              mState.grabbedDistance = std::max(1.0f, hit.distance);
              mState.grabbedHadRigidbody = false;
              mState.grabbedPrevBodyType = 0;
              mState.grabbedIsTreeInstance = false;
              mState.grabbedPrefab.clear();
              mState.grabbedInstanceIndex = 0;
              mState.grabbedBaseMatrix = glm::mat4(1.0f);
              if (reg.has<RigidbodyComponent>(hit.entityId)) {
                auto &rb = reg.get<RigidbodyComponent>(hit.entityId);
                mState.grabbedHadRigidbody = true;
                mState.grabbedPrevBodyType = (int)rb.type;
                if (rb.type == RigidbodyComponent::Type::Static)
                  rb.type = RigidbodyComponent::Type::Kinematic;
              }
              if (reg.has<TreeComponent>(hit.entityId)) {
                auto &tree = reg.get<TreeComponent>(hit.entityId);
                mState.grabbedIsTreeInstance = true;
                mState.grabbedPrefab = tree.prefabName;
                mState.grabbedInstanceIndex = tree.instanceIndex;
                glm::mat4 instM;
                if (mState.terrainSystem.getPrefabInstanceMatrix(
                        tree.prefabName, tree.instanceIndex, instM)) {
                  mState.grabbedBaseMatrix = instM;
                  glm::vec3 basePos = glm::vec3(instM[3]);
                  mState.grabbedOffset = basePos - camPos;
                } else {
                  mState.grabbedOffset = camPos + camFront * mState.grabbedDistance - camPos;
                }
              } else {
                mState.grabbedOffset = camPos + camFront * mState.grabbedDistance - camPos;
              }
            }
          }
        }
      }

      const bool dragging =
          (mState.grabbedEntityId != 0 &&
           Mouse::button(GLFW_MOUSE_BUTTON_LEFT));

      // Normal mouselook always active.
      if (mState.playerId != 0 && reg.has<TransformComponent>(mState.playerId)) {
        auto &tr = reg.get<TransformComponent>(mState.playerId);
        const float sens = mState.input.mouseSensitivity;
        tr.rotation.y -= dx * sens;
        tr.rotation.x += dy * sens;
        if (tr.rotation.x > 89.0f)
          tr.rotation.x = 89.0f;
        if (tr.rotation.x < -89.0f)
          tr.rotation.x = -89.0f;
        if (tr.rotation.y > 180.0f)
          tr.rotation.y -= 360.0f;
        if (tr.rotation.y < -180.0f)
          tr.rotation.y += 360.0f;
        mState.debugYaw = tr.rotation.y;
        mState.debugPitch = tr.rotation.x;
      }

      if (dragging) {
        // Drag object in camera screen plane using mouse deltas.
        if (reg.has<TransformComponent>(mState.grabbedEntityId) &&
            mState.playerId != 0 &&
            reg.has<TransformComponent>(mState.playerId)) {
          auto &grabTr = reg.get<TransformComponent>(mState.grabbedEntityId);
          auto &camTr = reg.get<TransformComponent>(mState.playerId);
          glm::vec3 front;
          front.x = -sin(glm::radians(camTr.rotation.y)) *
                    cos(glm::radians(camTr.rotation.x));
          front.y = sin(glm::radians(camTr.rotation.x));
          front.z = -cos(glm::radians(camTr.rotation.y)) *
                    cos(glm::radians(camTr.rotation.x));
          front = glm::normalize(front);
          glm::vec3 right =
              glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
          glm::vec3 up = glm::normalize(glm::cross(right, front));
          const float dragScale = 0.01f * mState.grabbedDistance;
          mState.grabbedOffset += right * (-dx) * dragScale;
          mState.grabbedOffset += up * (dy) * dragScale;
          const glm::vec3 targetPos = camTr.position + mState.grabbedOffset;
          const glm::vec3 oldPos = grabTr.position;
          const glm::vec3 delta = targetPos - oldPos;
          const float invDt = (dt > 0.0001f) ? (1.0f / dt) : 0.0f;
          mState.grabbedReleaseVelocity = delta * invDt;

          // Apply a simple physics pull if the object has a rigidbody.
          if (reg.has<RigidbodyComponent>(mState.grabbedEntityId)) {
            auto &rb = reg.get<RigidbodyComponent>(mState.grabbedEntityId);
            const glm::vec3 vel = delta * invDt * 0.85f;
            rb.pendingLinearVelocity = vel;
            rb.setLinearVelocity = true;
          }

          grabTr.position = targetPos;

          // If this is a terrain tree instance, update instanced matrix too.
          mState.debugGrabPrefab.clear();
          mState.debugGrabInstance = -1;
          mState.debugGrabMoved = false;
          if (reg.has<TreeComponent>(mState.grabbedEntityId)) {
            auto &tree = reg.get<TreeComponent>(mState.grabbedEntityId);
            mState.debugGrabPrefab = tree.prefabName;
            mState.debugGrabInstance = (int)tree.instanceIndex;
            glm::mat4 instM = mState.grabbedBaseMatrix;
            instM[3] = glm::vec4(targetPos, 1.0f);
            mState.debugGrabMoved =
                mState.terrainSystem.setPrefabInstanceMatrix(
                    tree.prefabName, tree.instanceIndex, instM);
          }
        } else {
          mState.grabbedEntityId = 0;
        }
      }

      if (Mouse::buttonWentUp(GLFW_MOUSE_BUTTON_LEFT)) {
        // On release: convert tree instance into a physics-enabled entity.
        if (mState.grabbedEntityId != 0 &&
            reg.has<TreeComponent>(mState.grabbedEntityId)) {
          if (mState.terrainSystem.convertTreeToEntity(
                  mState.grabbedEntityId)) {
            if (reg.has<RigidbodyComponent>(mState.grabbedEntityId)) {
              auto &rb = reg.get<RigidbodyComponent>(mState.grabbedEntityId);
              rb.type = RigidbodyComponent::Type::Dynamic;
              rb.lockRotation = false;
              rb.pendingLinearVelocity = mState.grabbedReleaseVelocity;
              rb.setLinearVelocity = true;
            } else {
              auto &rb =
                  reg.emplace<RigidbodyComponent>(mState.grabbedEntityId);
              rb.type = RigidbodyComponent::Type::Dynamic;
              rb.lockRotation = false;
              rb.pendingLinearVelocity = mState.grabbedReleaseVelocity;
              rb.setLinearVelocity = true;
            }
          }
        }
        if (mState.grabbedEntityId != 0 && mState.grabbedHadRigidbody &&
            reg.has<RigidbodyComponent>(mState.grabbedEntityId)) {
          auto &rb = reg.get<RigidbodyComponent>(mState.grabbedEntityId);
          rb.type = (RigidbodyComponent::Type)mState.grabbedPrevBodyType;
        }
        mState.grabbedEntityId = 0;
        mState.grabbedHadRigidbody = false;
        mState.grabbedPrevBodyType = 0;
        mState.grabbedIsTreeInstance = false;
        mState.grabbedPrefab.clear();
        mState.grabbedInstanceIndex = 0;
        mState.grabbedBaseMatrix = glm::mat4(1.0f);
        mState.grabbedReleaseVelocity = glm::vec3(0.0f);
      }
    }

    // ── Mouse2: Dynamic Terrain Cratering ──
    static float craterCooldown = 0.0f;
    if (craterCooldown > 0.0f) {
      craterCooldown -= dt;
    } else {
      bool mouse2Down =
          glfwGetMouseButton(mState.window, GLFW_MOUSE_BUTTON_RIGHT) ==
          GLFW_PRESS;
      if (mouse2Down) {
        auto &reg = mState.scene.registry();
        if (mState.playerId != 0 && reg.has<TransformComponent>(mState.playerId)) {
          auto &camTr = reg.get<TransformComponent>(mState.playerId);
          glm::vec3 front;
          front.x = -sin(glm::radians(camTr.rotation.y)) *
                    cos(glm::radians(camTr.rotation.x));
          front.y = sin(glm::radians(camTr.rotation.x));
          front.z = -cos(glm::radians(camTr.rotation.y)) *
                    cos(glm::radians(camTr.rotation.x));
          front = glm::normalize(front);

          auto hit = mState.physicsSystem.raycast(camTr.position, front, 100.0f,
                                                  mState.playerId);
          if (hit.hit) {
            // Apply a negative height offset to carve out a crater.
            // Radius 2.5 meters, scooping out 0.8 meters per tick.
            bool carved =
                mState.terrainSystem.applyHeightBrush(hit.position, 2.5f, -0.8f);
            if (carved) {
              craterCooldown = 0.1f; // Limit to ~10 carves per second
            }
          }
        }
      }
    }
    {
      ScopedCpuTimer timer(mState.profiler, "Scripts");
      mState.scriptSystem.update(mState.scene.registry(), dt);
    }
    {
      ScopedCpuTimer timer(mState.profiler, "Physics");
      mState.physicsSystem.update(mState.scene.registry(), dt);
    }

    if (mState.terrainSystem.isEnabled()) {
      ScopedCpuTimer timer(mState.profiler, "Terrain Clamp");
      auto &reg = mState.scene.registry();
      for (auto e : reg.viewAll<TransformComponent, ScriptComponent>())
        clampEntityToTerrain(mState, reg, e);
    }
  }

  // ── Editor Camera (orbit / pan / zoom via mouse) ──
  {
    ScopedCpuTimer timer(mState.profiler, "Editor Camera");
    bool imguiWants = uiOut.wantCaptureMouse || ImGuizmo::IsUsing();
    mState.editorCamera.update(mState.window, imguiWants);
  }

  // F key: focus on selected entity
  if (Keyboard::key(GLFW_KEY_F) && !uiOut.wantCaptureKeyboard &&
      mState.selection.selectedEntityId != 0) {
    auto &reg = mState.scene.registry();
    if (reg.has<TransformComponent>(mState.selection.selectedEntityId)) {
      glm::vec3 target =
          reg.get<TransformComponent>(mState.selection.selectedEntityId)
              .position;
      mState.editorCamera.focusOn(target);
    }
  }

  glm::vec3 cameraPos = mState.editorCamera.getPosition();
  glm::vec3 cameraFront = mState.editorCamera.getForwardVector();
  glm::vec3 cameraUp = mState.editorCamera.getUpVector();

  glm::mat4 view = mState.editorCamera.getViewMatrix();

  const bool forcePlayerCam =
      (mState.playState != AppState::PlayState::Playing &&
       mState.usePlayerCameraInEdit);

  if (mState.playState == AppState::PlayState::Playing || forcePlayerCam) {
    auto &reg = mState.scene.registry();
    if (mState.playerId == 0 || !reg.has<CameraComponent>(mState.playerId)) {
      mState.playerId =
          0; // Reset — stays 0 if no CameraComponent entity exists
      for (auto e : reg.view<CameraComponent>()) {
        if (!reg.has<LifecycleComponent>(e) ||
            reg.get<LifecycleComponent>(e).state ==
                EntityLifecycleState::Alive) {
          mState.playerId = e;
          break;
        }
      }
    }

    if (mState.playerId != 0 && reg.has<TransformComponent>(mState.playerId)) {
      auto &tr = reg.get<TransformComponent>(mState.playerId);
      if (forcePlayerCam) {
        tr.position = mState.editorCamera.getPosition();
        tr.rotation =
            glm::vec3(mState.editorCamera.pitch, mState.editorCamera.yaw, 0.0f);
      }
      // Clamp pitch and keep yaw bounded.
      if (tr.rotation.x > 89.0f)
        tr.rotation.x = 89.0f;
      if (tr.rotation.x < -89.0f)
        tr.rotation.x = -89.0f;
      if (tr.rotation.y > 180.0f)
        tr.rotation.y -= 360.0f;
      if (tr.rotation.y < -180.0f)
        tr.rotation.y += 360.0f;

      if (mState.terrainSystem.isEnabled())
        clampEntityToTerrain(mState, reg, mState.playerId);

      cameraPos = tr.position;

      glm::vec3 front;
      front.x =
          -sin(glm::radians(tr.rotation.y)) * cos(glm::radians(tr.rotation.x));
      front.y = sin(glm::radians(tr.rotation.x));
      front.z =
          -cos(glm::radians(tr.rotation.y)) * cos(glm::radians(tr.rotation.x));
      cameraFront = glm::normalize(front);

      // Use a fixed world up to avoid roll/inversion near steep angles.
      cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
      mState.debugCamFront = cameraFront;
      mState.debugCamUp = cameraUp;
      view = glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);
    }
  }

  // Viewmodel (axe)
  const bool showAxe =
      mState.axeEnabled &&
      mState.activeSlot == AppState::HotbarSlot::Axe &&
      (mState.playState == AppState::PlayState::Playing || mState.uiMode);
  if (showAxe) {
    auto &reg = mState.scene.registry();
    if (mState.axeEntity == 0 ||
        !reg.has<MeshComponent>(mState.axeEntity) ||
        !reg.has<TransformComponent>(mState.axeEntity)) {
      auto h = mState.assets.loadOBJ(mState.axePath);
      OBJModel *mdl = mState.assets.getOBJ(h);
      if (mdl) {
        EntityId eid = mState.scene.createEmptyEntity("Axe");
        reg.emplace<TransientComponent>(eid);
        auto &t = reg.get<TransformComponent>(eid);
        t.position = mState.axeOffset;
        t.rotation = mState.axeRotation;
        t.scale = mState.axeScale;
        auto &mc = reg.emplace<MeshComponent>(eid, mdl, true, false);
        mc.objHandle = h;
        mc.assetId = mState.axePath;
        mc.isViewModel = true;
        mState.axeEntity = eid;
      }
    }

    if (mState.axeEntity != 0 && reg.has<TransformComponent>(mState.axeEntity)) {
      if (reg.has<MeshComponent>(mState.axeEntity)) {
        reg.get<MeshComponent>(mState.axeEntity).visible = true;
      }
      auto &t = reg.get<TransformComponent>(mState.axeEntity);
      // Fixed viewmodel in screen space (no camera correlation).
      // View space looks down -Z, so treat positive Z as "forward".
      t.position = glm::vec3(mState.axeOffset.x, mState.axeOffset.y,
                             -std::abs(mState.axeOffset.z));
      t.rotation = mState.axeRotation;
      t.scale = mState.axeScale;
    }
  } else if (mState.axeEntity != 0) {
    auto &reg = mState.scene.registry();
    if (reg.has<MeshComponent>(mState.axeEntity)) {
      reg.get<MeshComponent>(mState.axeEntity).visible = false;
    }
  }

  const bool showTorch =
      mState.torchEnabled &&
      mState.activeSlot == AppState::HotbarSlot::Torch &&
      (mState.playState == AppState::PlayState::Playing || mState.uiMode);
  if (showTorch) {
    auto &reg = mState.scene.registry();
    if (mState.torchEntity == 0 ||
        !reg.has<MeshComponent>(mState.torchEntity) ||
        !reg.has<TransformComponent>(mState.torchEntity)) {
      EntityId eid = mState.scene.spawnPrimitive("cube");
      if (eid != 0) {
        reg.emplace<TransientComponent>(eid);
        if (reg.has<NameComponent>(eid))
          reg.get<NameComponent>(eid).name = "Torch";
        auto &t = reg.get<TransformComponent>(eid);
        t.position = mState.torchOffset;
        t.rotation = mState.torchRotation;
        t.scale = mState.torchScale;
        auto &mc = reg.get<MeshComponent>(eid);
        mc.visible = true;
        mc.castsShadow = false;
        mc.isViewModel = true;
        if (reg.has<MaterialOverrideComponent>(eid)) {
          auto &mo = reg.get<MaterialOverrideComponent>(eid);
          mo.enabled = true;
          mo.material.baseColor = glm::vec4(0.18f, 0.12f, 0.07f, 1.0f);
          mo.material.roughness = 1.0f;
          mo.material.metallic = 0.0f;
          mo.material.ao = 1.0f;
        } else {
          MaterialOverrideComponent mo;
          mo.enabled = true;
          mo.material.id = "__torch_stick";
          mo.material.baseColor = glm::vec4(0.18f, 0.12f, 0.07f, 1.0f);
          mo.material.roughness = 1.0f;
          mo.material.metallic = 0.0f;
          mo.material.ao = 1.0f;
          reg.emplace<MaterialOverrideComponent>(eid, std::move(mo));
        }
        mState.torchEntity = eid;
      }
    }

    if (mState.torchEntity != 0 &&
        reg.has<TransformComponent>(mState.torchEntity)) {
      if (reg.has<MeshComponent>(mState.torchEntity)) {
        reg.get<MeshComponent>(mState.torchEntity).visible = true;
      }
      auto &t = reg.get<TransformComponent>(mState.torchEntity);
      t.position = glm::vec3(mState.torchOffset.x, mState.torchOffset.y,
                             -std::abs(mState.torchOffset.z));
      t.rotation = mState.torchRotation;
      t.scale = mState.torchScale;
    }
  } else if (mState.torchEntity != 0) {
    auto &reg = mState.scene.registry();
    if (reg.has<MeshComponent>(mState.torchEntity)) {
      reg.get<MeshComponent>(mState.torchEntity).visible = false;
    }
  }

  // Update Terrain chunk loading around the camera
  {
    ScopedCpuTimer timer(mState.profiler, "Terrain Update");
    mState.terrainSystem.update(cameraPos);
  }

  int winW, winH;
  glfwGetWindowSize(mState.window, &winW, &winH);
  glm::mat4 projection = glm::perspective(
      glm::radians(mState.input.fov), (float)winW / (float)winH, 0.1f, 500.0f);
  const bool allowTemporalJitter =
      (mState.playState == AppState::PlayState::Playing);
  if (!allowTemporalJitter)
    mState.postProcessor.resetTemporalHistory();
  glm::mat4 renderProjection =
      mState.postProcessor.jitteredProjection(projection, allowTemporalJitter);

  const bool brushActive =
      mState.terrainBrush.enabled && mState.terrainSystem.isEnabled() &&
      mState.playState != AppState::PlayState::Playing;
  if (brushActive && !uiOut.wantCaptureMouse && !ImGuizmo::IsUsing()) {
    float mx = (float)Mouse::getMouseX();
    float my = (float)Mouse::getMouseY();
    Ray ray = MousePicking::screenToRay(mx, my, 0.0f, 0.0f, (float)winW,
                                        (float)winH, view, projection);
    glm::vec3 hit;
    if (raycastTerrain(mState.terrainSystem, ray, 300.0f, hit)) {
      constexpr int kSegments = 40;
      constexpr float kTwoPi = 6.28318530718f;
      const float r = mState.terrainBrush.radius;
      const float step = kTwoPi / (float)kSegments;
      ImDrawList *dl = ImGui::GetForegroundDrawList();
      ImU32 col = IM_COL32(255, 140, 40, 220);
      ImVec2 prev;
      bool havePrev = false;
      for (int i = 0; i <= kSegments; ++i) {
        float a = step * (float)i;
        glm::vec3 p(hit.x + std::cos(a) * r, hit.y + 0.05f,
                    hit.z + std::sin(a) * r);
        ImVec2 sp;
        if (projectToScreen(p, view, projection, (float)winW, (float)winH, sp)) {
          if (havePrev)
            dl->AddLine(prev, sp, col, 2.0f);
          prev = sp;
          havePrev = true;
        } else {
          havePrev = false;
        }
      }
    }
  }
  if (brushActive && !uiOut.wantCaptureMouse && !ImGuizmo::IsUsing() &&
      Mouse::button(GLFW_MOUSE_BUTTON_LEFT)) {
    float mx = (float)Mouse::getMouseX();
    float my = (float)Mouse::getMouseY();
    Ray ray = MousePicking::screenToRay(mx, my, 0.0f, 0.0f, (float)winW,
                                        (float)winH, view, projection);
    glm::vec3 hit;
    if (raycastTerrain(mState.terrainSystem, ray, 300.0f, hit)) {
      if (mState.terrainBrush.mode == 0 || mState.terrainBrush.mode == 1) {
        float dir = (mState.terrainBrush.mode == 0) ? 1.0f : -1.0f;
        float delta = dir * mState.terrainBrush.strength * dt;
        mState.terrainSystem.applyHeightBrush(
            hit, mState.terrainBrush.radius, delta);
      } else {
        const char *prefab = "prefab_pine";
        if (mState.terrainBrush.target == 1)
          prefab = "prefab_rock";
        else if (mState.terrainBrush.target == 2)
          prefab = "prefab_grass";
        bool add = (mState.terrainBrush.mode == 2);
        mState.terrainSystem.applyVegetationBrush(
            hit, mState.terrainBrush.radius, prefab, add,
            mState.terrainBrush.scatterCount);
      }
    }
  }

  if (mState.uiMode && mState.playState != AppState::PlayState::Playing &&
      !brushActive && Mouse::buttonWentDown(GLFW_MOUSE_BUTTON_LEFT) &&
      !uiOut.wantCaptureMouse && !ImGuizmo::IsUsing()) {
    float mx = (float)Mouse::getMouseX();
    float my = (float)Mouse::getMouseY();
    Ray ray = MousePicking::screenToRay(mx, my, 0.0f, 0.0f, (float)winW,
                                        (float)winH, view, projection);
    uint32_t hitId = MousePicking::pickEntity(ray, mState.scene.registry());

    if (hitId != 0) {
      bool ctrlHeld =
          glfwGetKey(mState.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
          glfwGetKey(mState.window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS ||
          glfwGetKey(mState.window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS;
      if (ctrlHeld) {
        auto &sel = mState.selection.selectedEntities;
        auto it = std::find(sel.begin(), sel.end(), hitId);
        if (it != sel.end()) {
          sel.erase(it);
          if (mState.selection.selectedEntityId == hitId)
            mState.selection.selectedEntityId = sel.empty() ? 0 : sel.back();
        } else {
          sel.push_back(hitId);
          mState.selection.selectedEntityId = hitId;
        }
      } else {
        mState.selection.selectedEntities.clear();
        mState.selection.selectedEntities.push_back(hitId);
        mState.selection.selectedEntityId = hitId;
      }
      mState.selection.lastClickedEntity = hitId;

      auto &reg = mState.scene.registry();
      if (reg.has<MeshComponent>(hitId) && reg.has<TransformComponent>(hitId)) {
        OBJModel *mdl = reg.get<MeshComponent>(hitId).objModel;
        if (mdl && mdl->submeshCount() > 1) {
          auto &tr = reg.get<TransformComponent>(hitId);
          std::string hitPart = MousePicking::pickSubmesh(ray, tr, *mdl);
          if (!hitPart.empty()) {
            mState.selection.editObjPart = true;
            mState.selection.selectedObjPartName = hitPart;
          } else {
            mState.selection.editObjPart = false;
            mState.selection.selectedObjPartName.clear();
          }
        } else {
          mState.selection.editObjPart = false;
          mState.selection.selectedObjPartName.clear();
        }
      } else {
        mState.selection.editObjPart = false;
        mState.selection.selectedObjPartName.clear();
      }
    } else {
      mState.selection.selectedEntities.clear();
      mState.selection.selectedEntityId = 0;
      mState.selection.editObjPart = false;
      mState.selection.selectedObjPartName.clear();
    }
  }

  // (grab handled earlier during play update)

  {
    if (mState.editor.drawGizmo(mState.uiMode, view, projection, mState.scene,
                                mState.sun, mState.events, selState,
                                cameraPos)) {
      mState.history.pendingHistoryCommit = true;
      mState.history.pendingHistoryLabel = "Edit Scene";
    }
  }

  if (mState.history.pendingHistoryCommit) {
    const bool interacting = ImGuizmo::IsUsing() || ImGui::IsAnyItemActive() ||
                             ImGui::IsMouseDown(0);
    if (!interacting || sceneMutatedByCommands) {
      commitHistorySnapshot(mState.history.pendingHistoryLabel.empty()
                                ? "Edit Scene"
                                : mState.history.pendingHistoryLabel);
      mState.history.pendingHistoryCommit = false;
      mState.history.pendingHistoryLabel.clear();
    }
  }

  {
    ScopedCpuTimer timer(mState.profiler, "Projectiles");
    mState.projectiles.update(dt);
  }

  if (mState.audioSubsystem) {
    if (mState.pending.requestTestFootstepAudio) {
      mState.audioSubsystem->playTestFootstep();
      mState.pending.requestTestFootstepAudio = false;
    }
    ScopedCpuTimer timer(mState.profiler, "Audio");
    mState.audioSubsystem->update(dt, cameraPos, cameraFront);
  }

  // Use non-jittered projection for culling stability.
  mState.renderSystem.setViewProjection(projection * view);
  mState.renderSystem.setCameraPosition(cameraPos);
  mState.renderSystem.setCullingEnabled(mState.render.frustumCulling);
  mState.renderSystem.setShadowCameraCulling(
      mState.render.shadowCameraCulling);
  mState.renderSystem.beginFrame();

  if (mState.renderLoopSubsystem) {
    ScopedCpuTimer timer(mState.profiler, "Render CPU");
    mState.renderLoopSubsystem->executeRenderPasses(
        view, renderProjection, cameraPos, cameraFront, cameraUp, renderTime);
  }

  if (mState.editorSubsystem) {
    ScopedCpuTimer timer(mState.profiler, "ImGui End");
    mState.editorSubsystem->endFrame();
  }

  const auto glStats = GLStateCache::instance().counters();
  mState.glProgramBinds = glStats.programBinds;
  mState.glTextureBinds = glStats.textureBinds;
  mState.glVaoBinds = glStats.vaoBinds;
  mState.glStateChanges = glStats.stateChanges;

  mState.profiler.endFrame();
}
