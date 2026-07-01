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
#include "ECS/Systems/DestructionSystem.h"
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
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>

#include "json.hpp"

namespace {
using json = nlohmann::json;

void saveConfig(const AppState &s, const char *filename) {}
void loadConfig(AppState &s, const char *filename) {}
constexpr float kTerrainEdgeMargin = 0.2f;
constexpr float kTerrainMinClearance = 0.15f;
constexpr const char *kProjectDefaultsPath = "project_defaults.json";

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

std::string formatRenderPassOrderForLog(const std::vector<std::string> &passes) {
  if (passes.empty())
    return "none";
  std::ostringstream ss;
  for (size_t i = 0; i < passes.size(); ++i) {
    if (i > 0)
      ss << " -> ";
    ss << passes[i];
  }
  return ss.str();
}

std::string formatTopCpuSamplesForLog(const std::vector<FrameProfiler::Sample> &samples,
                                      int limit) {
  if (samples.empty() || limit <= 0)
    return "none";

  std::vector<FrameProfiler::Sample> sorted = samples;
  std::sort(sorted.begin(), sorted.end(),
            [](const FrameProfiler::Sample &a, const FrameProfiler::Sample &b) {
              return a.ms > b.ms;
            });

  const size_t sampleCount =
      std::min(sorted.size(), (size_t)std::max(1, limit));
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(2);
  for (size_t i = 0; i < sampleCount; ++i) {
    if (i > 0)
      ss << " | ";
    ss << sorted[i].name << "=" << sorted[i].ms << "ms";
  }
  return ss.str();
}

void logPerformanceSnapshotIfNeeded(AppState &s, float dt) {
  auto &cfg = s.performanceLog;
  if (!cfg.enabled)
    return;

  s.performanceLogFrameIndex++;
  s.performanceLogSummaryTimer += std::max(dt, 0.0f);
  s.performanceLogSpikeCooldown =
      std::max(0.0f, s.performanceLogSpikeCooldown - std::max(dt, 0.0f));

  const FramePerformanceSnapshot &perf = s.performance;
  const bool spikeFrame =
      cfg.logSpikeFrames && perf.frameMs >= std::max(1.0f, cfg.spikeThresholdMs);
  const bool periodicSummary =
      cfg.logPeriodicSummary &&
      s.performanceLogSummaryTimer >= std::max(0.25f, cfg.summaryIntervalSec);
  const bool logFrame = cfg.logEveryFrame;

  if (!logFrame && !spikeFrame && !periodicSummary)
    return;
  if (spikeFrame && s.performanceLogSpikeCooldown > 0.0f && !logFrame)
    return;

  std::ostringstream header;
  header << std::fixed << std::setprecision(2);
  header << "frame#" << s.performanceLogFrameIndex << " fps=" << perf.fps
         << " frame=" << perf.frameMs << "ms gpu=" << perf.gpuFrameMs
         << "ms main=" << perf.gpuMainMs << "ms shadow=" << perf.gpuShadowMs
         << "ms gap=" << perf.cpuGpuGapMs
         << "ms bottleneck=" << performanceBottleneckLabel(perf.bottleneck)
         << " backend=" << perf.submissionBackendLabel;

  std::ostringstream details;
  details << std::fixed << std::setprecision(1);
  details << "\nGPU main split sky=" << perf.gpuMainSkyMs
          << " terrain=" << perf.gpuMainTerrainMs
          << " scene=" << perf.gpuMainSceneMs
          << " post=" << perf.gpuMainPostMs;
  details << "\nDraws main=" << perf.drawCallsMain
          << " shadow=" << perf.drawCallsShadow
          << " instMain=" << perf.instancedDrawCallsMain
          << " instShadow=" << perf.instancedDrawCallsShadow
          << " terrain=" << perf.terrainDrawCalls
          << " entities=" << perf.entityCount
          << " particles=" << perf.particleCount;
  details << "\nTerrain vis=" << perf.terrainVisibleChunks
          << " pages=" << perf.terrainGpuPagesUsed
          << " frustumCull=" << perf.terrainFrustumCulledChunks
          << " horizonCull=" << perf.terrainHorizonCulledChunks
          << " jobs=" << perf.terrainPendingJobs
          << " uploadsPending=" << perf.terrainPendingUploads
          << " collision=" << perf.terrainCollisionBodies
          << " uploadKB=" << (perf.terrainUploadBytes / 1024.0f)
          << " uploads=" << perf.terrainUploads
          << " skips=" << perf.terrainUploadSkips;
  details << "\nInst uploads main=" << perf.instancedUploadsMain
          << " shadow=" << perf.instancedUploadsShadow
          << " skipMain=" << perf.instancedUploadSkipsMain
          << " skipShadow=" << perf.instancedUploadSkipsShadow
          << " clustersMain=" << perf.instancedClustersVisibleMain << "/"
          << perf.instancedClustersTestedMain
          << " clustersShadow=" << perf.instancedClustersVisibleShadow << "/"
          << perf.instancedClustersTestedShadow;
  details << "\nShadow cascades=" << perf.shadowCascadesUpdated << "/"
          << std::max(perf.shadowCascadeCount, 1)
          << " staggered=" << (perf.shadowCascadeStaggered ? "yes" : "no")
          << " distCull=" << perf.shadowDistanceCulled
          << " smallCull=" << perf.shadowSmallCasterCulled;
  details << "\nGL binds program=" << perf.glProgramBinds
          << " texture=" << perf.glTextureBinds
          << " vao=" << perf.glVaoBinds
          << " state=" << perf.glStateChanges;
  details << "\nPasses: " << formatRenderPassOrderForLog(perf.renderPassOrder);
  details << "\nCPU top: "
          << formatTopCpuSamplesForLog(s.profiler.samples(), cfg.cpuSampleLimit);

  const std::string message = header.str() + details.str();
  if (spikeFrame) {
    LOG_WARN("Perf", "Spike detected: " + message);
    s.performanceLogSpikeCooldown = std::max(0.0f, cfg.spikeCooldownSec);
  } else if (logFrame) {
    LOG_INFO("Perf", "Frame: " + message);
  } else {
    LOG_INFO("Perf", "Summary: " + message);
  }

  if (periodicSummary)
    s.performanceLogSummaryTimer = 0.0f;
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
             "stop, perf_log <off|spikes|summary|all|threshold <ms>>");
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
    s.editorSubsystem->pending().pendingSpawnPaths.push_back(args[1]);
    return true;
  }

  if (cmd == "perf_log") {
    if (args.size() < 2) {
      LOG_INFO(
          "Console",
          std::string("perf_log = ") +
              (s.performanceLog.enabled ? "on" : "off") +
              " mode=" +
              (s.performanceLog.logEveryFrame
                   ? "all"
                   : (s.performanceLog.logSpikeFrames
                          ? (s.performanceLog.logPeriodicSummary ? "spikes"
                                                                 : "spikes_only")
                          : "summary")) +
              " threshold_ms=" +
              std::to_string(s.performanceLog.spikeThresholdMs));
      return false;
    }

    std::string mode = args[1];
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (mode == "off") {
      s.performanceLog.enabled = false;
      LOG_INFO("Console", "Performance logging disabled");
      return false;
    }
    if (mode == "spikes") {
      s.performanceLog.enabled = true;
      s.performanceLog.logEveryFrame = false;
      s.performanceLog.logSpikeFrames = true;
      s.performanceLog.logPeriodicSummary = true;
      s.performanceLogSummaryTimer = 0.0f;
      s.performanceLogSpikeCooldown = 0.0f;
      LOG_INFO("Console", "Performance logging set to spikes + summaries");
      return false;
    }
    if (mode == "summary") {
      s.performanceLog.enabled = true;
      s.performanceLog.logEveryFrame = false;
      s.performanceLog.logSpikeFrames = false;
      s.performanceLog.logPeriodicSummary = true;
      s.performanceLogSummaryTimer = 0.0f;
      LOG_INFO("Console", "Performance logging set to summaries only");
      return false;
    }
    if (mode == "all") {
      s.performanceLog.enabled = true;
      s.performanceLog.logEveryFrame = true;
      s.performanceLog.logSpikeFrames = true;
      s.performanceLog.logPeriodicSummary = false;
      s.performanceLogSpikeCooldown = 0.0f;
      LOG_WARN("Console",
               "Performance logging set to every frame; this may reduce FPS");
      return false;
    }
    if (mode == "threshold") {
      if (args.size() < 3) {
        LOG_WARN("Console", "Usage: perf_log threshold <ms>");
        return false;
      }
      s.performanceLog.spikeThresholdMs =
          std::clamp(std::stof(args[2]), 5.0f, 1000.0f);
      LOG_INFO("Console", "Performance spike threshold set to " +
                              std::to_string(s.performanceLog.spikeThresholdMs) +
                              " ms");
      return false;
    }

    LOG_WARN("Console", "Usage: perf_log <off|spikes|summary|all|threshold <ms>>");
    return false;
  }

  if (cmd == "teleport") {
    if (args.size() < 4) {
      LOG_WARN("Console", "Usage: teleport <x> <y> <z>");
      return false;
    }
    if (s.gameplay.playerId == 0 ||
        !s.scene.registry().has<TransformComponent>(s.gameplay.playerId)) {
      LOG_WARN("Console", "No player entity to teleport.");
      return false;
    }
    glm::vec3 pos(std::stof(args[1]), std::stof(args[2]),
                  std::stof(args[3]));
    auto &tr = s.scene.registry().get<TransformComponent>(s.gameplay.playerId);
    tr.position = pos;
    if (s.scene.registry().has<RigidbodyComponent>(s.gameplay.playerId)) {
      auto &rb = s.scene.registry().get<RigidbodyComponent>(s.gameplay.playerId);
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
    s.editorSubsystem->pending().requestTestFootstepAudio = true;
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
  j["skyHDRPath"] = s.skyHDRPath;
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
  j["visualSunColor"] = vec3ToJson(s.visualSunColor);
  j["visualSunDayColor"] = vec3ToJson(s.visualSunDayColor);
  j["visualSunDuskColor"] = vec3ToJson(s.visualSunDuskColor);
  j["visualSunNightColor"] = vec3ToJson(s.visualSunNightColor);
  j["useBlackHole"] = s.useBlackHole;
  j["blackHoleWorldMode"] = s.blackHoleWorldMode;
  j["blackHoleAzimuth"] = s.blackHoleAzimuth;
  j["blackHoleElevation"] = s.blackHoleElevation;
  j["blackHoleWorldPosition"] = vec3ToJson(s.blackHoleWorldPosition);
  j["blackHoleWorldRadius"] = s.blackHoleWorldRadius;
  j["blackHoleViewPitchDeg"] = s.blackHoleViewPitchDeg;
  j["blackHoleSizeDeg"] = s.blackHoleSizeDeg;
  j["blackHoleDiskTiltDeg"] = s.blackHoleDiskTiltDeg;
  j["blackHoleDiskInclinationDeg"] = s.blackHoleDiskInclinationDeg;
  j["blackHoleColor"] = vec3ToJson(s.blackHoleColor);
  j["blackHoleRingIntensity"] = s.blackHoleRingIntensity;
  j["blackHoleRingWidth"] = s.blackHoleRingWidth;
  j["blackHoleDistortion"] = s.blackHoleDistortion;
  j["blackHoleHaloIntensity"] = s.blackHoleHaloIntensity;
  j["blackHoleDiskSpinSpeed"] = s.blackHoleDiskSpinSpeed;
  j["blackHoleDiskFlowShear"] = s.blackHoleDiskFlowShear;
  j["blackHoleDiskTurbulence"] = s.blackHoleDiskTurbulence;
  j["blackHoleChromaticAberration"] = s.blackHoleChromaticAberration;
  j["blackHoleEclipseStrength"] = s.blackHoleEclipseStrength;
  j["blackHolePhotonRingIntensity"] = s.blackHolePhotonRingIntensity;
  j["blackHoleDopplerBoost"] = s.blackHoleDopplerBoost;
  j["blackHoleJetIntensity"] = s.blackHoleJetIntensity;
  j["blackHoleCoronaIntensity"] = s.blackHoleCoronaIntensity;
  j["blackHoleStarLensIntensity"] = s.blackHoleStarLensIntensity;
  j["blackHoleShadowStrength"] = s.blackHoleShadowStrength;
  j["blackHoleInnerDiskRadius"] = s.blackHoleInnerDiskRadius;
  j["blackHoleOuterDiskRadius"] = s.blackHoleOuterDiskRadius;
  j["blackHoleDiskTemperature"] = s.blackHoleDiskTemperature;
  j["blackHoleDiskDensity"] = s.blackHoleDiskDensity;
  j["blackHoleLensingStrength"] = s.blackHoleLensingStrength;
  j["blackHoleBackgroundStarIntensity"] =
      s.blackHoleBackgroundStarIntensity;
  j["blackHoleExposure"] = s.blackHoleExposure;
  j["blackHoleQuality"] = s.blackHoleQuality;
  j["skyAtmosphereStrength"] = s.skyAtmosphereStrength;
  j["skyGradientPower"] = s.skyGradientPower;
  j["skyHorizonGlow"] = s.skyHorizonGlow;
  j["skySunDiscIntensity"] = s.skySunDiscIntensity;
  j["skySunHaloIntensity"] = s.skySunHaloIntensity;
  j["skySunRaysIntensity"] = s.skySunRaysIntensity;
  j["skySunDiscSoftness"] = s.skySunDiscSoftness;
  j["skySunHaloSize"] = s.skySunHaloSize;
  j["skySunRaySharpness"] = s.skySunRaySharpness;
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
  if (j.contains("skyHDRPath"))
    s.skyHDRPath = j["skyHDRPath"].get<std::string>();
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
  loadVec3(j, "visualSunColor", s.visualSunColor);
  loadVec3(j, "visualSunDayColor", s.visualSunDayColor);
  loadVec3(j, "visualSunDuskColor", s.visualSunDuskColor);
  loadVec3(j, "visualSunNightColor", s.visualSunNightColor);
  if (j.contains("useBlackHole"))
    s.useBlackHole = j["useBlackHole"].get<bool>();
  if (j.contains("blackHoleWorldMode"))
    s.blackHoleWorldMode = j["blackHoleWorldMode"].get<bool>();
  if (j.contains("blackHoleAzimuth"))
    s.blackHoleAzimuth =
        std::clamp(j["blackHoleAzimuth"].get<float>(), 0.0f, 360.0f);
  if (j.contains("blackHoleElevation"))
    s.blackHoleElevation =
        std::clamp(j["blackHoleElevation"].get<float>(), -90.0f, 90.0f);
  loadVec3(j, "blackHoleWorldPosition", s.blackHoleWorldPosition);
  if (j.contains("blackHoleWorldRadius"))
    s.blackHoleWorldRadius =
        std::clamp(j["blackHoleWorldRadius"].get<float>(), 0.5f, 10000.0f);
  if (j.contains("blackHoleViewPitchDeg"))
    s.blackHoleViewPitchDeg =
        std::clamp(j["blackHoleViewPitchDeg"].get<float>(), -89.0f, 89.0f);
  if (j.contains("blackHoleSizeDeg"))
    s.blackHoleSizeDeg =
        std::clamp(j["blackHoleSizeDeg"].get<float>(), 0.10f, 20.0f);
  if (j.contains("blackHoleDiskTiltDeg"))
    s.blackHoleDiskTiltDeg =
        std::clamp(j["blackHoleDiskTiltDeg"].get<float>(), -180.0f, 180.0f);
  if (j.contains("blackHoleDiskInclinationDeg"))
    s.blackHoleDiskInclinationDeg = std::clamp(
        j["blackHoleDiskInclinationDeg"].get<float>(), 0.0f, 88.0f);
  loadVec3(j, "blackHoleColor", s.blackHoleColor);
  if (j.contains("blackHoleRingIntensity"))
    s.blackHoleRingIntensity =
        std::clamp(j["blackHoleRingIntensity"].get<float>(), 0.0f, 12.0f);
  if (j.contains("blackHoleRingWidth"))
    s.blackHoleRingWidth =
        std::clamp(j["blackHoleRingWidth"].get<float>(), 0.02f, 0.90f);
  if (j.contains("blackHoleDistortion"))
    s.blackHoleDistortion =
        std::clamp(j["blackHoleDistortion"].get<float>(), 0.0f, 1.0f);
  if (j.contains("blackHoleHaloIntensity"))
    s.blackHoleHaloIntensity =
        std::clamp(j["blackHoleHaloIntensity"].get<float>(), 0.0f, 4.0f);
  if (j.contains("blackHoleDiskSpinSpeed"))
    s.blackHoleDiskSpinSpeed =
        std::clamp(j["blackHoleDiskSpinSpeed"].get<float>(), 0.0f, 8.0f);
  if (j.contains("blackHoleDiskFlowShear"))
    s.blackHoleDiskFlowShear =
        std::clamp(j["blackHoleDiskFlowShear"].get<float>(), 0.0f, 1.0f);
  if (j.contains("blackHoleDiskTurbulence"))
    s.blackHoleDiskTurbulence =
        std::clamp(j["blackHoleDiskTurbulence"].get<float>(), 0.0f, 1.0f);
  if (j.contains("blackHoleChromaticAberration"))
    s.blackHoleChromaticAberration = std::clamp(
        j["blackHoleChromaticAberration"].get<float>(), 0.0f, 1.0f);
  if (j.contains("blackHoleEclipseStrength"))
    s.blackHoleEclipseStrength =
        std::clamp(j["blackHoleEclipseStrength"].get<float>(), 0.0f, 1.0f);
  if (j.contains("blackHolePhotonRingIntensity"))
    s.blackHolePhotonRingIntensity = std::clamp(
        j["blackHolePhotonRingIntensity"].get<float>(), 0.0f, 6.0f);
  if (j.contains("blackHoleDopplerBoost"))
    s.blackHoleDopplerBoost =
        std::clamp(j["blackHoleDopplerBoost"].get<float>(), 0.0f, 1.0f);
  if (j.contains("blackHoleJetIntensity"))
    s.blackHoleJetIntensity =
        std::clamp(j["blackHoleJetIntensity"].get<float>(), 0.0f, 4.0f);
  if (j.contains("blackHoleCoronaIntensity"))
    s.blackHoleCoronaIntensity =
        std::clamp(j["blackHoleCoronaIntensity"].get<float>(), 0.0f, 6.0f);
  if (j.contains("blackHoleStarLensIntensity"))
    s.blackHoleStarLensIntensity = std::clamp(
        j["blackHoleStarLensIntensity"].get<float>(), 0.0f, 4.0f);
  if (j.contains("blackHoleShadowStrength"))
    s.blackHoleShadowStrength =
        std::clamp(j["blackHoleShadowStrength"].get<float>(), 0.0f, 1.0f);
  if (j.contains("blackHoleInnerDiskRadius"))
    s.blackHoleInnerDiskRadius =
        std::clamp(j["blackHoleInnerDiskRadius"].get<float>(), 0.85f, 4.0f);
  if (j.contains("blackHoleOuterDiskRadius"))
    s.blackHoleOuterDiskRadius =
        std::clamp(j["blackHoleOuterDiskRadius"].get<float>(), 2.5f, 16.0f);
  if (j.contains("blackHoleDiskTemperature"))
    s.blackHoleDiskTemperature =
        std::clamp(j["blackHoleDiskTemperature"].get<float>(), 0.15f, 3.0f);
  if (j.contains("blackHoleDiskDensity"))
    s.blackHoleDiskDensity =
        std::clamp(j["blackHoleDiskDensity"].get<float>(), 0.0f, 3.0f);
  if (j.contains("blackHoleLensingStrength"))
    s.blackHoleLensingStrength =
        std::clamp(j["blackHoleLensingStrength"].get<float>(), 0.0f, 1.5f);
  if (j.contains("blackHoleBackgroundStarIntensity"))
    s.blackHoleBackgroundStarIntensity = std::clamp(
        j["blackHoleBackgroundStarIntensity"].get<float>(), 0.0f, 4.0f);
  if (j.contains("blackHoleExposure"))
    s.blackHoleExposure =
        std::clamp(j["blackHoleExposure"].get<float>(), 0.05f, 6.0f);
  if (j.contains("blackHoleQuality"))
    s.blackHoleQuality = std::clamp(j["blackHoleQuality"].get<int>(), 0, 2);
  if (j.contains("skyAtmosphereStrength"))
    s.skyAtmosphereStrength =
        std::clamp(j["skyAtmosphereStrength"].get<float>(), 0.0f, 1.0f);
  if (j.contains("skyGradientPower"))
    s.skyGradientPower =
        std::clamp(j["skyGradientPower"].get<float>(), 0.25f, 4.0f);
  if (j.contains("skyHorizonGlow"))
    s.skyHorizonGlow =
        std::clamp(j["skyHorizonGlow"].get<float>(), 0.0f, 2.0f);
  if (j.contains("skySunDiscIntensity"))
    s.skySunDiscIntensity =
        std::clamp(j["skySunDiscIntensity"].get<float>(), 0.0f, 80.0f);
  if (j.contains("skySunHaloIntensity"))
    s.skySunHaloIntensity =
        std::clamp(j["skySunHaloIntensity"].get<float>(), 0.0f, 8.0f);
  if (j.contains("skySunRaysIntensity"))
    s.skySunRaysIntensity =
        std::clamp(j["skySunRaysIntensity"].get<float>(), 0.0f, 4.0f);
  if (j.contains("skySunDiscSoftness"))
    s.skySunDiscSoftness =
        std::clamp(j["skySunDiscSoftness"].get<float>(), 0.0001f, 0.05f);
  if (j.contains("skySunHaloSize"))
    s.skySunHaloSize =
        std::clamp(j["skySunHaloSize"].get<float>(), 0.0f, 1.0f);
  if (j.contains("skySunRaySharpness"))
    s.skySunRaySharpness =
        std::clamp(j["skySunRaySharpness"].get<float>(), 1.0f, 40.0f);
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
  j["landscapeScale"] = s.landscapeScale;
  j["macroStrength"] = s.macroStrength;
  j["mountainSpan"] = s.mountainSpan;
  j["valleySpan"] = s.valleySpan;
  j["viewDistance"] = s.viewDistance;
  j["chunkWorldSize"] = s.chunkWorldSize;
  j["maxConcurrentChunkJobs"] = s.maxConcurrentChunkJobs;
  j["maxChunkLoadsPerUpdate"] = s.maxChunkLoadsPerUpdate;
  j["maxChunkUploadsPerFrame"] = s.maxChunkUploadsPerFrame;
  j["collisionChunkRadius"] = s.collisionChunkRadius;
  j["useGpuTerrain"] = s.useGpuTerrain;
  j["enableHorizonCulling"] = s.enableHorizonCulling;
  j["horizonCullingSectors"] = s.horizonCullingSectors;
  j["terrainGpuPageCapacity"] = s.terrainGpuPageCapacity;
  j["terrainWorkerThreads"] = s.terrainWorkerThreads;
  j["maxCompletedChunksPerFrame"] = s.maxCompletedChunksPerFrame;
  j["maxGpuUploadBytesPerFrame"] = s.maxGpuUploadBytesPerFrame;
  j["collisionUpdatesPerFrame"] = s.collisionUpdatesPerFrame;
  j["meshVegetationDistance"] = s.meshVegetationDistance;
  j["impostorVegetationDistance"] = s.impostorVegetationDistance;
  j["vegetationShadowDistance"] = s.vegetationShadowDistance;
  j["materialQuality"] = (int)s.materialQuality;
  j["useRidgeNoise"] = s.useRidgeNoise;
  j["singleBiomeOnly"] = s.singleBiomeOnly;
  j["octaves"] = s.octaves;
  j["lacunarity"] = s.lacunarity;
  j["gain"] = s.gain;
  j["treeDensity"] = s.treeDensity;
  j["interactiveTreeChunkRadius"] = s.interactiveTreeChunkRadius;
  j["interactiveTreeRatio"] = s.interactiveTreeRatio;
  j["maxInteractiveTreesPerChunk"] = s.maxInteractiveTreesPerChunk;
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
  if (j.contains("landscapeScale"))
    s.landscapeScale = j["landscapeScale"].get<float>();
  if (j.contains("macroStrength"))
    s.macroStrength = j["macroStrength"].get<float>();
  if (j.contains("mountainSpan"))
    s.mountainSpan = j["mountainSpan"].get<float>();
  if (j.contains("valleySpan"))
    s.valleySpan = j["valleySpan"].get<float>();
  if (j.contains("viewDistance"))
    s.viewDistance = j["viewDistance"].get<int>();
  if (j.contains("chunkWorldSize"))
    s.chunkWorldSize = j["chunkWorldSize"].get<float>();
  if (j.contains("maxConcurrentChunkJobs"))
    s.maxConcurrentChunkJobs = j["maxConcurrentChunkJobs"].get<int>();
  if (j.contains("maxChunkLoadsPerUpdate"))
    s.maxChunkLoadsPerUpdate = j["maxChunkLoadsPerUpdate"].get<int>();
  if (j.contains("maxChunkUploadsPerFrame"))
    s.maxChunkUploadsPerFrame = j["maxChunkUploadsPerFrame"].get<int>();
  if (j.contains("collisionChunkRadius"))
    s.collisionChunkRadius = j["collisionChunkRadius"].get<int>();
  if (j.contains("useGpuTerrain"))
    s.useGpuTerrain = j["useGpuTerrain"].get<bool>();
  if (j.contains("enableHorizonCulling"))
    s.enableHorizonCulling = j["enableHorizonCulling"].get<bool>();
  if (j.contains("horizonCullingSectors"))
    s.horizonCullingSectors = j["horizonCullingSectors"].get<int>();
  if (j.contains("terrainGpuPageCapacity"))
    s.terrainGpuPageCapacity = j["terrainGpuPageCapacity"].get<int>();
  if (j.contains("terrainWorkerThreads"))
    s.terrainWorkerThreads = j["terrainWorkerThreads"].get<int>();
  if (j.contains("maxCompletedChunksPerFrame"))
    s.maxCompletedChunksPerFrame = j["maxCompletedChunksPerFrame"].get<int>();
  if (j.contains("maxGpuUploadBytesPerFrame"))
    s.maxGpuUploadBytesPerFrame = j["maxGpuUploadBytesPerFrame"].get<int>();
  if (j.contains("collisionUpdatesPerFrame"))
    s.collisionUpdatesPerFrame = j["collisionUpdatesPerFrame"].get<int>();
  if (j.contains("meshVegetationDistance"))
    s.meshVegetationDistance = j["meshVegetationDistance"].get<int>();
  if (j.contains("impostorVegetationDistance"))
    s.impostorVegetationDistance = j["impostorVegetationDistance"].get<int>();
  if (j.contains("vegetationShadowDistance"))
    s.vegetationShadowDistance = j["vegetationShadowDistance"].get<int>();
  if (j.contains("materialQuality")) {
    const int quality = std::clamp(j["materialQuality"].get<int>(), 0, 3);
    s.materialQuality = (TerrainMaterialQuality)quality;
  }
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
  if (j.contains("interactiveTreeChunkRadius"))
    s.interactiveTreeChunkRadius = j["interactiveTreeChunkRadius"].get<int>();
  if (j.contains("interactiveTreeRatio"))
    s.interactiveTreeRatio = j["interactiveTreeRatio"].get<float>();
  if (j.contains("maxInteractiveTreesPerChunk"))
    s.maxInteractiveTreesPerChunk =
        j["maxInteractiveTreesPerChunk"].get<int>();
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
  j["useLayerTextures"] = s.useLayerTextures;
  j["grassAlbedoPath"] = s.grassAlbedoPath;
  j["grassNormalPath"] = s.grassNormalPath;
  j["grassRoughnessPath"] = s.grassRoughnessPath;
  j["dirtAlbedoPath"] = s.dirtAlbedoPath;
  j["dirtNormalPath"] = s.dirtNormalPath;
  j["dirtRoughnessPath"] = s.dirtRoughnessPath;
  j["layerTextureTiling"] = s.layerTextureTiling;
  j["layerTextureStrength"] = s.layerTextureStrength;
  j["layerNormalStrength"] = s.layerNormalStrength;
  j["layerRoughnessStrength"] = s.layerRoughnessStrength;
  j["useGroundTextures"] = s.useGroundTextures;
  j["groundAlbedoPath"] = s.groundAlbedoPath;
  j["groundNormalPath"] = s.groundNormalPath;
  j["groundRoughnessPath"] = s.groundRoughnessPath;
  j["groundHeightPath"] = s.groundHeightPath;
  j["groundTiling"] = s.groundTiling;
  j["groundBlendStrength"] = s.groundBlendStrength;
  j["groundRoughness"] = s.groundRoughness;
  j["groundHeightStrength"] = s.groundHeightStrength;
  j["groundPseudoHeightEnabled"] = s.groundPseudoHeightEnabled;
  j["groundPseudoHeightSource"] = s.groundPseudoHeightSource;
  j["groundPseudoHeightContrast"] = s.groundPseudoHeightContrast;
  j["groundPseudoHeightBias"] = s.groundPseudoHeightBias;
  j["groundGradeEnabled"] = s.groundGradeEnabled;
  j["groundGradeSaturation"] = s.groundGradeSaturation;
  j["groundGradeContrast"] = s.groundGradeContrast;
  j["groundGradeGamma"] = s.groundGradeGamma;
  j["groundGradeTint"] = vec3ToJson(s.groundGradeTint);
  j["groundBrightness"] = s.groundBrightness;
  j["groundVariationStrength"] = s.groundVariationStrength;
  j["groundVariationScale"] = s.groundVariationScale;
  j["groundFullOverride"] = s.groundFullOverride;
  j["sunGlintEnabled"] = s.sunGlintEnabled;
  j["sunGlintIntensity"] = s.sunGlintIntensity;
  j["sunGlintSharpness"] = s.sunGlintSharpness;
  j["sunGlintMaskScale"] = s.sunGlintMaskScale;
  j["sunGlintMaskStrength"] = s.sunGlintMaskStrength;
  j["sunGlintBaseSpecular"] = s.sunGlintBaseSpecular;
  j["sunGlintUseSceneSun"] = s.sunGlintUseSceneSun;
  j["sunGlintDirectionAzimuth"] = s.sunGlintDirectionAzimuth;
  j["sunGlintDirectionElevation"] = s.sunGlintDirectionElevation;
  j["sunGlintBandWidth"] = s.sunGlintBandWidth;
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
  if (j.contains("useLayerTextures"))
    s.useLayerTextures = j["useLayerTextures"].get<bool>();
  if (j.contains("grassAlbedoPath"))
    s.grassAlbedoPath = j["grassAlbedoPath"].get<std::string>();
  if (j.contains("grassNormalPath"))
    s.grassNormalPath = j["grassNormalPath"].get<std::string>();
  if (j.contains("grassRoughnessPath"))
    s.grassRoughnessPath = j["grassRoughnessPath"].get<std::string>();
  if (j.contains("dirtAlbedoPath"))
    s.dirtAlbedoPath = j["dirtAlbedoPath"].get<std::string>();
  if (j.contains("dirtNormalPath"))
    s.dirtNormalPath = j["dirtNormalPath"].get<std::string>();
  if (j.contains("dirtRoughnessPath"))
    s.dirtRoughnessPath = j["dirtRoughnessPath"].get<std::string>();
  if (j.contains("layerTextureTiling"))
    s.layerTextureTiling = j["layerTextureTiling"].get<float>();
  if (j.contains("layerTextureStrength"))
    s.layerTextureStrength = j["layerTextureStrength"].get<float>();
  if (j.contains("layerNormalStrength"))
    s.layerNormalStrength = j["layerNormalStrength"].get<float>();
  if (j.contains("layerRoughnessStrength"))
    s.layerRoughnessStrength = j["layerRoughnessStrength"].get<float>();
  if (j.contains("useGroundTextures"))
    s.useGroundTextures = j["useGroundTextures"].get<bool>();
  if (j.contains("groundAlbedoPath"))
    s.groundAlbedoPath = j["groundAlbedoPath"].get<std::string>();
  if (j.contains("groundNormalPath"))
    s.groundNormalPath = j["groundNormalPath"].get<std::string>();
  if (j.contains("groundRoughnessPath"))
    s.groundRoughnessPath = j["groundRoughnessPath"].get<std::string>();
  if (j.contains("groundHeightPath"))
    s.groundHeightPath = j["groundHeightPath"].get<std::string>();
  if (j.contains("groundTiling"))
    s.groundTiling = j["groundTiling"].get<float>();
  if (j.contains("groundBlendStrength"))
    s.groundBlendStrength = j["groundBlendStrength"].get<float>();
  if (j.contains("groundRoughness"))
    s.groundRoughness = j["groundRoughness"].get<float>();
  if (j.contains("groundHeightStrength"))
    s.groundHeightStrength = j["groundHeightStrength"].get<float>();
  if (j.contains("groundPseudoHeightEnabled"))
    s.groundPseudoHeightEnabled = j["groundPseudoHeightEnabled"].get<bool>();
  if (j.contains("groundPseudoHeightSource"))
    s.groundPseudoHeightSource = j["groundPseudoHeightSource"].get<int>();
  if (j.contains("groundPseudoHeightContrast"))
    s.groundPseudoHeightContrast = j["groundPseudoHeightContrast"].get<float>();
  if (j.contains("groundPseudoHeightBias"))
    s.groundPseudoHeightBias = j["groundPseudoHeightBias"].get<float>();
  if (j.contains("groundGradeEnabled"))
    s.groundGradeEnabled = j["groundGradeEnabled"].get<bool>();
  if (j.contains("groundGradeSaturation"))
    s.groundGradeSaturation = j["groundGradeSaturation"].get<float>();
  if (j.contains("groundGradeContrast"))
    s.groundGradeContrast = j["groundGradeContrast"].get<float>();
  if (j.contains("groundGradeGamma"))
    s.groundGradeGamma = j["groundGradeGamma"].get<float>();
  loadVec3(j, "groundGradeTint", s.groundGradeTint);
  if (j.contains("groundBrightness"))
    s.groundBrightness = j["groundBrightness"].get<float>();
  if (j.contains("groundVariationStrength"))
    s.groundVariationStrength = j["groundVariationStrength"].get<float>();
  if (j.contains("groundVariationScale"))
    s.groundVariationScale = j["groundVariationScale"].get<float>();
  if (j.contains("groundFullOverride"))
    s.groundFullOverride = j["groundFullOverride"].get<bool>();
  if (j.contains("sunGlintEnabled"))
    s.sunGlintEnabled = j["sunGlintEnabled"].get<bool>();
  if (j.contains("sunGlintIntensity"))
    s.sunGlintIntensity = j["sunGlintIntensity"].get<float>();
  if (j.contains("sunGlintSharpness"))
    s.sunGlintSharpness = j["sunGlintSharpness"].get<float>();
  if (j.contains("sunGlintMaskScale"))
    s.sunGlintMaskScale = j["sunGlintMaskScale"].get<float>();
  if (j.contains("sunGlintMaskStrength"))
    s.sunGlintMaskStrength = j["sunGlintMaskStrength"].get<float>();
  if (j.contains("sunGlintBaseSpecular"))
    s.sunGlintBaseSpecular = j["sunGlintBaseSpecular"].get<float>();
  if (j.contains("sunGlintUseSceneSun"))
    s.sunGlintUseSceneSun = j["sunGlintUseSceneSun"].get<bool>();
  if (j.contains("sunGlintDirectionAzimuth"))
    s.sunGlintDirectionAzimuth = j["sunGlintDirectionAzimuth"].get<float>();
  if (j.contains("sunGlintDirectionElevation"))
    s.sunGlintDirectionElevation =
        j["sunGlintDirectionElevation"].get<float>();
  if (j.contains("sunGlintBandWidth"))
    s.sunGlintBandWidth = j["sunGlintBandWidth"].get<float>();
  if (j.contains("flatGreenEnabled"))
    s.flatGreenEnabled = j["flatGreenEnabled"].get<bool>();
  loadVec3(j, "flatGreenColor", s.flatGreenColor);
}

json serializeRenderSettings(const RenderSettings &s) {
  json j;
  j["mixVal"] = s.mixVal;
  j["shadowStrength"] = s.shadowStrength;
  j["shadowFarPlane"] = s.shadowFarPlane;
  j["enableCascadedShadows"] = s.enableCascadedShadows;
  j["shadowCascadeCount"] = s.shadowCascadeCount;
  j["shadowMapResolution"] = s.shadowMapResolution;
  j["shadowCascadeDistance"] = s.shadowCascadeDistance;
  j["shadowCascadeLambda"] = s.shadowCascadeLambda;
  j["shadowNormalBias"] = s.shadowNormalBias;
  j["shadowDepthBias"] = s.shadowDepthBias;
  j["shadowSoftness"] = s.shadowSoftness;
  j["showShadowCascades"] = s.showShadowCascades;
  j["shadowUpdateInterval"] = s.shadowUpdateInterval;
  j["shadowUpdateDistance"] = s.shadowUpdateDistance;
  j["shadowUpdateAngle"] = s.shadowUpdateAngle;
  j["shadowStaggeredUpdates"] = s.shadowStaggeredUpdates;
  j["shadowCascadeCadence"] = s.shadowCascadeCadence;
  j["shadowCascadeDistanceScale"] = s.shadowCascadeDistanceScale;
  j["shadowCascadeAngleScale"] = s.shadowCascadeAngleScale;
  j["exposure"] = s.exposure;
  j["gamma"] = s.gamma;
  j["fogDensity"] = s.fogDensity;
  j["fogHeightFalloff"] = s.fogHeightFalloff;
  j["fogColor"] = vec3ToJson(s.fogColor);
  j["aerialPerspectiveEnabled"] = s.aerialPerspectiveEnabled;
  j["aerialPerspectiveDensity"] = s.aerialPerspectiveDensity;
  j["aerialPerspectiveStart"] = s.aerialPerspectiveStart;
  j["aerialPerspectiveHeightFalloff"] = s.aerialPerspectiveHeightFalloff;
  j["aerialPerspectiveSkyBlend"] = s.aerialPerspectiveSkyBlend;
  j["aerialPerspectiveSunGlow"] = s.aerialPerspectiveSunGlow;
  j["aerialPerspectiveDesaturation"] = s.aerialPerspectiveDesaturation;
  j["ambientHemisphereEnabled"] = s.ambientHemisphereEnabled;
  j["ambientHemisphereIntensity"] = s.ambientHemisphereIntensity;
  j["ambientSkyInfluence"] = s.ambientSkyInfluence;
  j["ambientHorizonStrength"] = s.ambientHorizonStrength;
  j["ambientTerrainBoost"] = s.ambientTerrainBoost;
  j["ambientSkyColor"] = vec3ToJson(s.ambientSkyColor);
  j["ambientHorizonColor"] = vec3ToJson(s.ambientHorizonColor);
  j["ambientGroundColor"] = vec3ToJson(s.ambientGroundColor);
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
  if (j.contains("enableCascadedShadows"))
    s.enableCascadedShadows = j["enableCascadedShadows"].get<bool>();
  if (j.contains("shadowCascadeCount"))
    s.shadowCascadeCount = std::clamp(j["shadowCascadeCount"].get<int>(), 1, 4);
  if (j.contains("shadowMapResolution"))
    s.shadowMapResolution =
        std::clamp(j["shadowMapResolution"].get<int>(), 512, 8192);
  if (j.contains("shadowCascadeDistance"))
    s.shadowCascadeDistance =
        std::max(20.0f, j["shadowCascadeDistance"].get<float>());
  if (j.contains("shadowCascadeLambda"))
    s.shadowCascadeLambda =
        std::clamp(j["shadowCascadeLambda"].get<float>(), 0.0f, 1.0f);
  if (j.contains("shadowNormalBias"))
    s.shadowNormalBias =
        std::clamp(j["shadowNormalBias"].get<float>(), 0.0f, 0.20f);
  if (j.contains("shadowDepthBias"))
    s.shadowDepthBias =
        std::clamp(j["shadowDepthBias"].get<float>(), 0.0f, 0.02f);
  if (j.contains("shadowSoftness"))
    s.shadowSoftness =
        std::clamp(j["shadowSoftness"].get<float>(), 0.2f, 4.0f);
  if (j.contains("showShadowCascades"))
    s.showShadowCascades = j["showShadowCascades"].get<bool>();
  if (j.contains("shadowUpdateInterval"))
    s.shadowUpdateInterval = j["shadowUpdateInterval"].get<int>();
  if (j.contains("shadowUpdateDistance"))
    s.shadowUpdateDistance = j["shadowUpdateDistance"].get<float>();
  if (j.contains("shadowUpdateAngle"))
    s.shadowUpdateAngle = j["shadowUpdateAngle"].get<float>();
  if (j.contains("shadowStaggeredUpdates"))
    s.shadowStaggeredUpdates = j["shadowStaggeredUpdates"].get<bool>();
  if (j.contains("shadowCascadeCadence"))
    s.shadowCascadeCadence =
        std::clamp(j["shadowCascadeCadence"].get<int>(), 1, 4);
  if (j.contains("shadowCascadeDistanceScale"))
    s.shadowCascadeDistanceScale =
        std::clamp(j["shadowCascadeDistanceScale"].get<float>(), 1.0f, 6.0f);
  if (j.contains("shadowCascadeAngleScale"))
    s.shadowCascadeAngleScale =
        std::clamp(j["shadowCascadeAngleScale"].get<float>(), 1.0f, 6.0f);
  if (j.contains("exposure"))
    s.exposure = j["exposure"].get<float>();
  if (j.contains("gamma"))
    s.gamma = j["gamma"].get<float>();
  if (j.contains("fogDensity"))
    s.fogDensity = j["fogDensity"].get<float>();
  if (j.contains("fogHeightFalloff"))
    s.fogHeightFalloff = j["fogHeightFalloff"].get<float>();
  loadVec3(j, "fogColor", s.fogColor);
  if (j.contains("aerialPerspectiveEnabled"))
    s.aerialPerspectiveEnabled = j["aerialPerspectiveEnabled"].get<bool>();
  if (j.contains("aerialPerspectiveDensity"))
    s.aerialPerspectiveDensity =
        std::clamp(j["aerialPerspectiveDensity"].get<float>(), 0.0f, 0.02f);
  if (j.contains("aerialPerspectiveStart"))
    s.aerialPerspectiveStart =
        std::clamp(j["aerialPerspectiveStart"].get<float>(), 0.0f, 1000.0f);
  if (j.contains("aerialPerspectiveHeightFalloff"))
    s.aerialPerspectiveHeightFalloff = std::clamp(
        j["aerialPerspectiveHeightFalloff"].get<float>(), 0.0f, 0.08f);
  if (j.contains("aerialPerspectiveSkyBlend"))
    s.aerialPerspectiveSkyBlend =
        std::clamp(j["aerialPerspectiveSkyBlend"].get<float>(), 0.0f, 1.0f);
  if (j.contains("aerialPerspectiveSunGlow"))
    s.aerialPerspectiveSunGlow =
        std::clamp(j["aerialPerspectiveSunGlow"].get<float>(), 0.0f, 2.0f);
  if (j.contains("aerialPerspectiveDesaturation"))
    s.aerialPerspectiveDesaturation = std::clamp(
        j["aerialPerspectiveDesaturation"].get<float>(), 0.0f, 1.0f);
  if (j.contains("ambientHemisphereEnabled"))
    s.ambientHemisphereEnabled = j["ambientHemisphereEnabled"].get<bool>();
  if (j.contains("ambientHemisphereIntensity"))
    s.ambientHemisphereIntensity =
        std::clamp(j["ambientHemisphereIntensity"].get<float>(), 0.0f, 3.0f);
  if (j.contains("ambientSkyInfluence"))
    s.ambientSkyInfluence =
        std::clamp(j["ambientSkyInfluence"].get<float>(), 0.0f, 1.0f);
  if (j.contains("ambientHorizonStrength"))
    s.ambientHorizonStrength =
        std::clamp(j["ambientHorizonStrength"].get<float>(), 0.0f, 1.5f);
  if (j.contains("ambientTerrainBoost"))
    s.ambientTerrainBoost =
        std::clamp(j["ambientTerrainBoost"].get<float>(), 0.5f, 2.5f);
  loadVec3(j, "ambientSkyColor", s.ambientSkyColor);
  loadVec3(j, "ambientHorizonColor", s.ambientHorizonColor);
  loadVec3(j, "ambientGroundColor", s.ambientGroundColor);
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

json serializePlayPerfHudSettings(const PlayPerfHudSettings &s) {
  json j;
  j["enabled"] = s.enabled;
  j["expanded"] = s.expanded;
  j["opacity"] = s.opacity;
  return j;
}

void applyPlayPerfHudSettings(const json &j, PlayPerfHudSettings &s) {
  if (j.contains("enabled"))
    s.enabled = j["enabled"].get<bool>();
  if (j.contains("expanded"))
    s.expanded = j["expanded"].get<bool>();
  if (j.contains("opacity"))
    s.opacity = std::clamp(j["opacity"].get<float>(), 0.10f, 0.95f);
}

ImVec4 perfBottleneckColor(PerformanceBottleneck bottleneck) {
  switch (bottleneck) {
  case PerformanceBottleneck::CpuDriver:
    return ImVec4(1.0f, 0.68f, 0.25f, 1.0f);
  case PerformanceBottleneck::Gpu:
    return ImVec4(0.35f, 0.85f, 0.45f, 1.0f);
  case PerformanceBottleneck::Mixed:
    return ImVec4(0.9f, 0.78f, 0.35f, 1.0f);
  case PerformanceBottleneck::WithinBudget:
    return ImVec4(0.45f, 0.75f, 1.0f, 1.0f);
  case PerformanceBottleneck::Collecting:
  default:
    return ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
  }
}

FramePerformanceSnapshot buildFramePerformanceSnapshot(AppState &s,
                                                       float dt) {
  FramePerformanceSnapshot perf;
  perf.frameMs = dt * 1000.0f;
  perf.fps = dt > 0.0f ? (1.0f / dt) : 0.0f;
  perf.gpuFrameMs = s.gpuFrameMs;
  perf.gpuShadowMs = s.gpuShadowMs;
  perf.gpuMainMs = s.gpuMainMs;
  perf.gpuMainSkyMs = s.gpuMainSkyMs;
  perf.gpuMainTerrainMs = s.gpuMainTerrainMs;
  perf.gpuMainSceneMs = s.gpuMainSceneMs;
  perf.gpuMainPostMs = s.gpuMainPostMs;
  perf.gpuTimerReady = perf.gpuFrameMs > 0.05f;
  perf.cpuGpuGapMs =
      perf.gpuTimerReady ? std::max(0.0f, perf.frameMs - perf.gpuFrameMs) : 0.0f;
  if (!perf.gpuTimerReady) {
    perf.bottleneck = PerformanceBottleneck::Collecting;
  } else if (perf.frameMs > 16.7f && perf.gpuFrameMs < perf.frameMs * 0.65f) {
    perf.bottleneck = PerformanceBottleneck::CpuDriver;
  } else if (perf.gpuFrameMs > perf.frameMs * 0.82f) {
    perf.bottleneck = PerformanceBottleneck::Gpu;
  } else if (perf.frameMs > 16.7f) {
    perf.bottleneck = PerformanceBottleneck::Mixed;
  } else {
    perf.bottleneck = PerformanceBottleneck::WithinBudget;
  }

  perf.entityCount = (int)s.scene.registry().view<TransformComponent>().size();
  perf.particleCount = (int)s.projectiles.count();

  const auto &renderStats = s.renderSystem.stats();
  perf.visibleDrawn = renderStats.drawn;
  perf.visibleCulled = renderStats.culled;
  perf.drawCallsMain = renderStats.drawCallsMain;
  perf.drawCallsShadow = renderStats.drawCallsShadow;
  perf.instancedDrawCallsMain = renderStats.instancedDrawCallsMain;
  perf.instancedDrawCallsShadow = renderStats.instancedDrawCallsShadow;
  perf.instancedUploadsMain = renderStats.instancedUploadsMain;
  perf.instancedUploadsShadow = renderStats.instancedUploadsShadow;
  perf.instancedUploadSkipsMain = renderStats.instancedUploadSkipsMain;
  perf.instancedUploadSkipsShadow = renderStats.instancedUploadSkipsShadow;
  perf.instancedUploadBytesMain = renderStats.instancedUploadBytesMain;
  perf.instancedUploadBytesShadow = renderStats.instancedUploadBytesShadow;
  perf.instancedClustersTestedMain = renderStats.instancedClustersTestedMain;
  perf.instancedClustersTestedShadow = renderStats.instancedClustersTestedShadow;
  perf.instancedClustersVisibleMain = renderStats.instancedClustersVisibleMain;
  perf.instancedClustersVisibleShadow = renderStats.instancedClustersVisibleShadow;
  perf.shadowDistanceCulled = renderStats.shadowDistanceCulled;
  perf.shadowSmallCasterCulled = renderStats.shadowSmallCasterCulled;
  perf.shadowCascadeCount = s.render.activeShadowCascadeCount;
  perf.shadowCascadesUpdated = s.shadowCascadesUpdated;
  perf.shadowCascadeStaggered = s.shadowCascadeStaggered;

  const auto &terrainStats = s.terrainSystem.stats();
  perf.terrainDrawCalls = terrainStats.terrainDrawCalls;
  perf.terrainVisibleChunks = terrainStats.visibleChunks;
  perf.terrainFrustumCulledChunks = terrainStats.frustumCulledChunks;
  perf.terrainHorizonCulledChunks = terrainStats.horizonCulledChunks;
  perf.terrainGpuPagesUsed = terrainStats.terrainGpuPagesUsed;
  perf.terrainUploads = terrainStats.terrainGpuInstanceUploads;
  perf.terrainUploadSkips = terrainStats.terrainGpuInstanceUploadSkips;
  perf.terrainUploadBytes = terrainStats.terrainGpuInstanceUploadBytes;
  perf.terrainPendingJobs = terrainStats.pendingJobs;
  perf.terrainPendingUploads = terrainStats.pendingUploads;
  perf.terrainCollisionBodies = terrainStats.collisionBodies;
  perf.gpuTerrainActive = terrainStats.gpuTerrainActive;
  perf.gpuTerrainFallback = terrainStats.gpuTerrainFallback;

  perf.glProgramBinds = s.glProgramBinds;
  perf.glTextureBinds = s.glTextureBinds;
  perf.glVaoBinds = s.glVaoBinds;
  perf.glStateChanges = s.glStateChanges;

  perf.submissionBackendLabel =
      renderSubmissionBackendLabel(s.renderer.capabilities().preferredSubmissionBackend);
  perf.modernSubmissionBackend =
      s.renderer.capabilities().preferredSubmissionBackend ==
      RenderSubmissionBackend::Modern;
  perf.renderPassOrder = s.lastRenderPassOrder;
  return perf;
}

void drawPlayPerformanceHud(const AppState &s) {
  if (s.playState != AppState::PlayState::Playing || !s.playPerfHud.enabled)
    return;

  const FramePerformanceSnapshot &perf = s.performance;
  ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowBgAlpha(s.playPerfHud.opacity);
  ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 12.0f,
             viewport->WorkPos.y + 12.0f),
      ImGuiCond_Always, ImVec2(1.0f, 0.0f));
  ImGui::Begin("PlayPerfHUD", nullptr,
               ImGuiWindowFlags_NoDecoration |
                   ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing |
                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);

  ImGui::Text("FPS %.1f | Frame %.2f ms", perf.fps, perf.frameMs);
  ImGui::Text("GPU %.2f | Main %.2f | Shadow %.2f", perf.gpuFrameMs,
              perf.gpuMainMs, perf.gpuShadowMs);
  ImGui::TextColored(perfBottleneckColor(perf.bottleneck), "%s",
                     performanceBottleneckLabel(perf.bottleneck));
  ImGui::Text("Draws M:%d I:%d T:%d", perf.drawCallsMain,
              perf.instancedDrawCallsMain, perf.terrainDrawCalls);
  ImGui::Text("Terrain uploads %d/%d | %.1f KB", perf.terrainUploads,
              perf.terrainUploadSkips, perf.terrainUploadBytes / 1024.0f);

  if (s.playPerfHud.expanded) {
    ImGui::Separator();
    ImGui::Text("Backend: %s", perf.submissionBackendLabel.c_str());
    ImGui::Text("GPU split Sky %.2f | Terrain %.2f", perf.gpuMainSkyMs,
                perf.gpuMainTerrainMs);
    ImGui::Text("GPU split Scene %.2f | Post %.2f", perf.gpuMainSceneMs,
                perf.gpuMainPostMs);
    ImGui::Text("Chunks vis %d | pages %d | frustum %d | horizon %d",
                perf.terrainVisibleChunks, perf.terrainGpuPagesUsed,
                perf.terrainFrustumCulledChunks,
                perf.terrainHorizonCulledChunks);
    ImGui::Text("Pending jobs %d | uploads %d | collision %d",
                perf.terrainPendingJobs, perf.terrainPendingUploads,
                perf.terrainCollisionBodies);
    ImGui::Text("Inst uploads M:%d S:%d | skip M:%d S:%d",
                perf.instancedUploadsMain, perf.instancedUploadsShadow,
                perf.instancedUploadSkipsMain, perf.instancedUploadSkipsShadow);
    ImGui::Text("Inst clusters M:%d/%d | S:%d/%d",
                perf.instancedClustersVisibleMain,
                perf.instancedClustersTestedMain,
                perf.instancedClustersVisibleShadow,
                perf.instancedClustersTestedShadow);
    const char *shadowRefreshMode =
        perf.shadowCascadesUpdated <= 0
            ? "reused"
            : (perf.shadowCascadeStaggered ? "staggered" : "full");
    ImGui::Text("Shadow cascades %d/%d | %s", perf.shadowCascadesUpdated,
                std::max(perf.shadowCascadeCount, 1), shadowRefreshMode);
    ImGui::Text("Shadow culls dist:%d small:%d", perf.shadowDistanceCulled,
                perf.shadowSmallCasterCulled);
    ImGui::Text("Inst KB M:%.1f | S:%.1f",
                perf.instancedUploadBytesMain / 1024.0f,
                perf.instancedUploadBytesShadow / 1024.0f);
    ImGui::Text("GL binds P:%d T:%d V:%d S:%d", perf.glProgramBinds,
                perf.glTextureBinds, perf.glVaoBinds, perf.glStateChanges);
    if (!perf.renderPassOrder.empty()) {
      std::string order;
      for (size_t i = 0; i < perf.renderPassOrder.size(); ++i) {
        if (i > 0)
          order += " -> ";
        order += perf.renderPassOrder[i];
      }
      ImGui::TextWrapped("Passes: %s", order.c_str());
    }
  }

  ImGui::End();
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
  j["ssaoIntensity"] = s.ssaoIntensity;
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
  j["enableAutoExposure"] = s.enableAutoExposure;
  j["autoExposureMin"] = s.autoExposureMin;
  j["autoExposureMax"] = s.autoExposureMax;
  j["autoExposureSpeed"] = s.autoExposureSpeed;
  j["autoExposureTarget"] = s.autoExposureTarget;
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
  if (j.contains("ssaoIntensity"))
    s.ssaoIntensity = j["ssaoIntensity"].get<float>();
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
  if (j.contains("enableAutoExposure"))
    s.enableAutoExposure = j["enableAutoExposure"].get<bool>();
  if (j.contains("autoExposureMin"))
    s.autoExposureMin = j["autoExposureMin"].get<float>();
  if (j.contains("autoExposureMax"))
    s.autoExposureMax = j["autoExposureMax"].get<float>();
  if (j.contains("autoExposureSpeed"))
    s.autoExposureSpeed = j["autoExposureSpeed"].get<float>();
  if (j.contains("autoExposureTarget"))
    s.autoExposureTarget = j["autoExposureTarget"].get<float>();
}

json serializeProjectDefaults(const AppState &s) {
  json root;
  root["version"] = 1;
  root["renderSettings"] = serializeRenderSettings(s.render);
  root["audioSettings"] = serializeAudioSettings(s.audioSubsystem->settings());
  root["playPerfHud"] = serializePlayPerfHudSettings(s.playPerfHud);
  root["sunSettings"] = serializeSunSettings(s.sun);
  root["skySettings"] = serializeSkySettings(s.skyUI);
  root["postProcess"] = serializePostProcess(s.postProcessor);
  root["inputSettings"] = json{{"walkStep", s.input.walkStep},
                               {"runMult", s.input.runMult},
                               {"jumpStrength", s.input.jumpStrength},
                               {"gravity", s.input.gravity},
                               {"freezePhysics", s.input.freezePhysics},
                               {"creativeFlight", s.input.creativeFlight},
                               {"mouseSensitivity", s.input.mouseSensitivity},
                               {"fov", s.input.fov}};
  root["viewmodelSettings"] = json{
      {"axeEnabled", s.gameplay.viewmodel.axeEnabled},
      {"axeOffset", vec3ToJson(s.gameplay.viewmodel.axeOffset)},
      {"axeRotation", vec3ToJson(s.gameplay.viewmodel.axeRotation)},
      {"axeScale", vec3ToJson(s.gameplay.viewmodel.axeScale)},
      {"torchEnabled", s.gameplay.viewmodel.torchEnabled},
      {"torchOffset", vec3ToJson(s.gameplay.viewmodel.torchOffset)},
      {"torchRotation", vec3ToJson(s.gameplay.viewmodel.torchRotation)},
      {"torchScale", vec3ToJson(s.gameplay.viewmodel.torchScale)},
      {"usePlayerCameraInEdit", s.gameplay.viewmodel.usePlayerCameraInEdit},
      {"activeSlot", s.gameplay.activeSlot == GameplayState::HotbarSlot::Torch ? 2 : 1}};
  return root;
}

void applyProjectDefaults(const json &root, AppState &s) {
  if (root.contains("renderSettings"))
    applyRenderSettings(root["renderSettings"], s.render);
  if (root.contains("audioSettings"))
    applyAudioSettings(root["audioSettings"], s.audioSubsystem->settings());
  if (root.contains("playPerfHud"))
    applyPlayPerfHudSettings(root["playPerfHud"], s.playPerfHud);
  if (root.contains("sunSettings"))
    applySunSettings(root["sunSettings"], s.sun);
  if (root.contains("skySettings"))
    applySkySettings(root["skySettings"], s.skyUI);
  if (root.contains("postProcess"))
    applyPostProcess(root["postProcess"], s.postProcessor);
  if (root.contains("inputSettings") && root["inputSettings"].is_object()) {
    const json &input = root["inputSettings"];
    if (input.contains("walkStep"))
      s.input.walkStep = input["walkStep"].get<float>();
    if (input.contains("runMult"))
      s.input.runMult = input["runMult"].get<float>();
    if (input.contains("jumpStrength"))
      s.input.jumpStrength = input["jumpStrength"].get<float>();
    if (input.contains("gravity"))
      s.input.gravity = input["gravity"].get<float>();
    if (input.contains("freezePhysics"))
      s.input.freezePhysics = input["freezePhysics"].get<bool>();
    if (input.contains("creativeFlight"))
      s.input.creativeFlight = input["creativeFlight"].get<bool>();
    if (input.contains("mouseSensitivity"))
      s.input.mouseSensitivity = input["mouseSensitivity"].get<float>();
    if (input.contains("fov"))
      s.input.fov = input["fov"].get<float>();
  }
  if (root.contains("viewmodelSettings") &&
      root["viewmodelSettings"].is_object()) {
    const json &vm = root["viewmodelSettings"];
    if (vm.contains("axeEnabled"))
      s.gameplay.viewmodel.axeEnabled = vm["axeEnabled"].get<bool>();
    loadVec3(vm, "axeOffset", s.gameplay.viewmodel.axeOffset);
    loadVec3(vm, "axeRotation", s.gameplay.viewmodel.axeRotation);
    loadVec3(vm, "axeScale", s.gameplay.viewmodel.axeScale);
    if (vm.contains("torchEnabled"))
      s.gameplay.viewmodel.torchEnabled = vm["torchEnabled"].get<bool>();
    loadVec3(vm, "torchOffset", s.gameplay.viewmodel.torchOffset);
    loadVec3(vm, "torchRotation", s.gameplay.viewmodel.torchRotation);
    loadVec3(vm, "torchScale", s.gameplay.viewmodel.torchScale);
    if (vm.contains("usePlayerCameraInEdit"))
      s.gameplay.viewmodel.usePlayerCameraInEdit = vm["usePlayerCameraInEdit"].get<bool>();
    if (vm.contains("activeSlot")) {
      s.gameplay.activeSlot = vm["activeSlot"].get<int>() == 2
                         ? GameplayState::HotbarSlot::Torch
                         : GameplayState::HotbarSlot::Axe;
    }
  }
}

bool loadProjectDefaults(AppState &s) {
  std::ifstream in(kProjectDefaultsPath);
  if (!in.is_open())
    return false;

  json root = json::parse(in, nullptr, false);
  if (root.is_discarded()) {
    LOG_WARN("Runtime", "Failed to parse project_defaults.json");
    return false;
  }

  applyProjectDefaults(root, s);
  LOG_INFO("Runtime", "Loaded project defaults from project_defaults.json");
  return true;
}

bool saveProjectDefaults(const AppState &s) {
  std::ofstream out(kProjectDefaultsPath);
  if (!out.is_open())
    return false;
  out << serializeProjectDefaults(s).dump(2);
  return true;
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

glm::vec3 playCameraForward(const glm::vec3 &rotationDeg) {
  glm::vec3 front;
  front.x = -std::sin(glm::radians(rotationDeg.y)) *
            std::cos(glm::radians(rotationDeg.x));
  front.y = std::sin(glm::radians(rotationDeg.x));
  front.z = -std::cos(glm::radians(rotationDeg.y)) *
            std::cos(glm::radians(rotationDeg.x));
  return glm::normalize(front);
}

bool damageDestructibleHit(AppState &state, Registry &reg,
                           const PhysicsRaycastResult &hit,
                           const glm::vec3 &impulseDirection, float damage) {
  if (!hit.hit || hit.entityId == 0 ||
      !reg.has<DestructibleComponent>(hit.entityId))
    return false;

  auto &destructible = reg.get<DestructibleComponent>(hit.entityId);
  if (!destructible.enabled || destructible.fractured)
    return false;

  destructible.health = std::max(0.0f, destructible.health - damage);
  if (destructible.health <= 0.0f) {
    DestructionSystem::fractureEntity(state.scene, state.assets, hit.entityId,
                                      hit.position, impulseDirection,
                                      &state.physicsSystem);
  }
  return true;
}
} // namespace

bool CoreAppLayer::initialize() {
  mState.events.subscribe<SaveConfigRequestedEvent>(
      [this](const SaveConfigRequestedEvent &) {
        mState.editorSubsystem->pending().requestSaveConfig = true;
      });
  mState.events.subscribe<LoadConfigRequestedEvent>(
      [this](const LoadConfigRequestedEvent &) {
        mState.editorSubsystem->pending().requestLoadConfig = true;
      });
  mState.events.subscribe<SaveProjectConfigRequestedEvent>(
      [this](const SaveProjectConfigRequestedEvent &) {
        mState.editorSubsystem->pending().requestSaveProjectConfig = true;
      });
  mState.events.subscribe<SaveProjectDefaultsRequestedEvent>(
      [this](const SaveProjectDefaultsRequestedEvent &) {
        mState.editorSubsystem->pending().requestSaveProjectDefaults = true;
      });
  mState.events.subscribe<ResetProjectDefaultsRequestedEvent>(
      [this](const ResetProjectDefaultsRequestedEvent &) {
        mState.editorSubsystem->pending().requestResetProjectDefaults = true;
      });
  mState.events.subscribe<SpawnEntityRequestedEvent>(
      [this](const SpawnEntityRequestedEvent &e) {
        mState.editorSubsystem->pending().pendingSpawnPaths.push_back(e.path);
      });
  mState.events.subscribe<CreateEmptyEntityRequestedEvent>(
      [this](const CreateEmptyEntityRequestedEvent &e) {
        mState.editorSubsystem->pending().pendingEmptyEntityNames.push_back(e.name);
      });
  mState.events.subscribe<DeleteEntityRequestedEvent>(
      [this](const DeleteEntityRequestedEvent &e) {
        mState.editorSubsystem->pending().pendingDeleteEntityIds.push_back(e.entityId);
      });
  mState.events.subscribe<SaveSceneRequestedEvent>(
      [this](const SaveSceneRequestedEvent &e) {
        mState.editorSubsystem->pending().pendingSceneSavePath = e.path;
      });
  mState.events.subscribe<LoadSceneRequestedEvent>(
      [this](const LoadSceneRequestedEvent &e) {
        mState.editorSubsystem->pending().pendingSceneLoadPath = e.path;
      });
  mState.events.subscribe<UndoRequestedEvent>(
      [this](const UndoRequestedEvent &) {
        mState.editorSubsystem->history().requestUndo = true;
      });
  mState.events.subscribe<RedoRequestedEvent>(
      [this](const RedoRequestedEvent &) {
        mState.editorSubsystem->history().requestRedo = true;
      });
  mState.events.subscribe<SceneHistoryJumpRequestedEvent>(
      [this](const SceneHistoryJumpRequestedEvent &e) {
        mState.editorSubsystem->history().requestHistoryJump = e.index;
      });

  loadProjectDefaults(mState);

  commitHistorySnapshot("Initial");

  if (!mState.projectConfig.startupScene.empty()) {
    const std::string startupScenePath =
        mState.projectConfig.projectPath(mState.projectConfig.startupScene);
    if (std::filesystem::exists(startupScenePath)) {
      mState.editorSubsystem->pending().pendingSceneLoadPath = startupScenePath;
      LOG_INFO("Runtime", "Queued startup scene: " + startupScenePath);
    } else {
      LOG_WARN("Runtime",
               "Startup scene not found: " + startupScenePath);
    }
  }

  return true;
}

void CoreAppLayer::shutdown() {}

void CoreAppLayer::applyHistorySnapshot(int idx) {
  if (idx < 0 || idx >= (int)mState.editorSubsystem->history().historySnapshots.size())
    return;
  if (!mState.scene.loadFromString(mState.editorSubsystem->history().historySnapshots[idx]))
    return;
  mState.editorSubsystem->history().historyCursor = idx;
  mState.editorSubsystem->selection().selectedEntityId = 0;
  mState.editorSubsystem->selection().selectedEntities.clear();
  mState.editorSubsystem->selection().lastClickedEntity = 0;
  mState.gameplay.playerId = 0;
  for (auto e : mState.scene.registry().view<CameraComponent>()) {
    if (!mState.scene.registry().has<LifecycleComponent>(e) ||
        mState.scene.registry().get<LifecycleComponent>(e).state ==
            EntityLifecycleState::Alive) {
      mState.gameplay.playerId = e;
      break;
    }
  }
}

void CoreAppLayer::commitHistorySnapshot(const std::string &label) {
  const std::string snap = mState.scene.serializeToString();
  if (mState.editorSubsystem->history().historyCursor >= 0 &&
      mState.editorSubsystem->history().historyCursor <
          (int)mState.editorSubsystem->history().historySnapshots.size() &&
      mState.editorSubsystem->history().historySnapshots[mState.editorSubsystem->history().historyCursor] == snap)
    return;

  if (mState.editorSubsystem->history().historyCursor + 1 <
      (int)mState.editorSubsystem->history().historySnapshots.size()) {
    mState.editorSubsystem->history().historySnapshots.erase(
        mState.editorSubsystem->history().historySnapshots.begin() + mState.editorSubsystem->history().historyCursor +
            1,
        mState.editorSubsystem->history().historySnapshots.end());
    mState.editorSubsystem->history().historyLabels.erase(mState.editorSubsystem->history().historyLabels.begin() +
                                           mState.editorSubsystem->history().historyCursor + 1,
                                       mState.editorSubsystem->history().historyLabels.end());
  }

  mState.editorSubsystem->history().historySnapshots.push_back(snap);
  mState.editorSubsystem->history().historyLabels.push_back(label);
  mState.editorSubsystem->history().historyCursor =
      (int)mState.editorSubsystem->history().historySnapshots.size() - 1;

  const int maxHistory = 128;
  if ((int)mState.editorSubsystem->history().historySnapshots.size() > maxHistory) {
    const int trim = (int)mState.editorSubsystem->history().historySnapshots.size() - maxHistory;
    mState.editorSubsystem->history().historySnapshots.erase(
        mState.editorSubsystem->history().historySnapshots.begin(),
        mState.editorSubsystem->history().historySnapshots.begin() + trim);
    mState.editorSubsystem->history().historyLabels.erase(mState.editorSubsystem->history().historyLabels.begin(),
                                       mState.editorSubsystem->history().historyLabels.begin() +
                                           trim);
    mState.editorSubsystem->history().historyCursor -= trim;
    if (mState.editorSubsystem->history().historyCursor < 0)
      mState.editorSubsystem->history().historyCursor = 0;
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

  auto& selState = mState.editorSubsystem->selection();

  EditorContext ctx{
      mState.uiMode,
      mState.input.walkStep,
      mState.input.runMult,
      mState.input.jumpStrength,
      mState.input.gravity,
      mState.input.freezePhysics,
      mState.input.creativeFlight,
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
      mState.terrainSystem.brushSettings(),
      mState.skyUI.solidSky,
      mState.skyUI.skyHDRPath,
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
      mState.skyUI.visualSunColor,
      mState.skyUI.visualSunDayColor,
      mState.skyUI.visualSunDuskColor,
      mState.skyUI.visualSunNightColor,
      mState.skyUI.useBlackHole,
      mState.skyUI.blackHoleWorldMode,
      mState.skyUI.blackHoleAzimuth,
      mState.skyUI.blackHoleElevation,
      mState.skyUI.blackHoleWorldPosition,
      mState.skyUI.blackHoleWorldRadius,
      mState.skyUI.blackHoleViewPitchDeg,
      mState.skyUI.blackHoleSizeDeg,
      mState.skyUI.blackHoleDiskTiltDeg,
      mState.skyUI.blackHoleDiskInclinationDeg,
      mState.skyUI.blackHoleColor,
      mState.skyUI.blackHoleRingIntensity,
      mState.skyUI.blackHoleRingWidth,
      mState.skyUI.blackHoleDistortion,
      mState.skyUI.blackHoleHaloIntensity,
      mState.skyUI.blackHoleDiskSpinSpeed,
      mState.skyUI.blackHoleDiskFlowShear,
      mState.skyUI.blackHoleDiskTurbulence,
      mState.skyUI.blackHoleChromaticAberration,
      mState.skyUI.blackHoleEclipseStrength,
      mState.skyUI.blackHolePhotonRingIntensity,
      mState.skyUI.blackHoleDopplerBoost,
      mState.skyUI.blackHoleJetIntensity,
      mState.skyUI.blackHoleCoronaIntensity,
      mState.skyUI.blackHoleStarLensIntensity,
      mState.skyUI.blackHoleShadowStrength,
      mState.skyUI.blackHoleInnerDiskRadius,
      mState.skyUI.blackHoleOuterDiskRadius,
      mState.skyUI.blackHoleDiskTemperature,
      mState.skyUI.blackHoleDiskDensity,
      mState.skyUI.blackHoleLensingStrength,
      mState.skyUI.blackHoleBackgroundStarIntensity,
      mState.skyUI.blackHoleExposure,
      mState.skyUI.blackHoleQuality,
      mState.skyUI.skyAtmosphereStrength,
      mState.skyUI.skyGradientPower,
      mState.skyUI.skyHorizonGlow,
      mState.skyUI.skySunDiscIntensity,
      mState.skyUI.skySunHaloIntensity,
      mState.skyUI.skySunRaysIntensity,
      mState.skyUI.skySunDiscSoftness,
      mState.skyUI.skySunHaloSize,
      mState.skyUI.skySunRaySharpness,
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
      mState.render.enableCascadedShadows,
      mState.render.shadowCascadeCount,
      mState.render.shadowMapResolution,
      mState.render.shadowCascadeDistance,
      mState.render.shadowCascadeLambda,
      mState.render.shadowNormalBias,
      mState.render.shadowDepthBias,
      mState.render.shadowSoftness,
      mState.render.showShadowCascades,
      mState.render.shadowUpdateInterval,
      mState.render.shadowUpdateDistance,
      mState.render.shadowUpdateAngle,
      mState.render.shadowStaggeredUpdates,
      mState.render.shadowCascadeCadence,
      mState.render.shadowCascadeDistanceScale,
      mState.render.shadowCascadeAngleScale,
      mState.render.shadowCameraCulling,
      mState.render.exposure,
      mState.render.gamma,
      mState.render.fogDensity,
      mState.render.fogHeightFalloff,
      mState.render.fogColor,
      mState.render.aerialPerspectiveEnabled,
      mState.render.aerialPerspectiveDensity,
      mState.render.aerialPerspectiveStart,
      mState.render.aerialPerspectiveHeightFalloff,
      mState.render.aerialPerspectiveSkyBlend,
      mState.render.aerialPerspectiveSunGlow,
      mState.render.aerialPerspectiveDesaturation,
      mState.render.ambientHemisphereEnabled,
      mState.render.ambientHemisphereIntensity,
      mState.render.ambientSkyInfluence,
      mState.render.ambientHorizonStrength,
      mState.render.ambientTerrainBoost,
      mState.render.ambientSkyColor,
      mState.render.ambientHorizonColor,
      mState.render.ambientGroundColor,
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
      mState.gameplay,
      mState.audioSubsystem->settings(),
      mState.audioSubsystem->backendAvailable(),
      mState.audioSubsystem->status(),
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
      mState.editorSubsystem->history(),
      mState.editorSubsystem->pending(),
      &mState.profiler.samples(),
      mState.gpuFrameMs,
      mState.gpuShadowMs,
      mState.gpuMainMs,
      mState.glProgramBinds,
      mState.glTextureBinds,
      mState.glVaoBinds,
      mState.glStateChanges,
      mState.performance,
      selState,
      (int &)mState.playState};

  EditorUIOutput uiOut{};
  {
    ScopedCpuTimer timer(mState.profiler, "Editor UI");
    uiOut = mState.editor.draw(ctx);
  }
  if (!uiOut.consoleCommands.empty()) {
    for (auto &cmd : uiOut.consoleCommands) {
      mState.editorSubsystem->pending().pendingConsoleCommands.push_back(cmd);
    }
  }
  if (Keyboard::keyWentDown(GLFW_KEY_GRAVE_ACCENT)) {
    mState.editor.toggleConsole();
  }
  if (Keyboard::keyWentDown(GLFW_KEY_F3)) {
    const bool shiftHeld =
        Keyboard::key(GLFW_KEY_LEFT_SHIFT) || Keyboard::key(GLFW_KEY_RIGHT_SHIFT);
    if (shiftHeld) {
      if (!mState.playPerfHud.enabled)
        mState.playPerfHud.enabled = true;
      mState.playPerfHud.expanded = !mState.playPerfHud.expanded;
    } else {
      mState.playPerfHud.enabled = !mState.playPerfHud.enabled;
    }
  }
  if (!uiOut.wantCaptureKeyboard) {
    if (Keyboard::keyWentDown(GLFW_KEY_1))
      mState.gameplay.activeSlot = GameplayState::HotbarSlot::Axe;
    if (Keyboard::keyWentDown(GLFW_KEY_2))
      mState.gameplay.activeSlot = GameplayState::HotbarSlot::Torch;
  }
  if (uiOut.sceneModified) {
    mState.editorSubsystem->history().pendingHistoryCommit = true;
    mState.editorSubsystem->history().pendingHistoryLabel = "Edit Scene";
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
    ImGui::Text("dx: %.2f  dy: %.2f", mState.gameplay.debug.debugMouseDX,
                mState.gameplay.debug.debugMouseDY);
    ImGui::Text("yaw: %.2f  pitch: %.2f", mState.gameplay.debug.debugYaw,
                mState.gameplay.debug.debugPitch);
    ImGui::Text("front: %.2f %.2f %.2f", mState.gameplay.debug.debugCamFront.x,
                mState.gameplay.debug.debugCamFront.y, mState.gameplay.debug.debugCamFront.z);
    ImGui::Text("up: %.2f %.2f %.2f", mState.gameplay.debug.debugCamUp.x, mState.gameplay.debug.debugCamUp.y,
                mState.gameplay.debug.debugCamUp.z);
    ImGui::Text("gameplay: %s  id: %u  dist: %.2f",
                mState.gameplay.debug.debugGameplayHitKind.empty()
                    ? "Idle"
                    : mState.gameplay.debug.debugGameplayHitKind.c_str(),
                mState.gameplay.debug.debugGameplayHitId, mState.gameplay.debug.debugGameplayHitDist);
    if (!mState.gameplay.debug.debugGameplayHitName.empty())
      ImGui::Text("hitName: %s", mState.gameplay.debug.debugGameplayHitName.c_str());
    if (!mState.gameplay.debug.debugGameplayMissReason.empty())
      ImGui::Text("miss: %s", mState.gameplay.debug.debugGameplayMissReason.c_str());
    ImGui::Text("grabbed: %u", mState.gameplay.grab.grabbedEntityId);
    if (!mState.gameplay.debug.debugGrabPrefab.empty())
      ImGui::Text("prefab: %s  idx: %d  moved: %s",
                  mState.gameplay.debug.debugGrabPrefab.c_str(), mState.gameplay.debug.debugGrabInstance,
                  mState.gameplay.debug.debugGrabMoved ? "yes" : "no");
    ImGui::End();
  }
  drawPlayPerformanceHud(mState);

  // Handle Play mode cursor locking and ESC to pause
  if (mState.playState == AppState::PlayState::Playing &&
      Keyboard::keyWentDown(GLFW_KEY_ESCAPE)) {
    mState.playState = AppState::PlayState::Paused;
  }

  static AppState::PlayState lastPlayState = AppState::PlayState::Stopped;
  static bool usingRawMouse = false;
  if (mState.playState == AppState::PlayState::Playing &&
      lastPlayState != AppState::PlayState::Playing) {
    auto &reg = mState.scene.registry();
    if (mState.gameplay.playerId == 0 || !reg.has<CameraComponent>(mState.gameplay.playerId)) {
      mState.gameplay.playerId = 0;
      for (auto e : reg.view<CameraComponent>()) {
        if (!reg.has<LifecycleComponent>(e) ||
            reg.get<LifecycleComponent>(e).state ==
                EntityLifecycleState::Alive) {
          mState.gameplay.playerId = e;
          break;
        }
      }
    }
    if (mState.gameplay.playerId != 0 && reg.has<TransformComponent>(mState.gameplay.playerId)) {
      auto &tr = reg.get<TransformComponent>(mState.gameplay.playerId);
      tr.position = mState.editorCamera.getPosition();
      tr.rotation =
          glm::vec3(mState.editorCamera.pitch, mState.editorCamera.yaw, 0.0f);
      if (mState.terrainSystem.isEnabled() && !mState.input.creativeFlight)
        clampEntityToTerrain(mState, reg, mState.gameplay.playerId);
    }

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
    mState.gameplay.woodCount = 0;
    // Lock player rotation only during play to avoid physics overwrites.
    if (mState.gameplay.playerId != 0 && reg.has<RigidbodyComponent>(mState.gameplay.playerId)) {
      reg.get<RigidbodyComponent>(mState.gameplay.playerId).lockRotation = true;
    }
    mState.playerController.reset();
    mState.playerInteraction.reset();
  } else if (mState.playState != AppState::PlayState::Playing &&
             lastPlayState == AppState::PlayState::Playing) {
    glfwSetInputMode(mState.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    if (usingRawMouse)
      glfwSetInputMode(mState.window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
    Mouse::setManualMode(false);
    Mouse::resetDeltas();
    auto &reg = mState.scene.registry();
    if (mState.gameplay.playerId != 0 && reg.has<RigidbodyComponent>(mState.gameplay.playerId)) {
      reg.get<RigidbodyComponent>(mState.gameplay.playerId).lockRotation = false;
    }
    mState.playerController.reset();
    mState.playerInteraction.reset();
  }

  lastPlayState = mState.playState;

  bool sceneMutatedByCommands = false;
  {
    ScopedCpuTimer timer(mState.profiler, "Commands/Scene");
    if (!mState.editorSubsystem->pending().pendingConsoleCommands.empty()) {
      for (const auto &cmd : mState.editorSubsystem->pending().pendingConsoleCommands) {
        if (executeConsoleCommand(mState, cmd))
          sceneMutatedByCommands = true;
      }
      mState.editorSubsystem->pending().pendingConsoleCommands.clear();
    }
    if (mState.editorSubsystem->pending().requestSaveConfig) {
      saveConfig(mState, "editor_state.bin");
      mState.editorSubsystem->pending().requestSaveConfig = false;
    }
    if (mState.editorSubsystem->pending().requestLoadConfig) {
      loadConfig(mState, "editor_state.bin");
      mState.editorSubsystem->pending().requestLoadConfig = false;
    }
    if (mState.editorSubsystem->pending().requestSaveProjectConfig) {
      if (!mState.projectConfig.saveToFile("project_config.json")) {
        LOG_ERROR("Runtime", "Failed to save project_config.json");
      }
      mState.editorSubsystem->pending().requestSaveProjectConfig = false;
    }
    if (mState.editorSubsystem->pending().requestSaveProjectDefaults) {
      if (!saveProjectDefaults(mState)) {
        LOG_ERROR("Runtime", "Failed to save project_defaults.json");
      } else {
        LOG_INFO("Runtime", "Saved project defaults to project_defaults.json");
      }
      mState.editorSubsystem->pending().requestSaveProjectDefaults = false;
    }
    if (mState.editorSubsystem->pending().requestResetProjectDefaults) {
      std::error_code ec;
      std::filesystem::remove(kProjectDefaultsPath, ec);
      if (ec) {
        LOG_ERROR("Runtime",
                  "Failed to remove project_defaults.json: " + ec.message());
      } else {
        LOG_INFO("Runtime", "Reset project defaults");
      }
      mState.editorSubsystem->pending().requestResetProjectDefaults = false;
    }
    if (!mState.editorSubsystem->pending().pendingSceneSavePath.empty()) {
      json root = json::parse(mState.scene.serializeToString(), nullptr, false);
      if (root.is_discarded())
        root = json::object();
      if (!root.contains("entities"))
        root["entities"] = json::array();
      root["terrainSettings"] = serializeTerrainSettings(mState.terrainSettings);
      root["terrainMaterial"] = serializeTerrainMaterial(mState.terrainMaterial);
      root["renderSettings"] = serializeRenderSettings(mState.render);
      root["audioSettings"] = serializeAudioSettings(mState.audioSubsystem->settings());
      root["playPerfHud"] = serializePlayPerfHudSettings(mState.playPerfHud);
      root["sunSettings"] = serializeSunSettings(mState.sun);
      root["skySettings"] = serializeSkySettings(mState.skyUI);
      root["postProcess"] = serializePostProcess(mState.postProcessor);

      std::ofstream out(mState.editorSubsystem->pending().pendingSceneSavePath);
      if (!out.is_open()) {
        LOG_ERROR(
            "Runtime",
            "Failed to save scene: " + mState.editorSubsystem->pending().pendingSceneSavePath);
      } else {
        out << root.dump(2);
      }
      mState.editorSubsystem->pending().pendingSceneSavePath.clear();
    }
    if (!mState.editorSubsystem->pending().pendingSceneLoadPath.empty()) {
      std::ifstream in(mState.editorSubsystem->pending().pendingSceneLoadPath);
      json root = json::parse(in, nullptr, false);
      if (root.is_discarded()) {
        LOG_ERROR(
            "Runtime",
            "Failed to load scene: " + mState.editorSubsystem->pending().pendingSceneLoadPath);
        mState.editorSubsystem->pending().pendingSceneLoadPath.clear();
        return;
      }

      mState.terrainSystem.shutdown();

      if (!mState.scene.loadFromString(root.dump()))
        LOG_ERROR(
            "Runtime",
            "Failed to load scene: " + mState.editorSubsystem->pending().pendingSceneLoadPath);
      else {
        if (root.contains("terrainSettings"))
          applyTerrainSettings(root["terrainSettings"], mState.terrainSettings);
        if (root.contains("terrainMaterial"))
          applyTerrainMaterial(root["terrainMaterial"], mState.terrainMaterial);
        if (root.contains("renderSettings"))
          applyRenderSettings(root["renderSettings"], mState.render);
        if (root.contains("audioSettings"))
          applyAudioSettings(root["audioSettings"], mState.audioSubsystem->settings());
        if (root.contains("playPerfHud"))
          applyPlayPerfHudSettings(root["playPerfHud"], mState.playPerfHud);
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

        mState.editorSubsystem->history().pendingHistoryCommit = true;
        mState.editorSubsystem->history().pendingHistoryLabel = "Load Scene";
        sceneMutatedByCommands = true;
        mState.gameplay.playerId = 0;
        for (auto e : mState.scene.registry().view<CameraComponent>()) {
          if (!mState.scene.registry().has<LifecycleComponent>(e) ||
              mState.scene.registry().get<LifecycleComponent>(e).state ==
                  EntityLifecycleState::Alive) {
            mState.gameplay.playerId = e;
            break;
          }
        }
      }
      mState.editorSubsystem->pending().pendingSceneLoadPath.clear();
    }

    if (mState.editorSubsystem->history().requestHistoryJump >= 0) {
      applyHistorySnapshot(mState.editorSubsystem->history().requestHistoryJump);
      mState.editorSubsystem->history().requestHistoryJump = -1;
      sceneMutatedByCommands = true;
    } else if (mState.editorSubsystem->history().requestUndo) {
      applyHistorySnapshot(mState.editorSubsystem->history().historyCursor - 1);
      sceneMutatedByCommands = true;
    } else if (mState.editorSubsystem->history().requestRedo) {
      applyHistorySnapshot(mState.editorSubsystem->history().historyCursor + 1);
      sceneMutatedByCommands = true;
    }
    mState.editorSubsystem->history().requestUndo = false;
    mState.editorSubsystem->history().requestRedo = false;

    if (mState.autoProcessImportQueue)
      mState.assets.processImportQueue();
    if (mState.hotReloadEnabled) {
      mState.hotReloadMessages = mState.assets.pollHotReload();
    } else {
      mState.hotReloadMessages.clear();
    }

    for (const std::string &emptyName :
         mState.editorSubsystem->pending().pendingEmptyEntityNames) {
      (void)mState.scene.createEmptyEntity(emptyName.empty() ? "Empty"
                                                             : emptyName);
      mState.editorSubsystem->history().pendingHistoryCommit = true;
      mState.editorSubsystem->history().pendingHistoryLabel = "Create Entity";
      sceneMutatedByCommands = true;
    }
    mState.editorSubsystem->pending().pendingEmptyEntityNames.clear();

    for (uint32_t entityId : mState.editorSubsystem->pending().pendingDeleteEntityIds) {
      if (entityId != 0) {
        mState.scene.deleteEntity(entityId);
        mState.editorSubsystem->history().pendingHistoryCommit = true;
        mState.editorSubsystem->history().pendingHistoryLabel = "Delete Entity";
        sceneMutatedByCommands = true;
      }
    }
    mState.editorSubsystem->pending().pendingDeleteEntityIds.clear();

    for (const std::string &path : mState.editorSubsystem->pending().pendingSpawnPaths) {
      uint32_t spawnedId = 0;

      // Intercept procedural primitives (__primitive_cube, etc.)
      const std::string prefix = "__primitive_";
      if (path == "__spaceship") {
        spawnedId = mState.scene.spawnPrimitive("cone");
        if (spawnedId != 0) {
          auto &reg = mState.scene.registry();
          auto &tr = reg.get<TransformComponent>(spawnedId);
          tr.position = mState.editorCamera.getPosition() +
                        mState.editorCamera.getForwardVector() * 12.0f;
          tr.rotation = glm::vec3(90.0f, mState.editorCamera.yaw, 0.0f);
          tr.scale = glm::vec3(1.8f, 4.8f, 1.8f);

          if (reg.has<NameComponent>(spawnedId))
            reg.get<NameComponent>(spawnedId).name = "Spaceship";
          else
            reg.emplace<NameComponent>(spawnedId, "Spaceship");

          auto &ship = reg.emplace<SpaceshipComponent>(spawnedId);
          ship.centerOfMass = glm::vec3(0.0f, -0.35f, 0.0f);
          ship.maxSpeed = 190.0f;
          ship.turnRateDeg = 105.0f;
          ship.bankAngleDeg = 32.0f;
          ship.idleDrag = 0.42f;
          ship.brakeDrag = 1.55f;

          auto &col = reg.emplace<ColliderComponent>(spawnedId);
          col.shape = ColliderComponent::Shape::Capsule;
          col.dimensions = glm::vec3(0.9f, 5.6f, 0.9f);

          auto &rb = reg.emplace<RigidbodyComponent>(spawnedId);
          rb.type = RigidbodyComponent::Type::Kinematic;
          rb.mass = ship.dryMassKg + ship.fuelMassKg;
          rb.friction = 0.6f;
          rb.restitution = 0.05f;

          auto &mat = reg.emplace<MaterialOverrideComponent>(spawnedId);
          mat.material.baseColor = glm::vec4(0.55f, 0.58f, 0.62f, 1.0f);
          mat.material.roughness = 0.34f;
          mat.material.metallic = 0.82f;
          mat.material.ao = 1.0f;
          mat.material.id = "__spaceship_default";
        }
      } else if (path.substr(0, prefix.size()) == prefix) {
        std::string shape = path.substr(prefix.size());
        spawnedId = mState.scene.spawnPrimitive(shape);
      } else {
        spawnedId = mState.scene.spawnFromFile(path);
      }

      if (spawnedId == 0) {
        LOG_ERROR("Runtime", "Failed to spawn: " + path);
      } else {
        mState.editorSubsystem->history().pendingHistoryCommit = true;
        mState.editorSubsystem->history().pendingHistoryLabel = "Spawn Asset";
        sceneMutatedByCommands = true;
      }
    }
    mState.editorSubsystem->pending().pendingSpawnPaths.clear();

    if (!mState.editorSubsystem->pending().pendingDropPaths.empty()) {
      for (const std::string &path : mState.editorSubsystem->pending().pendingDropPaths) {
        const uint32_t spawnedId = mState.scene.spawnFromFile(path);
        if (spawnedId == 0)
          LOG_ERROR("Runtime", "Failed to load dropped model: " + path);
        else {
          mState.editorSubsystem->history().pendingHistoryCommit = true;
          mState.editorSubsystem->history().pendingHistoryLabel = "Spawn Asset";
          sceneMutatedByCommands = true;
        }
      }
      mState.editorSubsystem->pending().pendingDropPaths.clear();
    }

    {
      ScopedCpuTimer timer(mState.profiler, "Destruction Release");
      DestructionSystem::preparePendingDestroy(mState.scene, mState.assets,
                                              &mState.physicsSystem);
    }

    const size_t entityCountBeforeFlush =
        mState.scene.registry().view<TransformComponent>().size();
    mState.scene.flushPendingDestroy();
    const size_t entityCountAfterFlush =
        mState.scene.registry().view<TransformComponent>().size();
    if (entityCountAfterFlush != entityCountBeforeFlush) {
      mState.editorSubsystem->history().pendingHistoryCommit = true;
      mState.editorSubsystem->history().pendingHistoryLabel = "Destroy Entity";
      sceneMutatedByCommands = true;
    }

    // Keep raw mesh pointers synchronized with authoritative asset handles.
    mState.scene.refreshMeshReferences();
  }

  if (mState.render.wireframe)
    GLStateCache::instance().setPolygonMode(GL_LINE);
  else
    GLStateCache::instance().setPolygonMode(GL_FILL);

  // Run rewritten gameplay systems only when Playing. Player control,
  // gameplay interactions, physics and scripts now have one clear order.
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

    {
      ScopedCpuTimer timer(mState.profiler, "Player Controller");
      mState.playerController.update(mState, dt);
    }
    {
      ScopedCpuTimer timer(mState.profiler, "Spaceship Controls");
      mState.spaceshipControl.update(mState, dt);
    }
    {
      ScopedCpuTimer timer(mState.profiler, "Scripts");
      mState.scriptSystem.update(mState.scene.registry(), dt);
    }
    {
      ScopedCpuTimer timer(mState.profiler, "Destruction Runtime");
      DestructionSystem::updateRuntime(
          mState.scene, dt, &mState.physicsSystem,
          mState.terrainSystem.isEnabled() ? &mState.terrainSystem : nullptr);
    }
    {
      ScopedCpuTimer timer(mState.profiler, "Physics");
      mState.physicsSystem.update(mState.scene.registry(), dt);
    }
    {
      ScopedCpuTimer timer(mState.profiler, "Player Interactions");
      mState.playerInteraction.update(mState, dt);
    }

    if (mState.terrainSystem.isEnabled()) {
      ScopedCpuTimer timer(mState.profiler, "Terrain Clamp");
      auto &reg = mState.scene.registry();
      for (auto e : reg.viewAll<TransformComponent, ScriptComponent>()) {
        if (e != mState.gameplay.playerId)
          clampEntityToTerrain(mState, reg, e);
      }
    }
  }

  // Legacy play-mode interaction block kept unreachable during the rewrite
  // rollout. The new systems above are now authoritative.
  if (false && mState.playState == AppState::PlayState::Playing) {
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
      if (mState.gameplay.playerId == 0 || !reg.has<CameraComponent>(mState.gameplay.playerId)) {
        mState.gameplay.playerId = 0;
        for (auto e : reg.view<CameraComponent>()) {
          if (!reg.has<LifecycleComponent>(e) ||
              reg.get<LifecycleComponent>(e).state ==
                  EntityLifecycleState::Alive) {
            mState.gameplay.playerId = e;
            break;
          }
        }
      }
      const float dx = (float)Mouse::getDX();
      const float dy = (float)Mouse::getDY();
      mState.gameplay.debug.debugMouseDX = dx;
      mState.gameplay.debug.debugMouseDY = dy;
      const bool primaryDown = Mouse::button(GLFW_MOUSE_BUTTON_LEFT);
      const bool primaryPressed = Mouse::buttonWentDown(GLFW_MOUSE_BUTTON_LEFT);
      const bool grabModifier =
          glfwGetKey(mState.window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
          glfwGetKey(mState.window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;

      // Mouse1 is now the laser. Keep physics grab available with Shift+Mouse1.
      if (primaryPressed &&
          grabModifier &&
          mState.gameplay.grab.grabbedEntityId == 0 &&
          mState.gameplay.playerId != 0 &&
          reg.has<TransformComponent>(mState.gameplay.playerId)) {
        const auto &camTr = reg.get<TransformComponent>(mState.gameplay.playerId);
        const glm::vec3 camPos = camTr.position;
        const glm::vec3 camFront = playCameraForward(camTr.rotation);
        PhysicsRaycastResult hit = mState.physicsSystem.raycast(
            camPos, camFront, 20.0f, mState.gameplay.playerId);
        mState.gameplay.debug.debugGrabHitId = hit.entityId;
        mState.gameplay.debug.debugGrabHitDist = hit.distance;
        mState.gameplay.debug.debugGrabHitName.clear();
        if (hit.entityId != 0 && reg.has<NameComponent>(hit.entityId)) {
          mState.gameplay.debug.debugGrabHitName =
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
            if (hit.entityId == mState.gameplay.playerId)
              canGrab = false;
            if (canGrab) {
              mState.gameplay.grab.grabbedEntityId = hit.entityId;
              mState.gameplay.grab.grabbedDistance = std::max(1.0f, hit.distance);
              mState.gameplay.grab.grabbedHadRigidbody = false;
              mState.gameplay.grab.grabbedPrevBodyType = 0;
              mState.gameplay.grab.grabbedIsTreeInstance = false;
              mState.gameplay.grab.grabbedPrefab.clear();
              mState.gameplay.grab.grabbedInstanceIndex = 0;
              mState.gameplay.grab.grabbedBaseMatrix = glm::mat4(1.0f);
              if (reg.has<RigidbodyComponent>(hit.entityId)) {
                auto &rb = reg.get<RigidbodyComponent>(hit.entityId);
                mState.gameplay.grab.grabbedHadRigidbody = true;
                mState.gameplay.grab.grabbedPrevBodyType = (int)rb.type;
                if (rb.type == RigidbodyComponent::Type::Static)
                  rb.type = RigidbodyComponent::Type::Kinematic;
              }
              if (reg.has<TreeComponent>(hit.entityId)) {
                auto &tree = reg.get<TreeComponent>(hit.entityId);
                mState.gameplay.grab.grabbedIsTreeInstance = true;
                mState.gameplay.grab.grabbedPrefab = tree.prefabName;
                mState.gameplay.grab.grabbedInstanceIndex = tree.instanceIndex;
                glm::mat4 instM;
                if (mState.terrainSystem.getPrefabInstanceMatrix(
                        tree.prefabName, tree.instanceIndex, instM)) {
                  mState.gameplay.grab.grabbedBaseMatrix = instM;
                  glm::vec3 basePos = glm::vec3(instM[3]);
                  mState.gameplay.grab.grabbedOffset = basePos - camPos;
                } else {
                  mState.gameplay.grab.grabbedOffset = camPos + camFront * mState.gameplay.grab.grabbedDistance - camPos;
                }
              } else {
                mState.gameplay.grab.grabbedOffset = camPos + camFront * mState.gameplay.grab.grabbedDistance - camPos;
              }
            }
          }
        }
      }

      const bool dragging =
          (mState.gameplay.grab.grabbedEntityId != 0 &&
           primaryDown);

      // Normal mouselook always active.
      if (mState.gameplay.playerId != 0 && reg.has<TransformComponent>(mState.gameplay.playerId)) {
        auto &tr = reg.get<TransformComponent>(mState.gameplay.playerId);
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
        mState.gameplay.debug.debugYaw = tr.rotation.y;
        mState.gameplay.debug.debugPitch = tr.rotation.x;
      }

      const bool wantsLaser =
          primaryPressed && !grabModifier &&
          mState.gameplay.activeSlot == GameplayState::HotbarSlot::Axe &&
          mState.gameplay.viewmodel.axeEnabled && mState.gameplay.grab.grabbedEntityId == 0 &&
          mState.gameplay.playerId != 0 &&
          reg.has<TransformComponent>(mState.gameplay.playerId);
      if (wantsLaser) {
        const auto &camTr = reg.get<TransformComponent>(mState.gameplay.playerId);
        const glm::vec3 front = playCameraForward(camTr.rotation);
        const float maxDistance = 240.0f;
        const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
        glm::vec3 right = glm::cross(front, worldUp);
        if (glm::length(right) < 0.0001f)
          right = glm::vec3(1.0f, 0.0f, 0.0f);
        right = glm::normalize(right);
        const glm::vec3 up = glm::normalize(glm::cross(right, front));
        const glm::vec3 start =
            camTr.position + right * mState.gameplay.viewmodel.axeOffset.x +
            up * mState.gameplay.viewmodel.axeOffset.y +
            front * (std::abs(mState.gameplay.viewmodel.axeOffset.z) + 0.10f);
        PhysicsRaycastResult hit =
            mState.physicsSystem.raycast(camTr.position, front, maxDistance,
                                         mState.gameplay.playerId);
        const glm::vec3 end =
            hit.hit ? hit.position : camTr.position + front * maxDistance;
        mState.projectiles.setLaserBeam(start, end, hit.hit, hit.entityId,
                                        hit.normal, 0.24f);
        mState.gameplay.debug.debugGrabHitId = hit.entityId;
        mState.gameplay.debug.debugGrabHitDist = hit.hit ? hit.distance : maxDistance;
        mState.gameplay.debug.debugGrabHitName.clear();
        if (hit.entityId != 0 && reg.has<NameComponent>(hit.entityId)) {
          mState.gameplay.debug.debugGrabHitName = reg.get<NameComponent>(hit.entityId).name;
        }
        if (hit.hit) {
          constexpr float kLaserDamage = 100.0f;
          damageDestructibleHit(mState, reg, hit, front, kLaserDamage);
        }
      }

      if (dragging) {
        // Drag object in camera screen plane using mouse deltas.
        if (reg.has<TransformComponent>(mState.gameplay.grab.grabbedEntityId) &&
            mState.gameplay.playerId != 0 &&
            reg.has<TransformComponent>(mState.gameplay.playerId)) {
          auto &grabTr = reg.get<TransformComponent>(mState.gameplay.grab.grabbedEntityId);
          auto &camTr = reg.get<TransformComponent>(mState.gameplay.playerId);
          const glm::vec3 front = playCameraForward(camTr.rotation);
          glm::vec3 right =
              glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
          glm::vec3 up = glm::normalize(glm::cross(right, front));
          const float dragScale = 0.01f * mState.gameplay.grab.grabbedDistance;
          mState.gameplay.grab.grabbedOffset += right * (-dx) * dragScale;
          mState.gameplay.grab.grabbedOffset += up * (dy) * dragScale;
          const glm::vec3 targetPos = camTr.position + mState.gameplay.grab.grabbedOffset;
          const glm::vec3 oldPos = grabTr.position;
          const glm::vec3 delta = targetPos - oldPos;
          const float invDt = (dt > 0.0001f) ? (1.0f / dt) : 0.0f;
          mState.gameplay.grab.grabbedReleaseVelocity = delta * invDt;

          // Apply a simple physics pull if the object has a rigidbody.
          if (reg.has<RigidbodyComponent>(mState.gameplay.grab.grabbedEntityId)) {
            auto &rb = reg.get<RigidbodyComponent>(mState.gameplay.grab.grabbedEntityId);
            const glm::vec3 vel = delta * invDt * 0.85f;
            rb.pendingLinearVelocity = vel;
            rb.setLinearVelocity = true;
          }

          grabTr.position = targetPos;

          // If this is a terrain tree instance, update instanced matrix too.
          mState.gameplay.debug.debugGrabPrefab.clear();
          mState.gameplay.debug.debugGrabInstance = -1;
          mState.gameplay.debug.debugGrabMoved = false;
          if (reg.has<TreeComponent>(mState.gameplay.grab.grabbedEntityId)) {
            auto &tree = reg.get<TreeComponent>(mState.gameplay.grab.grabbedEntityId);
            mState.gameplay.debug.debugGrabPrefab = tree.prefabName;
            mState.gameplay.debug.debugGrabInstance = (int)tree.instanceIndex;
            glm::mat4 instM = mState.gameplay.grab.grabbedBaseMatrix;
            instM[3] = glm::vec4(targetPos, 1.0f);
            mState.gameplay.debug.debugGrabMoved =
                mState.terrainSystem.setPrefabInstanceMatrix(
                    tree.prefabName, tree.instanceIndex, instM);
          }
        } else {
          mState.gameplay.grab.grabbedEntityId = 0;
        }
      }

      if (Mouse::buttonWentUp(GLFW_MOUSE_BUTTON_LEFT)) {
        // On release: convert tree instance into a physics-enabled entity.
        if (mState.gameplay.grab.grabbedEntityId != 0 &&
            reg.has<TreeComponent>(mState.gameplay.grab.grabbedEntityId)) {
          if (mState.terrainSystem.convertTreeToEntity(
                  mState.gameplay.grab.grabbedEntityId)) {
            if (reg.has<RigidbodyComponent>(mState.gameplay.grab.grabbedEntityId)) {
              auto &rb = reg.get<RigidbodyComponent>(mState.gameplay.grab.grabbedEntityId);
              rb.type = RigidbodyComponent::Type::Dynamic;
              rb.lockRotation = false;
              rb.pendingLinearVelocity = mState.gameplay.grab.grabbedReleaseVelocity;
              rb.setLinearVelocity = true;
            } else {
              auto &rb =
                  reg.emplace<RigidbodyComponent>(mState.gameplay.grab.grabbedEntityId);
              rb.type = RigidbodyComponent::Type::Dynamic;
              rb.lockRotation = false;
              rb.pendingLinearVelocity = mState.gameplay.grab.grabbedReleaseVelocity;
              rb.setLinearVelocity = true;
            }
          }
        }
        if (mState.gameplay.grab.grabbedEntityId != 0 && mState.gameplay.grab.grabbedHadRigidbody &&
            reg.has<RigidbodyComponent>(mState.gameplay.grab.grabbedEntityId)) {
          auto &rb = reg.get<RigidbodyComponent>(mState.gameplay.grab.grabbedEntityId);
          rb.type = (RigidbodyComponent::Type)mState.gameplay.grab.grabbedPrevBodyType;
        }
        mState.gameplay.grab.grabbedEntityId = 0;
        mState.gameplay.grab.grabbedHadRigidbody = false;
        mState.gameplay.grab.grabbedPrevBodyType = 0;
        mState.gameplay.grab.grabbedIsTreeInstance = false;
        mState.gameplay.grab.grabbedPrefab.clear();
        mState.gameplay.grab.grabbedInstanceIndex = 0;
        mState.gameplay.grab.grabbedBaseMatrix = glm::mat4(1.0f);
        mState.gameplay.grab.grabbedReleaseVelocity = glm::vec3(0.0f);
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
        if (mState.gameplay.playerId != 0 && reg.has<TransformComponent>(mState.gameplay.playerId)) {
          auto &camTr = reg.get<TransformComponent>(mState.gameplay.playerId);
          glm::vec3 front;
          front.x = -sin(glm::radians(camTr.rotation.y)) *
                    cos(glm::radians(camTr.rotation.x));
          front.y = sin(glm::radians(camTr.rotation.x));
          front.z = -cos(glm::radians(camTr.rotation.y)) *
                    cos(glm::radians(camTr.rotation.x));
          front = glm::normalize(front);

          auto hit = mState.physicsSystem.raycast(camTr.position, front, 100.0f,
                                                  mState.gameplay.playerId);
          if (hit.hit) {
            constexpr float kMouse2Damage = 100.0f;
            const bool consumedByDestruction =
                damageDestructibleHit(mState, reg, hit, front, kMouse2Damage);
            if (consumedByDestruction)
              craterCooldown = 0.18f;

            if (!consumedByDestruction) {
              // Apply a negative height offset to carve out a crater.
              // Radius 2.5 meters, scooping out 0.8 meters per tick.
              bool carved = mState.terrainSystem.applyHeightBrush(
                  hit.position, 2.5f, -0.8f);
              if (carved) {
                craterCooldown = 0.1f; // Limit to ~10 carves per second
              }
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
  if (mState.playState != AppState::PlayState::Playing) {
    mState.projectiles.clearLaserBeam();
    ScopedCpuTimer timer(mState.profiler, "Editor Destruction Preview");
    DestructionSystem::updateEditorPreview(
        mState.scene, dt,
        mState.terrainSystem.isEnabled() ? &mState.terrainSystem : nullptr);
  }

  {
    ScopedCpuTimer timer(mState.profiler, "Editor Camera");
    bool imguiWants = uiOut.wantCaptureMouse || ImGuizmo::IsUsing();
    mState.editorCamera.update(mState.window, imguiWants, dt);
  }

  // F key: focus on selected entity
  if (Keyboard::key(GLFW_KEY_F) && !uiOut.wantCaptureKeyboard &&
      mState.editorSubsystem->selection().selectedEntityId != 0) {
    auto &reg = mState.scene.registry();
    if (reg.has<TransformComponent>(mState.editorSubsystem->selection().selectedEntityId)) {
      glm::vec3 target =
          reg.get<TransformComponent>(mState.editorSubsystem->selection().selectedEntityId)
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
       mState.gameplay.viewmodel.usePlayerCameraInEdit);

  if (mState.playState == AppState::PlayState::Playing || forcePlayerCam) {
    auto &reg = mState.scene.registry();
    if (mState.gameplay.playerId == 0 || !reg.has<CameraComponent>(mState.gameplay.playerId)) {
      mState.gameplay.playerId =
          0; // Reset — stays 0 if no CameraComponent entity exists
      for (auto e : reg.view<CameraComponent>()) {
        if (!reg.has<LifecycleComponent>(e) ||
            reg.get<LifecycleComponent>(e).state ==
                EntityLifecycleState::Alive) {
          mState.gameplay.playerId = e;
          break;
        }
      }
    }

    if (mState.gameplay.playerId != 0 && reg.has<TransformComponent>(mState.gameplay.playerId)) {
      auto &tr = reg.get<TransformComponent>(mState.gameplay.playerId);
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
        clampEntityToTerrain(mState, reg, mState.gameplay.playerId);

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
      mState.gameplay.debug.debugCamFront = cameraFront;
      mState.gameplay.debug.debugCamUp = cameraUp;
      view = glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);
    }
  }

  // Viewmodel (axe)
  const bool showAxe =
      mState.gameplay.viewmodel.axeEnabled &&
      mState.gameplay.activeSlot == GameplayState::HotbarSlot::Axe &&
      (mState.playState == AppState::PlayState::Playing || mState.uiMode);
  if (showAxe) {
    auto &reg = mState.scene.registry();
    if (mState.gameplay.axeEntity == 0 ||
        !reg.has<MeshComponent>(mState.gameplay.axeEntity) ||
        !reg.has<TransformComponent>(mState.gameplay.axeEntity)) {
      auto h = mState.assets.loadOBJ(mState.gameplay.viewmodel.axePath);
      OBJModel *mdl = mState.assets.getOBJ(h);
      if (mdl) {
        EntityId eid = mState.scene.createEmptyEntity("Axe");
        reg.emplace<TransientComponent>(eid);
        auto &t = reg.get<TransformComponent>(eid);
        t.position = mState.gameplay.viewmodel.axeOffset;
        t.rotation = mState.gameplay.viewmodel.axeRotation;
        t.scale = mState.gameplay.viewmodel.axeScale;
        auto &mc = reg.emplace<MeshComponent>(eid, mdl, true, false);
        mc.objHandle = h;
        mc.assetId = mState.gameplay.viewmodel.axePath;
        mc.isViewModel = true;
        mState.gameplay.axeEntity = eid;
      }
    }

    if (mState.gameplay.axeEntity != 0 && reg.has<TransformComponent>(mState.gameplay.axeEntity)) {
      if (reg.has<MeshComponent>(mState.gameplay.axeEntity)) {
        reg.get<MeshComponent>(mState.gameplay.axeEntity).visible = true;
      }
      auto &t = reg.get<TransformComponent>(mState.gameplay.axeEntity);
      // Fixed viewmodel in screen space (no camera correlation).
      // View space looks down -Z, so treat positive Z as "forward".
      t.position = glm::vec3(mState.gameplay.viewmodel.axeOffset.x, mState.gameplay.viewmodel.axeOffset.y,
                             -std::abs(mState.gameplay.viewmodel.axeOffset.z));
      t.rotation = mState.gameplay.viewmodel.axeRotation;
      t.scale = mState.gameplay.viewmodel.axeScale;
    }
  } else if (mState.gameplay.axeEntity != 0) {
    auto &reg = mState.scene.registry();
    if (reg.has<MeshComponent>(mState.gameplay.axeEntity)) {
      reg.get<MeshComponent>(mState.gameplay.axeEntity).visible = false;
    }
  }

  const bool showTorch =
      mState.gameplay.viewmodel.torchEnabled &&
      mState.gameplay.activeSlot == GameplayState::HotbarSlot::Torch &&
      (mState.playState == AppState::PlayState::Playing || mState.uiMode);
  if (showTorch) {
    auto &reg = mState.scene.registry();
    if (mState.gameplay.torchEntity == 0 ||
        !reg.has<MeshComponent>(mState.gameplay.torchEntity) ||
        !reg.has<TransformComponent>(mState.gameplay.torchEntity)) {
      EntityId eid = mState.scene.spawnPrimitive("cube");
      if (eid != 0) {
        reg.emplace<TransientComponent>(eid);
        if (reg.has<NameComponent>(eid))
          reg.get<NameComponent>(eid).name = "Torch";
        auto &t = reg.get<TransformComponent>(eid);
        t.position = mState.gameplay.viewmodel.torchOffset;
        t.rotation = mState.gameplay.viewmodel.torchRotation;
        t.scale = mState.gameplay.viewmodel.torchScale;
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
        mState.gameplay.torchEntity = eid;
      }
    }

    if (mState.gameplay.torchEntity != 0 &&
        reg.has<TransformComponent>(mState.gameplay.torchEntity)) {
      if (reg.has<MeshComponent>(mState.gameplay.torchEntity)) {
        reg.get<MeshComponent>(mState.gameplay.torchEntity).visible = true;
      }
      auto &t = reg.get<TransformComponent>(mState.gameplay.torchEntity);
      t.position = glm::vec3(mState.gameplay.viewmodel.torchOffset.x, mState.gameplay.viewmodel.torchOffset.y,
                             -std::abs(mState.gameplay.viewmodel.torchOffset.z));
      t.rotation = mState.gameplay.viewmodel.torchRotation;
      t.scale = mState.gameplay.viewmodel.torchScale;
    }
  } else if (mState.gameplay.torchEntity != 0) {
    auto &reg = mState.scene.registry();
    if (reg.has<MeshComponent>(mState.gameplay.torchEntity)) {
      reg.get<MeshComponent>(mState.gameplay.torchEntity).visible = false;
    }
  }

  // Update Terrain chunk loading around the camera
  const size_t frameUploadBudget =
      (size_t)std::max(1024 * 1024, mState.terrainSettings.maxGpuUploadBytesPerFrame);
  mState.terrainSystem.setSubmissionBackend(
      mState.renderer.capabilities().preferredSubmissionBackend);
  mState.terrainSystem.beginFrameUploadBudget(frameUploadBudget);
  {
    ScopedCpuTimer timer(mState.profiler, "Terrain Update");
    mState.terrainSystem.update(cameraPos);
  }

  int winW, winH;
  glfwGetWindowSize(mState.window, &winW, &winH);
  glm::mat4 projection = glm::perspective(
      glm::radians(mState.input.fov), (float)winW / (float)winH, 0.1f, 500.0f);
  // Keep the world render stable in both editor and play modes.
  // The previous path jittered the entire camera only in play mode, which made
  // terrain, SSAO, shadows, and volumetrics look noticeably worse than the
  // editor view. Keep a stable projection, but preserve history while TAA is
  // enabled so the TAA controls actually affect the final image.
  if (!mState.postProcessor.enableTAA) {
    mState.postProcessor.resetTemporalHistory();
  }
  glm::mat4 renderProjection = projection;

  const bool brushActive =
      mState.terrainSystem.brushSettings().enabled && mState.terrainSystem.isEnabled() &&
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
      const float r = mState.terrainSystem.brushSettings().radius;
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
      if (mState.terrainSystem.brushSettings().mode == 0 || mState.terrainSystem.brushSettings().mode == 1) {
        float dir = (mState.terrainSystem.brushSettings().mode == 0) ? 1.0f : -1.0f;
        float delta = dir * mState.terrainSystem.brushSettings().strength * dt;
        mState.terrainSystem.applyHeightBrush(
            hit, mState.terrainSystem.brushSettings().radius, delta);
      } else {
        const char *prefab = "prefab_pine";
        if (mState.terrainSystem.brushSettings().target == 1)
          prefab = "prefab_rock";
        else if (mState.terrainSystem.brushSettings().target == 2)
          prefab = "prefab_grass";
        bool add = (mState.terrainSystem.brushSettings().mode == 2);
        mState.terrainSystem.applyVegetationBrush(
            hit, mState.terrainSystem.brushSettings().radius, prefab, add,
            mState.terrainSystem.brushSettings().scatterCount);
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
        auto &sel = mState.editorSubsystem->selection().selectedEntities;
        auto it = std::find(sel.begin(), sel.end(), hitId);
        if (it != sel.end()) {
          sel.erase(it);
          if (mState.editorSubsystem->selection().selectedEntityId == hitId)
            mState.editorSubsystem->selection().selectedEntityId = sel.empty() ? 0 : sel.back();
        } else {
          sel.push_back(hitId);
          mState.editorSubsystem->selection().selectedEntityId = hitId;
        }
      } else {
        mState.editorSubsystem->selection().selectedEntities.clear();
        mState.editorSubsystem->selection().selectedEntities.push_back(hitId);
        mState.editorSubsystem->selection().selectedEntityId = hitId;
      }
      mState.editorSubsystem->selection().lastClickedEntity = hitId;

      auto &reg = mState.scene.registry();
      if (reg.has<MeshComponent>(hitId) && reg.has<TransformComponent>(hitId)) {
        OBJModel *mdl = reg.get<MeshComponent>(hitId).objModel;
        if (mdl && mdl->submeshCount() > 1) {
          auto &tr = reg.get<TransformComponent>(hitId);
          std::string hitPart = MousePicking::pickSubmesh(ray, tr, *mdl);
          if (!hitPart.empty()) {
            mState.editorSubsystem->selection().editObjPart = true;
            mState.editorSubsystem->selection().selectedObjPartName = hitPart;
          } else {
            mState.editorSubsystem->selection().editObjPart = false;
            mState.editorSubsystem->selection().selectedObjPartName.clear();
          }
        } else {
          mState.editorSubsystem->selection().editObjPart = false;
          mState.editorSubsystem->selection().selectedObjPartName.clear();
        }
      } else {
        mState.editorSubsystem->selection().editObjPart = false;
        mState.editorSubsystem->selection().selectedObjPartName.clear();
      }
    } else {
      mState.editorSubsystem->selection().selectedEntities.clear();
      mState.editorSubsystem->selection().selectedEntityId = 0;
      mState.editorSubsystem->selection().editObjPart = false;
      mState.editorSubsystem->selection().selectedObjPartName.clear();
    }
  }

  // (grab handled earlier during play update)

  {
    if (mState.editor.drawGizmo(mState.uiMode, view, projection, mState.scene,
                                mState.sun, mState.events, selState,
                                cameraPos)) {
      mState.editorSubsystem->history().pendingHistoryCommit = true;
      mState.editorSubsystem->history().pendingHistoryLabel = "Edit Scene";
    }
  }

  if (mState.editorSubsystem->history().pendingHistoryCommit) {
    const bool interacting = ImGuizmo::IsUsing() || ImGui::IsAnyItemActive() ||
                             ImGui::IsMouseDown(0);
    if (!interacting || sceneMutatedByCommands) {
      commitHistorySnapshot(mState.editorSubsystem->history().pendingHistoryLabel.empty()
                                ? "Edit Scene"
                                : mState.editorSubsystem->history().pendingHistoryLabel);
      mState.editorSubsystem->history().pendingHistoryCommit = false;
      mState.editorSubsystem->history().pendingHistoryLabel.clear();
    }
  }

  {
    ScopedCpuTimer timer(mState.profiler, "Projectiles");
    mState.projectiles.update(dt);
  }

  if (mState.audioSubsystem) {
    if (mState.editorSubsystem->pending().requestTestFootstepAudio) {
      mState.audioSubsystem->playTestFootstep();
      mState.editorSubsystem->pending().requestTestFootstepAudio = false;
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
  mState.renderSystem.setSubmissionBackend(
      mState.renderer.capabilities().preferredSubmissionBackend);
  mState.renderSystem.setFrameUploadBudgetBytes(
      mState.terrainSystem.remainingFrameUploadBudgetBytes());
  mState.renderSystem.beginFrame();
  mState.renderSystem.prepareFrame(mState.scene.registry());

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
  mState.performance = buildFramePerformanceSnapshot(mState, dt);
  logPerformanceSnapshotIfNeeded(mState, dt);
}
