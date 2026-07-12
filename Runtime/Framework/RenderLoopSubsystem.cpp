#include "RenderLoopSubsystem.h"
#include "AppState.h"
#include "EditorSubsystem.h"
#include "PhysicsDebugRenderer.h"
#include "Texture.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <limits>
#include <unordered_set>
#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace {
constexpr int kMaxShadowCascades = 4;

void logTerrainMaterialWarningOnce(const std::string &key,
                                   const std::string &message) {
  static std::unordered_set<std::string> loggedWarnings;
  if (loggedWarnings.insert(key).second) {
    LOG_WARN("TerrainMaterial", message);
  }
}

void logSkyWarningOnce(const std::string &key, const std::string &message) {
  static std::unordered_set<std::string> loggedWarnings;
  if (loggedWarnings.insert(key).second) {
    LOG_WARN("Sky", message);
  }
}

float projectionFovY(const glm::mat4 &projection) {
  return 2.0f * std::atan(1.0f / std::max(std::abs(projection[1][1]), 0.0001f));
}

float projectionAspect(const glm::mat4 &projection) {
  return std::max(0.05f, projection[1][1] / std::max(projection[0][0], 0.0001f));
}

std::array<glm::vec3, 8> splitFrustumCorners(const glm::mat4 &view,
                                             float fovY, float aspect,
                                             float nearPlane,
                                             float farPlane) {
  const glm::mat4 splitProj =
      glm::perspective(fovY, aspect, std::max(0.001f, nearPlane),
                       std::max(nearPlane + 0.01f, farPlane));
  const glm::mat4 invVP = glm::inverse(splitProj * view);
  std::array<glm::vec3, 8> corners{};
  int idx = 0;
  for (int z = 0; z < 2; ++z) {
    const float ndcZ = (z == 0) ? -1.0f : 1.0f;
    for (int y = 0; y < 2; ++y) {
      for (int x = 0; x < 2; ++x) {
        glm::vec4 p = invVP * glm::vec4(x ? 1.0f : -1.0f,
                                        y ? 1.0f : -1.0f, ndcZ, 1.0f);
        corners[idx++] = glm::vec3(p) / std::max(p.w, 0.0001f);
      }
    }
  }
  return corners;
}

glm::mat4 fitLightMatrixToCorners(const std::array<glm::vec3, 8> &corners,
                                  const glm::vec3 &sunDirRaw,
                                  float cascadeFar, int shadowResolution) {
  glm::vec3 center(0.0f);
  for (const glm::vec3 &corner : corners)
    center += corner;
  center /= 8.0f;

  float radius = 0.0f;
  for (const glm::vec3 &corner : corners)
    radius = std::max(radius, glm::length(corner - center));
  radius = std::max(1.0f, std::ceil(radius * 16.0f) / 16.0f);

  glm::vec3 sunDir = glm::normalize(sunDirRaw);
  glm::vec3 up(0.0f, 1.0f, 0.0f);
  if (std::abs(glm::dot(up, sunDir)) > 0.95f)
    up = glm::vec3(1.0f, 0.0f, 0.0f);

  const float lightDistance = radius + cascadeFar + 80.0f;
  glm::mat4 shadowView =
      glm::lookAt(center - sunDir * lightDistance, center, up);

  glm::vec3 minL(FLT_MAX);
  glm::vec3 maxL(-FLT_MAX);
  for (const glm::vec3 &corner : corners) {
    glm::vec3 lightSpace = glm::vec3(shadowView * glm::vec4(corner, 1.0f));
    minL = glm::min(minL, lightSpace);
    maxL = glm::max(maxL, lightSpace);
  }

  glm::vec3 centerL = glm::vec3(shadowView * glm::vec4(center, 1.0f));
  const float texelSize =
      (radius * 2.0f) / std::max(1, shadowResolution);
  centerL.x = std::floor(centerL.x / texelSize) * texelSize;
  centerL.y = std::floor(centerL.y / texelSize) * texelSize;

  const float minX = centerL.x - radius;
  const float maxX = centerL.x + radius;
  const float minY = centerL.y - radius;
  const float maxY = centerL.y + radius;
  const float zPad = std::max(40.0f, cascadeFar * 0.35f);
  const float lightNear = std::max(0.1f, -maxL.z - zPad);
  const float lightFar = std::max(lightNear + 1.0f, -minL.z + zPad);

  return glm::ortho(minX, maxX, minY, maxY, lightNear, lightFar) *
         shadowView;
}

void logGlContextErrors(const char *stage, int cascadeIndex = -1) {
  GLenum err = glGetError();
  if (err == GL_NO_ERROR)
    return;

  std::string msg = std::string(stage ? stage : "GL stage") +
                    " generated GL error " + std::to_string((int)err);
  if (cascadeIndex >= 0)
    msg += " on cascade " + std::to_string(cascadeIndex);
  LOG_ERROR("Render", msg);

  while ((err = glGetError()) != GL_NO_ERROR) {
    std::string extra = std::string(stage ? stage : "GL stage") +
                        " generated additional GL error " +
                        std::to_string((int)err);
    if (cascadeIndex >= 0)
      extra += " on cascade " + std::to_string(cascadeIndex);
    LOG_ERROR("Render", extra);
  }
}

uint64_t hashCombine64(uint64_t seed, uint64_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
  return seed;
}

uint64_t quantizedFloatHash(float value, float scale) {
  const double scaled = std::round((double)value * (double)scale);
  return static_cast<uint64_t>(static_cast<int64_t>(scaled));
}

uint64_t buildShadowConfigHash(const RenderSettings &render,
                               const glm::mat4 &projection, float nearPlane,
                               float farPlane) {
  uint64_t h = 1469598103934665603ull;
  h = hashCombine64(h, (uint64_t)(render.enableCascadedShadows ? 1 : 0));
  h = hashCombine64(h, (uint64_t)std::clamp(render.shadowCascadeCount, 1, 4));
  h = hashCombine64(h, (uint64_t)std::max(render.shadowMapResolution, 256));
  h = hashCombine64(h, quantizedFloatHash(render.shadowCascadeDistance, 10.0f));
  h = hashCombine64(h, quantizedFloatHash(render.shadowCascadeLambda, 1000.0f));
  h = hashCombine64(h, quantizedFloatHash(render.shadowFarPlane, 10.0f));
  h = hashCombine64(h, quantizedFloatHash(nearPlane, 1000.0f));
  h = hashCombine64(h, quantizedFloatHash(farPlane, 100.0f));
  h = hashCombine64(h, quantizedFloatHash(projection[0][0], 10000.0f));
  h = hashCombine64(h, quantizedFloatHash(projection[1][1], 10000.0f));
  h = hashCombine64(h, (uint64_t)(render.shadowStaggeredUpdates ? 1 : 0));
  h = hashCombine64(h, (uint64_t)std::clamp(render.shadowCascadeCadence, 1, 4));
  h = hashCombine64(h,
                    quantizedFloatHash(render.shadowCascadeDistanceScale, 100.0f));
  h = hashCombine64(h,
                    quantizedFloatHash(render.shadowCascadeAngleScale, 100.0f));
  return h;
}

int cascadeCadence(int cascadeIndex, int cadenceBase) {
  int cadence = 1;
  const int safeBase = std::clamp(cadenceBase, 1, 4);
  for (int i = 0; i < cascadeIndex; ++i)
    cadence = std::min(cadence * safeBase, 64);
  return cadence;
}
} // namespace

bool RenderLoopSubsystem::initialize() { return true; }

void RenderLoopSubsystem::shutdown() {
  if (mShadowQueries[0] != 0 || mShadowQueries[1] != 0) {
    glDeleteQueries(2, mShadowQueries);
    mShadowQueries[0] = 0;
    mShadowQueries[1] = 0;
    mShadowQuerySubmitted[0] = false;
    mShadowQuerySubmitted[1] = false;
  }
  for (int slot = 0; slot < MainGpuTimerCount; ++slot) {
    if (mMainQueries[slot][0] != 0 || mMainQueries[slot][1] != 0) {
      glDeleteQueries(2, mMainQueries[slot]);
      mMainQueries[slot][0] = 0;
      mMainQueries[slot][1] = 0;
      mMainQuerySubmitted[slot][0] = false;
      mMainQuerySubmitted[slot][1] = false;
    }
  }
  mShadowValid = false;
  mLastShadowConfigHash = 0;
  std::fill(std::begin(mCascadeValid), std::end(mCascadeValid), false);
  std::fill(std::begin(mCascadeLastUpdateFrame),
            std::end(mCascadeLastUpdateFrame), 0ull);
  mMainQueryPrimed = false;
}

void RenderLoopSubsystem::executeRenderPasses(const glm::mat4 &view,
                                              const glm::mat4 &projection,
                                              const glm::vec3 &cameraPos,
                                              const glm::vec3 &cameraFront,
                                              const glm::vec3 &cameraUp,
                                              float renderTime) {
  ++mShadowFrameIndex;
  mState.shadowCascadesUpdated = 0;
  mState.shadowCascadeStaggered = false;
  // Update sun direction for day/night cycle (affects shadow pass).
  if (mState.skyUI.dayNightEnabled) {
    const float t = mState.skyUI.timeOfDay;
    const float az = glm::radians(mState.sun.sunAzimuth);
    const float phase = t * 6.28318530718f - 1.57079632679f;
    const float elevDeg = std::sin(phase) * 75.0f;
    mState.sun.sunElevation = elevDeg;
    const float el = glm::radians(elevDeg);
    mState.sun.sunDir = glm::normalize(
        glm::vec3(std::cos(el) * std::sin(az), std::sin(el),
                  std::cos(el) * std::cos(az)));
    mState.sun.sunDir = -mState.sun.sunDir;
  }
  if (mShadowQueries[0] == 0 && mShadowQueries[1] == 0) {
    glGenQueries(2, mShadowQueries);
  }
  for (int slot = 0; slot < MainGpuTimerCount; ++slot) {
    if (mMainQueries[slot][0] == 0 && mMainQueries[slot][1] == 0) {
      glGenQueries(2, mMainQueries[slot]);
    }
  }

  mState.renderGraph.clear();
  bool shouldRenderShadowPass = false;
  if (!mState.render.disableShadows) {
    const int interval = std::max(1, mState.render.shadowUpdateInterval);
    if (!mShadowValid || interval <= 1) {
      shouldRenderShadowPass = true;
    } else if ((mShadowFrameIndex % (uint64_t)interval) == 0) {
      shouldRenderShadowPass = true;
    }

    if (!shouldRenderShadowPass) {
      const float dist =
          glm::length(cameraPos - mLastShadowCamPos);
      const float distThresh = mState.render.shadowUpdateDistance;
      const float dotDir =
          glm::clamp(glm::dot(glm::normalize(mState.sun.sunDir),
                              glm::normalize(mLastShadowSunDir)),
                     -1.0f, 1.0f);
      const float angleDeg = glm::degrees(std::acos(dotDir));
      if (dist > distThresh || angleDeg > mState.render.shadowUpdateAngle) {
        shouldRenderShadowPass = true;
      }
    }
  }

  const int shadowWriteIndex = mShadowQueryIndex;
  bool shadowQuerySubmittedThisFrame = false;
  if (shouldRenderShadowPass) {
    mState.renderGraph.addPass({"ShadowPass", {}, [&]() {
                                  const int qIdx = shadowWriteIndex;
                                  if (mShadowQueries[qIdx] != 0)
                                    glBeginQuery(GL_TIME_ELAPSED,
                                                 mShadowQueries[qIdx]);
                                  shadowQuerySubmittedThisFrame =
                                      mShadowQueries[qIdx] != 0;
                                  renderShadowPass(
                                      view, projection, cameraPos,
                                      mState.sun.sunDir, 0.1f,
                                      mState.render.shadowFarPlane);
                                  if (shadowQuerySubmittedThisFrame)
                                    glEndQuery(GL_TIME_ELAPSED);
                                }});
  }
  mState.renderGraph.addPass(
      {"MainPass",
       shouldRenderShadowPass ? std::vector<std::string>{"ShadowPass"}
                              : std::vector<std::string>{},
       [&]() {
         renderMainPass(view, projection, cameraPos, cameraFront, cameraUp,
                        renderTime);
       }});
  (void)mState.renderGraph.execute();
  mState.lastRenderPassOrder = mState.renderGraph.lastExecutionOrder();

  const int shadowRead = (shadowWriteIndex + 1) % 2;
  if (mShadowQueries[shadowRead] != 0 &&
      mShadowQuerySubmitted[shadowRead]) {
    GLuint available = 0;
    glGetQueryObjectuiv(mShadowQueries[shadowRead],
                        GL_QUERY_RESULT_AVAILABLE, &available);
    if (available) {
      GLuint64 timeNs = 0;
      glGetQueryObjectui64v(mShadowQueries[shadowRead], GL_QUERY_RESULT,
                            &timeNs);
      mState.gpuShadowMs = static_cast<float>(timeNs / 1000000.0);
      mShadowQuerySubmitted[shadowRead] = false;
    }
  }
  if (shadowQuerySubmittedThisFrame) {
    mShadowQuerySubmitted[shadowWriteIndex] = true;
    mShadowQueryIndex = shadowRead;
  }

  const int mainRead = (mMainQueryIndex + 1) % 2;
  if (mMainQueryPrimed) {
    float subPassMs[MainGpuTimerCount] = {mState.gpuMainSkyMs,
                                          mState.gpuMainTerrainMs,
                                          mState.gpuMainSceneMs,
                                          mState.gpuMainPostMs};
    for (int slot = 0; slot < MainGpuTimerCount; ++slot) {
      if (mMainQueries[slot][mainRead] == 0 ||
          !mMainQuerySubmitted[slot][mainRead]) {
        continue;
      }

      GLuint available = 0;
      glGetQueryObjectuiv(mMainQueries[slot][mainRead],
                          GL_QUERY_RESULT_AVAILABLE, &available);
      if (!available)
        continue;

      GLuint64 timeNs = 0;
      glGetQueryObjectui64v(mMainQueries[slot][mainRead], GL_QUERY_RESULT,
                            &timeNs);
      subPassMs[slot] = static_cast<float>(timeNs / 1000000.0);
      mMainQuerySubmitted[slot][mainRead] = false;
    }

    mState.gpuMainSkyMs = subPassMs[MainGpuTimerSky];
    mState.gpuMainTerrainMs = subPassMs[MainGpuTimerTerrain];
    mState.gpuMainSceneMs = subPassMs[MainGpuTimerScene];
    mState.gpuMainPostMs = subPassMs[MainGpuTimerPost];
    mState.gpuMainMs = mState.gpuMainSkyMs + mState.gpuMainTerrainMs +
                       mState.gpuMainSceneMs + mState.gpuMainPostMs;
  }
  for (int slot = 0; slot < MainGpuTimerCount; ++slot)
    mMainQuerySubmitted[slot][mMainQueryIndex] =
        mMainQueries[slot][mMainQueryIndex] != 0;
  mMainQueryPrimed = true;
  mMainQueryIndex = mainRead;

  // OpenGL does not allow nested GL_TIME_ELAPSED queries. Keep shadow/main
  // timings separate, then expose an approximate frame GPU time from them.
  mState.gpuFrameMs =
      mState.gpuMainMs + (shouldRenderShadowPass ? mState.gpuShadowMs : 0.0f);
}

void RenderLoopSubsystem::renderShadowPass(const glm::mat4 &view,
                                           const glm::mat4 &projection,
                                           const glm::vec3 &cameraPos,
                                           const glm::vec3 &sunDirRaw,
                                           float nearPlane, float farPlane) {
  const bool wantsCsm =
      mState.render.enableCascadedShadows && mState.render.shadowCascadeCount > 1;
  int cascadeCount =
      wantsCsm ? std::clamp(mState.render.shadowCascadeCount, 2, kMaxShadowCascades)
               : 1;
  const int requestedRes =
      std::clamp(mState.render.shadowMapResolution, 512, 8192);
  if (!mState.renderer.ensureShadowResources(requestedRes, cascadeCount)) {
    cascadeCount = 1;
    (void)mState.renderer.ensureShadowResources(requestedRes, 1);
  }
  logGlContextErrors("ensureShadowResources");
  if (mState.renderer.shadowTex() == 0) {
    mState.render.shadowUsingCsm = false;
    mState.render.activeShadowCascadeCount = 1;
    mShadowValid = false;
    mLastShadowConfigHash = 0;
    std::fill(std::begin(mCascadeValid), std::end(mCascadeValid), false);
    return;
  }

  const bool useCsm =
      cascadeCount > 1 && mState.renderer.shadowArrayTex() != 0 &&
      mState.renderer.shadowLayers() >= cascadeCount;
  if (!useCsm)
    cascadeCount = 1;

  const float maxShadowDistance =
      useCsm ? std::max(nearPlane + 1.0f, mState.render.shadowCascadeDistance)
             : std::max(nearPlane + 1.0f, farPlane);
  const float lambda =
      std::clamp(mState.render.shadowCascadeLambda, 0.0f, 1.0f);
  const float fovY = projectionFovY(projection);
  const float aspect = projectionAspect(projection);
  const uint64_t shadowConfigHash =
      buildShadowConfigHash(mState.render, projection, nearPlane, farPlane);
  const bool forceAllCascades =
      !mShadowValid || shadowConfigHash != mLastShadowConfigHash;
  const bool staggeredUpdates =
      useCsm && cascadeCount > 1 && mState.render.shadowStaggeredUpdates;

  float splitNear = std::max(0.05f, nearPlane);
  int updatedCascadeCount = 0;
  for (int i = 0; i < cascadeCount; ++i) {
    const float p = float(i + 1) / float(cascadeCount);
    const float logSplit =
        splitNear * std::pow(maxShadowDistance / splitNear, p);
    const float uniformSplit =
        splitNear + (maxShadowDistance - splitNear) * p;
    const float splitFar = (i == cascadeCount - 1)
                               ? maxShadowDistance
                               : glm::mix(uniformSplit, logSplit, lambda);
    mState.render.shadowCascadeSplits[i] = splitFar;

    const auto corners =
        splitFrustumCorners(view, fovY, aspect, splitNear, splitFar);
    const glm::mat4 lightSpace =
        fitLightMatrixToCorners(corners, sunDirRaw, splitFar,
                                mState.renderer.shadowRes());

    bool updateCascade = true;
    if (i > 0 && staggeredUpdates && !forceAllCascades) {
      const int cadence =
          cascadeCadence(i, mState.render.shadowCascadeCadence);
      const uint64_t framesSinceUpdate =
          mShadowFrameIndex >= mCascadeLastUpdateFrame[i]
              ? (mShadowFrameIndex - mCascadeLastUpdateFrame[i])
              : cadence;
      const float distanceThreshold =
          mState.render.shadowUpdateDistance *
          std::pow(std::max(mState.render.shadowCascadeDistanceScale, 1.0f),
                   (float)i);
      const float angleThreshold =
          mState.render.shadowUpdateAngle *
          std::pow(std::max(mState.render.shadowCascadeAngleScale, 1.0f),
                   (float)i);
      const float dist = glm::length(cameraPos - mLastCascadeCamPos[i]);
      const float dotDir =
          glm::clamp(glm::dot(glm::normalize(sunDirRaw),
                              glm::normalize(mLastCascadeSunDir[i])),
                     -1.0f, 1.0f);
      const float angleDeg = glm::degrees(std::acos(dotDir));
      updateCascade = !mCascadeValid[i] || framesSinceUpdate >= (uint64_t)cadence ||
                      dist > distanceThreshold || angleDeg > angleThreshold;
    }

    if (!updateCascade) {
      splitNear = splitFar;
      continue;
    }

    mState.render.lightSpaceMatrices[i] = lightSpace;
    if (i == 0)
      mState.render.lightSpaceMatrix = lightSpace;

    const float shadowCullLimit =
        splitFar + std::max(25.0f, splitFar * 0.35f);
    mState.renderSystem.setViewProjection(lightSpace);
    mState.renderSystem.setShadowDistanceLimit(shadowCullLimit);

    mState.renderer.beginShadowPass(useCsm ? i : -1);
    logGlContextErrors("beginShadowPass", i);

    Shader &depthSh = mState.renderer.shadowShader();
    depthSh.activate();
    depthSh.setMat4("uLightSpaceMatrix", lightSpace);

    const bool gpuTerrain = mState.terrainSystem.gpuTerrainActive();
    if (gpuTerrain) {
      mState.terrainSystem.renderGpuTerrain(depthSh, lightSpace, cameraPos,
                                            true);
      logGlContextErrors("renderGpuTerrain shadow", i);
    }
    const bool keepObjTerrainFallback =
        gpuTerrain && mState.terrainSystem.stats().gpuTerrainFallback;
    mState.renderSystem.update(
        mState.scene.registry(), depthSh, true, 0, false, false,
        gpuTerrain && !keepObjTerrainFallback
            ? RenderSystem::TerrainFilter::ExcludeTerrain
            : RenderSystem::TerrainFilter::All);
    logGlContextErrors("renderSystem shadow update", i);

    mState.renderer.endShadowPass();
    logGlContextErrors("endShadowPass", i);

    ++updatedCascadeCount;
    mCascadeValid[i] = true;
    mCascadeLastUpdateFrame[i] = mShadowFrameIndex;
    mLastCascadeCamPos[i] = cameraPos;
    mLastCascadeSunDir[i] = glm::normalize(sunDirRaw);

    splitNear = splitFar;
  }
  mState.renderSystem.setShadowDistanceLimit(
      std::numeric_limits<float>::infinity());
  for (int i = cascadeCount; i < kMaxShadowCascades; ++i) {
    mState.render.shadowCascadeSplits[i] =
        mState.render.shadowCascadeSplits[cascadeCount - 1];
    mState.render.lightSpaceMatrices[i] =
        mState.render.lightSpaceMatrices[cascadeCount - 1];
  }
  mState.render.lightSpaceMatrix = mState.render.lightSpaceMatrices[0];

  mState.render.activeShadowCascadeCount = cascadeCount;
  mState.render.shadowUsingCsm = useCsm;
  mState.render.activeShadowMapResolution = mState.renderer.shadowRes();
  mState.shadowCascadesUpdated = updatedCascadeCount;
  mState.shadowCascadeStaggered =
      staggeredUpdates && updatedCascadeCount < cascadeCount;
  mShadowValid = true;
  mLastShadowConfigHash = shadowConfigHash;
  mLastShadowCamPos = cameraPos;
  mLastShadowSunDir = glm::normalize(sunDirRaw);
}

void RenderLoopSubsystem::renderMainPass(const glm::mat4 &view,
                                         const glm::mat4 &projection,
                                         const glm::vec3 &cameraPos,
                                         const glm::vec3 &cameraFront,
                                         const glm::vec3 &cameraUp,
                                         float nowT) {
  mState.renderSystem.setViewProjection(projection * view);
  mState.renderSystem.setShadowDistanceLimit(
      std::numeric_limits<float>::infinity());
  auto beginMainGpuTimer = [&](MainGpuTimerSlot slot) {
    const int qIdx = mMainQueryIndex;
    if (mMainQueries[slot][qIdx] == 0)
      return false;
    glBeginQuery(GL_TIME_ELAPSED, mMainQueries[slot][qIdx]);
    return true;
  };
  auto endMainGpuTimer = [&](bool active) {
    if (active)
      glEndQuery(GL_TIME_ELAPSED);
  };

  if (!mState.render.disableHDR && !mState.skyUI.skyHDRPath.empty()) {
    if (!mState.sky.reloadHDR(mState.skyUI.skyHDRPath)) {
      logSkyWarningOnce("sky-hdr-reload:" + mState.skyUI.skyHDRPath,
                        "Failed to reload HDR sky: " + mState.skyUI.skyHDRPath);
    }
  }

  glm::vec3 lightDir = mState.sun.sunDir;
  float far_plane = mState.render.shadowFarPlane;
  auto applyShadowUniforms = [&](Shader &shader) {
    const bool shadowsEnabled = !mState.render.disableShadows && mShadowValid;
    const bool useCsm = shadowsEnabled && mState.render.shadowUsingCsm;
    const int cascadeCount =
        std::clamp(mState.render.activeShadowCascadeCount, 1,
                   kMaxShadowCascades);
    shader.setBool("uShadowsEnabled", shadowsEnabled);
    shader.setBool("uUseCascadedShadows", useCsm);
    shader.setInt("uCascadeCount", cascadeCount);
    shader.setFloat("uShadowNormalBias", mState.render.shadowNormalBias);
    shader.setFloat("uShadowDepthBias", mState.render.shadowDepthBias);
    shader.setFloat("uShadowSoftness", mState.render.shadowSoftness);
    shader.setBool("uShowShadowCascades", mState.render.showShadowCascades);
    shader.setInt("uShadowMapArray", 16);
    for (int i = 0; i < kMaxShadowCascades; ++i) {
      shader.setMat4("uLightSpaceMatrices[" + std::to_string(i) + "]",
                     mState.render.lightSpaceMatrices[i]);
      shader.setFloat("uCascadeSplits[" + std::to_string(i) + "]",
                      mState.render.shadowCascadeSplits[i]);
    }
    glActiveTexture(GL_TEXTURE16);
    glBindTexture(GL_TEXTURE_2D_ARRAY,
                  useCsm ? mState.renderer.shadowArrayTex() : 0);
    glActiveTexture(GL_TEXTURE0);
  };

  glm::vec3 skyHorizon = glm::make_vec3(mState.skyUI.skyHorizon);
  glm::vec3 skyTop = glm::make_vec3(mState.skyUI.skyTop);
  glm::vec3 sunColor = mState.sun.sunColor;
  glm::vec3 visualSunColor = mState.skyUI.visualSunColor;
  float nightFactor = 0.0f;
  glm::vec3 fogColor = mState.render.fogColor;
  float fogDensity = mState.render.fogDensity;
  float emissiveBoost = 1.0f;
  float emissiveFlicker = 0.0f;
  float skyExposure = mState.render.exposure;
  float skyGamma = mState.render.gamma;
  float ambientRampStrength = mState.render.ambientRampStrength;
  glm::vec3 ambientRampTop = mState.render.ambientRampTop;
  glm::vec3 ambientRampBottom = mState.render.ambientRampBottom;
  glm::vec3 ambientSkyColor = mState.render.ambientSkyColor;
  glm::vec3 ambientHorizonColor = mState.render.ambientHorizonColor;
  glm::vec3 ambientGroundColor = mState.render.ambientGroundColor;

  if (mState.skyUI.dayNightEnabled) {
    const float t = mState.skyUI.timeOfDay;
    const float phase = t * 6.28318530718f - 1.57079632679f;
    const float sunHeight = std::sin(phase);
    const float dayFactor = glm::smoothstep(-0.05f, 0.20f, sunHeight);
    nightFactor = glm::smoothstep(0.15f, -0.20f, sunHeight);
    float duskFactor = 1.0f - std::abs(sunHeight);
    duskFactor = glm::smoothstep(0.20f, 0.80f, duskFactor);

    const glm::vec3 dayH = glm::make_vec3(mState.skyUI.dayHorizon);
    const glm::vec3 dayT = glm::make_vec3(mState.skyUI.dayTop);
    const glm::vec3 nightH = glm::make_vec3(mState.skyUI.nightHorizon);
    const glm::vec3 nightT = glm::make_vec3(mState.skyUI.nightTop);

    skyHorizon = glm::mix(nightH, dayH, dayFactor);
    skyTop = glm::mix(nightT, dayT, dayFactor);

    const glm::vec3 duskSun = mState.skyUI.sunDuskColor;
    const glm::vec3 nightSun = mState.skyUI.sunNightColor;
    const glm::vec3 daySun = mState.skyUI.sunDayColor;

    sunColor = glm::mix(nightSun, duskSun, duskFactor);
    sunColor = glm::mix(sunColor, daySun, dayFactor);

    const glm::vec3 visualDuskSun = mState.skyUI.visualSunDuskColor;
    const glm::vec3 visualNightSun = mState.skyUI.visualSunNightColor;
    const glm::vec3 visualDaySun = mState.skyUI.visualSunDayColor;
    visualSunColor = glm::mix(visualNightSun, visualDuskSun, duskFactor);
    visualSunColor = glm::mix(visualSunColor, visualDaySun, dayFactor);

    // Slightly tint the horizon at dusk for nicer gradients.
    skyHorizon = glm::mix(skyHorizon, visualSunColor, duskFactor * 0.25f);

    // Night atmosphere adjustments
    fogColor = glm::mix(fogColor, glm::vec3(0.03f, 0.05f, 0.10f), nightFactor);
    fogDensity = fogDensity * glm::mix(1.0f, 1.6f, nightFactor);
    emissiveBoost = glm::mix(1.0f, 1.8f, nightFactor);
    emissiveFlicker = 0.08f * nightFactor;
    skyExposure = mState.render.exposure * glm::mix(1.0f, 0.7f, nightFactor);
    skyGamma = glm::mix(mState.render.gamma, 1.9f, nightFactor);
    ambientRampStrength =
        mState.render.ambientRampStrength * glm::mix(1.0f, 0.6f, nightFactor);
    ambientRampTop = glm::mix(mState.render.ambientRampTop,
                              glm::vec3(0.08f, 0.12f, 0.18f), nightFactor);
    ambientRampBottom = glm::mix(mState.render.ambientRampBottom,
                                 glm::vec3(0.02f, 0.03f, 0.05f),
                                 nightFactor);
  }

  // Elevation-driven lighting model for more natural day/dusk look.
  float daylight = glm::clamp(-lightDir.y, 0.0f, 1.0f);
  float dayBlend = glm::smoothstep(0.03f, 0.30f, daylight);
  glm::vec3 sunriseTint(1.20f, 0.70f, 0.45f);
  glm::vec3 litSunColor =
      sunColor * glm::mix(sunriseTint, glm::vec3(1.0f), dayBlend);
  float litSunIntensity =
      mState.sun.lightIntensity * glm::mix(0.08f, 1.0f, daylight);
  litSunIntensity = glm::mix(litSunIntensity, litSunIntensity * 0.25f,
                             nightFactor);
  float litAmbient =
      glm::max(0.02f, mState.sun.ambientStrength * (0.25f + 0.75f * daylight));
  litAmbient = glm::mix(litAmbient, litAmbient * 0.5f, nightFactor);
  const float skyAmbientInfluence =
      glm::clamp(mState.render.ambientSkyInfluence, 0.0f, 1.0f);
  ambientSkyColor = glm::mix(ambientSkyColor, skyTop, skyAmbientInfluence);
  ambientHorizonColor =
      glm::mix(ambientHorizonColor, skyHorizon, skyAmbientInfluence);
  ambientGroundColor =
      glm::mix(ambientGroundColor, glm::vec3(0.02f, 0.03f, 0.05f),
               nightFactor * 0.55f);

  const bool torchLightActive =
      mState.gameplay.viewmodel.torchEnabled &&
      mState.gameplay.activeSlot == GameplayState::HotbarSlot::Torch &&
      (mState.playState == AppState::PlayState::Playing || mState.uiMode);
  const glm::vec3 camRight =
      glm::normalize(glm::cross(cameraFront, cameraUp));
  const float torchFlickerNoise =
      0.50f +
      0.50f * (0.50f * std::sin(nowT * 7.3f + 0.4f) +
               0.30f * std::sin(nowT * 12.7f + 1.9f) +
               0.20f * std::sin(nowT * 19.9f + 4.2f));
  const glm::vec3 torchLightPos =
      cameraPos + camRight * (0.34f + 0.020f * std::sin(nowT * 8.1f)) +
      cameraUp * (-0.12f + 0.018f * std::sin(nowT * 13.7f + 1.1f)) +
      cameraFront * (0.78f + 0.032f * std::sin(nowT * 10.4f + 2.3f));
  const glm::vec3 torchLightDir = glm::normalize(
      cameraFront * (0.82f + 0.05f * std::sin(nowT * 6.2f)) +
      camRight * (0.08f * std::sin(nowT * 9.4f + 0.8f)) +
      glm::vec3(0.0f, -0.57f - 0.05f * std::sin(nowT * 7.9f + 1.7f), 0.0f));
  const glm::vec3 torchLightColor =
      glm::mix(glm::vec3(0.95f, 0.34f, 0.08f),
               glm::vec3(1.0f, 0.80f, 0.42f), torchFlickerNoise * 0.65f);
  const float torchNightBoost = glm::mix(1.2f, 2.4f, nightFactor);
  const float torchFireIntensity =
      (8.2f + 3.4f * torchFlickerNoise) * torchNightBoost;
  const float torchFireConstant = 1.0f;
  const float torchFireLinear = 0.14f;
  const float torchFireQuadratic = 0.032f;
  const float torchFireFlicker = 0.22f;
  const float torchFireAmbient =
      (0.75f + 0.25f * torchFlickerNoise) * torchNightBoost;
  const float torchFireAmbientRadius = 9.5f + 1.6f * torchFlickerNoise;

  glEnable(GL_STENCIL_TEST);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  glStencilFunc(GL_ALWAYS, 1, 0xFF);
  glStencilMask(0xFF);

  const bool skyGpuTimerActive = beginMainGpuTimer(MainGpuTimerSky);
  // Replace beginFrame with PostProcessor integration
  mState.postProcessor.resize(mState.scrW, mState.scrH);
  logGlContextErrors("postProcessor.resize");
  mState.postProcessor.beginRenderPass();
  logGlContextErrors("postProcessor.beginRenderPass");

  // Clear depth and set clear color (since PostProcessor clear doesn't set
  // color)
  glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  glStencilMask(0x00);
  logGlContextErrors("main pass frame setup");

  if (mState.render.disableHDR) {
    mState.sky.setSolidSky(true);
    mState.sky.setSkyColors(skyHorizon, skyTop);
  } else {
    mState.sky.setSolidSky(false);
  }

  bool o_skyCloudsEnabled = mState.sky.skyCloudsEnabled;
  const float baseSkyCloudDensity = mState.sky.skyCloudDensity;
  const glm::vec3 baseSkyCloudColor = mState.sky.skyCloudColor;
  const float baseCloudAlpha = mState.cloud.alpha;
  const glm::vec3 baseCloudColor = mState.cloud.color;
  if (mState.render.disableClouds) {
    mState.sky.skyCloudsEnabled = false;
  }

  // Keep lighting and sky presentation separate: minimal sky only affects the
  // visible backdrop, not sun light intensity.
  const bool minimalSky = mState.skyUI.minimalSky;
  const float backdropBlend =
      glm::clamp(mState.skyUI.skyBackdropBlend, 0.0f, 1.0f);
  const float featureVisibility =
      glm::clamp(mState.skyUI.skyFeatureVisibility, 0.0f, 1.0f);
  if (minimalSky) {
    const glm::vec3 fogDrivenSky = glm::mix(fogColor, skyTop, 0.18f);
    skyHorizon = glm::mix(skyHorizon, fogDrivenSky, backdropBlend);
    skyTop = glm::mix(skyTop, fogDrivenSky,
                      glm::mix(backdropBlend, 1.0f, 0.35f));
    mState.sky.skyCloudDensity = baseSkyCloudDensity * featureVisibility;
    mState.sky.skyCloudColor =
        glm::mix(fogDrivenSky, baseSkyCloudColor, featureVisibility);
    mState.cloud.alpha =
        baseCloudAlpha * glm::mix(0.0f, 0.75f, featureVisibility);
    mState.cloud.color =
        glm::mix(fogDrivenSky, baseCloudColor, featureVisibility);
  }

  mState.sky.nightFactor = nightFactor;
  mState.sky.starIntensity = glm::mix(0.0f, 0.65f, nightFactor);
  mState.sky.milkyWayIntensity = glm::mix(0.0f, 0.35f, nightFactor);
  mState.sky.nightHorizonGlow = glm::mix(glm::vec3(0.0f),
                                         glm::vec3(0.08f, 0.12f, 0.20f),
                                         nightFactor);
  mState.sky.nightDitherStrength = glm::mix(0.0f, 0.006f, nightFactor);
  mState.sky.atmosphereStrength = mState.skyUI.skyAtmosphereStrength;
  mState.sky.gradientPower = mState.skyUI.skyGradientPower;
  mState.sky.horizonGlow = mState.skyUI.skyHorizonGlow;
  mState.sky.sunDiscSoftness = mState.skyUI.skySunDiscSoftness;
  mState.sky.sunHaloSize = mState.skyUI.skySunHaloSize;
  mState.sky.sunRaySharpness = mState.skyUI.skySunRaySharpness;
  mState.sky.useBlackHole = false;
  glm::vec3 blackHoleDir = glm::normalize(glm::vec3(-0.60f, 0.47f, -0.60f));
  {
    const float bhAz = glm::radians(mState.skyUI.blackHoleAzimuth);
    const float bhEl = glm::radians(mState.skyUI.blackHoleElevation);
    blackHoleDir = glm::normalize(glm::vec3(
        std::cos(bhEl) * std::sin(bhAz), std::sin(bhEl),
        std::cos(bhEl) * std::cos(bhAz)));
    if (mState.skyUI.blackHoleWorldMode) {
      const glm::vec3 toBlackHole =
          mState.skyUI.blackHoleWorldPosition - cameraPos;
      if (glm::dot(toBlackHole, toBlackHole) > 0.0001f)
        blackHoleDir = glm::normalize(toBlackHole);
    }
    mState.sky.blackHoleDir = blackHoleDir;
    mState.sky.blackHoleSizeDeg = mState.skyUI.blackHoleSizeDeg;
  }
  mState.sky.blackHoleDiskTiltDeg = mState.skyUI.blackHoleDiskTiltDeg;
  mState.sky.blackHoleDiskInclinationDeg =
      mState.skyUI.blackHoleDiskInclinationDeg;
  mState.sky.blackHoleColor = mState.skyUI.blackHoleColor;
  mState.sky.blackHoleRingIntensity = mState.skyUI.blackHoleRingIntensity;
  mState.sky.blackHoleRingWidth = mState.skyUI.blackHoleRingWidth;
  mState.sky.blackHoleDistortion = mState.skyUI.blackHoleDistortion;
  mState.sky.blackHoleHaloIntensity = mState.skyUI.blackHoleHaloIntensity;
  mState.sky.blackHoleDiskSpinSpeed = mState.skyUI.blackHoleDiskSpinSpeed;
  mState.sky.blackHoleDiskTurbulence = mState.skyUI.blackHoleDiskTurbulence;
  mState.sky.blackHoleChromaticAberration =
      mState.skyUI.blackHoleChromaticAberration;
  mState.sky.blackHoleEclipseStrength = mState.skyUI.blackHoleEclipseStrength;
  mState.sky.blackHolePhotonRingIntensity =
      mState.skyUI.blackHolePhotonRingIntensity;
  mState.sky.blackHoleDopplerBoost = mState.skyUI.blackHoleDopplerBoost;
  mState.sky.blackHoleJetIntensity = mState.skyUI.blackHoleJetIntensity;
  mState.sky.blackHoleCoronaIntensity = mState.skyUI.blackHoleCoronaIntensity;
  mState.sky.blackHoleStarLensIntensity =
      mState.skyUI.blackHoleStarLensIntensity;
  mState.sky.blackHoleShadowStrength = mState.skyUI.blackHoleShadowStrength;
  const float baseDisc = mState.skyUI.skySunDiscIntensity;
  const float baseHalo = mState.skyUI.skySunHaloIntensity;
  const float baseRays = mState.skyUI.skySunRaysIntensity;
  mState.sky.sunDiscIntensity =
      baseDisc * glm::mix(1.0f, 0.18f, nightFactor);
  mState.sky.sunHaloIntensity =
      baseHalo * glm::mix(1.0f, 0.25f, nightFactor);
  mState.sky.sunRaysIntensity =
      baseRays * glm::mix(1.0f, 0.05f, nightFactor);
  if (minimalSky) {
    mState.sky.sunDiscIntensity *= featureVisibility;
    mState.sky.sunHaloIntensity *= featureVisibility;
    mState.sky.sunRaysIntensity *= featureVisibility;
  }
  bool blackHoleRendered = false;
  if (mState.skyUI.useBlackHole && mState.blackHole.isReady()) {
  BlackHoleSettings blackHoleSettings;
  blackHoleSettings.enabled = true;
  blackHoleSettings.direction = blackHoleDir;
  blackHoleSettings.viewPitchDeg = mState.skyUI.blackHoleViewPitchDeg;
  blackHoleSettings.eventHorizonSizeDeg = mState.skyUI.blackHoleSizeDeg;
    if (mState.skyUI.blackHoleWorldMode) {
      const float distance =
          glm::length(mState.skyUI.blackHoleWorldPosition - cameraPos);
      const float radius = std::max(0.01f, mState.skyUI.blackHoleWorldRadius);
      const float angularDiameterDeg =
          glm::degrees(2.0f * std::atan(radius / std::max(distance, 0.01f)));
      blackHoleSettings.eventHorizonSizeDeg =
          std::clamp(angularDiameterDeg, 0.10f, 20.0f);
    }
    blackHoleSettings.diskTiltDeg = mState.skyUI.blackHoleDiskTiltDeg;
    blackHoleSettings.diskInclinationDeg =
        mState.skyUI.blackHoleDiskInclinationDeg;
    blackHoleSettings.innerDiskRadius =
        mState.skyUI.blackHoleInnerDiskRadius;
    blackHoleSettings.outerDiskRadius =
        mState.skyUI.blackHoleOuterDiskRadius;
    blackHoleSettings.diskTemperature =
        mState.skyUI.blackHoleDiskTemperature;
    blackHoleSettings.diskDensity = mState.skyUI.blackHoleDiskDensity;
    blackHoleSettings.diskTurbulence =
        mState.skyUI.blackHoleDiskTurbulence;
    blackHoleSettings.diskSpinSpeed = mState.skyUI.blackHoleDiskSpinSpeed;
    blackHoleSettings.diskFlowShear = mState.skyUI.blackHoleDiskFlowShear;
    blackHoleSettings.dopplerStrength = mState.skyUI.blackHoleDopplerBoost;
    blackHoleSettings.lensingStrength =
        mState.skyUI.blackHoleLensingStrength;
    blackHoleSettings.photonRingIntensity =
        mState.skyUI.blackHolePhotonRingIntensity;
    blackHoleSettings.ringWidth = mState.skyUI.blackHoleRingWidth;
    blackHoleSettings.coronaIntensity = mState.skyUI.blackHoleCoronaIntensity;
    blackHoleSettings.shadowStrength = mState.skyUI.blackHoleShadowStrength;
    blackHoleSettings.backgroundStarIntensity =
        mState.skyUI.blackHoleBackgroundStarIntensity;
    blackHoleSettings.exposure = mState.skyUI.blackHoleExposure;
    blackHoleSettings.diskColor = mState.skyUI.blackHoleColor;
    blackHoleSettings.quality = static_cast<BlackHoleQuality>(
        std::clamp(mState.skyUI.blackHoleQuality, 0, 2));
    mState.blackHole.draw(view, projection, blackHoleSettings, nowT);
    blackHoleRendered = true;
  }
  if (!blackHoleRendered) {
    mState.sky.draw(view, projection, skyExposure, skyGamma, mState.sun.sunDir,
                    visualSunColor, mState.sun.sunSize, nowT);
  }
  mState.sky.sunDiscIntensity = baseDisc;
  mState.sky.sunHaloIntensity = baseHalo;
  mState.sky.sunRaysIntensity = baseRays;

  // Fireflies (night-only)
  static float lastFireflyT = 0.0f;
  float fireflyDt = nowT - lastFireflyT;
  if (fireflyDt < 0.0f)
    fireflyDt = 0.0f;
  if (fireflyDt > 0.05f)
    fireflyDt = 0.05f;
  lastFireflyT = nowT;
  float fireflyFactor =
      mState.skyUI.firefliesEnabled
          ? (mState.skyUI.dayNightEnabled ? nightFactor : 1.0f)
          : 0.0f;
  if (fireflyFactor > 0.0f) {
    mState.fireflies.update(fireflyDt, cameraPos, fireflyFactor,
                            mState.skyUI.fireflyCount,
                            mState.skyUI.fireflyRadius,
                            mState.skyUI.fireflyHeightMin,
                            mState.skyUI.fireflyHeightMax);
  }

  mState.sky.skyCloudsEnabled = o_skyCloudsEnabled;
  mState.sky.skyCloudDensity = baseSkyCloudDensity;
  mState.sky.skyCloudColor = baseSkyCloudColor;

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);

  if (!mState.render.disableClouds) {
    mState.cloud.draw(view, projection, cameraPos, nowT, litSunColor,
                      litSunIntensity, lightDir);
  }
  mState.cloud.alpha = baseCloudAlpha;
  mState.cloud.color = baseCloudColor;
  endMainGpuTimer(skyGpuTimerActive);

  auto applyAtmosphereUniforms = [&](Shader &shader) {
    shader.setBool("uAerialPerspectiveEnabled",
                   mState.render.aerialPerspectiveEnabled);
    shader.setFloat("uAerialPerspectiveDensity",
                    mState.render.aerialPerspectiveDensity);
    shader.setFloat("uAerialPerspectiveStart",
                    mState.render.aerialPerspectiveStart);
    shader.setFloat("uAerialPerspectiveHeightFalloff",
                    mState.render.aerialPerspectiveHeightFalloff);
    shader.setFloat("uAerialPerspectiveSkyBlend",
                    mState.render.aerialPerspectiveSkyBlend);
    shader.setFloat("uAerialPerspectiveSunGlow",
                    mState.render.aerialPerspectiveSunGlow);
    shader.setFloat("uAerialPerspectiveDesaturation",
                    mState.render.aerialPerspectiveDesaturation);
    shader.setVec3("uAerialHorizonColor", skyHorizon);
    shader.setVec3("uAerialZenithColor", skyTop);
  };
  auto applyAmbientUniforms = [&](Shader &shader) {
    shader.setBool("uAmbientHemiEnabled",
                   mState.render.ambientHemisphereEnabled);
    shader.setFloat("uAmbientHemiIntensity",
                    mState.render.ambientHemisphereIntensity);
    shader.setFloat("uAmbientHorizonStrength",
                    mState.render.ambientHorizonStrength);
    shader.setFloat("uAmbientTerrainBoost", mState.render.ambientTerrainBoost);
    shader.setVec3("uAmbientSkyColor", ambientSkyColor);
    shader.setVec3("uAmbientHorizonColor", ambientHorizonColor);
    shader.setVec3("uAmbientGroundColor", ambientGroundColor);
  };

  mState.renderer.shader().activate();

  mState.renderer.shader().setInt("texture1", 0);
  mState.renderer.shader().setInt("shadowCube", 1);
  mState.renderer.shader().setInt("uEnvMap", 2);

  mState.renderer.setFrameUniforms(
      view, projection, mState.render.mixVal, nowT, litSunColor, litAmbient,
      cameraPos, litSunIntensity, lightDir, far_plane,
      mState.render.shadowStrength, mState.render.exposure, mState.render.gamma,
      fogColor, fogDensity,
      mState.render.fogHeightFalloff,
      mState.render.ambientHemisphereEnabled,
      mState.render.ambientHemisphereIntensity,
      mState.render.ambientHorizonStrength,
      mState.render.ambientTerrainBoost,
      ambientSkyColor, ambientHorizonColor, ambientGroundColor,
      mState.render.toonEnabled, mState.render.toonSteps, mState.render.toonMin,
      mState.render.shadowBandEnabled, mState.render.shadowBandSteps,
      mState.render.shadowBandSoftness, mState.render.ambientRampEnabled,
      ambientRampStrength, ambientRampTop, ambientRampBottom,
      mState.render.rimEnabled, mState.render.rimPower, mState.render.rimStrength,
      mState.render.rimColor, emissiveBoost, emissiveFlicker);

  mState.renderer.shader().setMat4("uLightSpaceMatrix",
                                   mState.render.lightSpaceMatrix);
  applyShadowUniforms(mState.renderer.shader());
  applyAtmosphereUniforms(mState.renderer.shader());
  applyAmbientUniforms(mState.renderer.shader());
  const bool envAvailable =
      !mState.render.disableHDR && mState.sky.hasHDRTexture();
  mState.renderer.shader().setBool("uEnvMapAvailable", envAvailable);
  mState.renderer.shader().setMat3("uEnvRotation", mState.sky.rotationMatrix());
  mState.renderer.shader().setFloat("uEnvYaw", mState.sky.yaw01());
  mState.renderer.shader().setFloat(
      "uEnvIntensity", glm::mix(0.28f, 0.12f, nightFactor));
  mState.renderer.shader().setFloat(
      "uEnvDiffuseStrength", glm::mix(1.15f, 0.65f, nightFactor));
  mState.renderer.shader().setFloat(
      "uEnvSpecularStrength", glm::mix(1.0f, 0.55f, nightFactor));
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, envAvailable ? mState.sky.hdrTexture() : 0);
  glActiveTexture(GL_TEXTURE0);
  mState.renderer.shader().setBool("uHasFire", torchLightActive);
  mState.renderer.shader().setVec3("uFirePos", torchLightPos);
  mState.renderer.shader().setVec3("uFireDir", torchLightDir);
  mState.renderer.shader().setVec3("uFireColor", torchLightColor);
  mState.renderer.shader().setFloat("uFireIntensity", torchFireIntensity);
  mState.renderer.shader().setFloat("uFireConstant", torchFireConstant);
  mState.renderer.shader().setFloat("uFireLinear", torchFireLinear);
  mState.renderer.shader().setFloat("uFireQuadratic", torchFireQuadratic);
  mState.renderer.shader().setFloat("uFireFlicker", torchFireFlicker);
  mState.renderer.shader().setFloat("uFireAmbient", torchFireAmbient);
  mState.renderer.shader().setFloat("uFireAmbientRadius",
                                    torchFireAmbientRadius);

  const TerrainMaterialSettings &tm = mState.terrainMaterial;
  const bool terrainLayerTexturesActive = tm.useLayerTextures;
  const bool terrainGroundTexturesActive =
      tm.useGroundTextures && !terrainLayerTexturesActive;
  if (terrainGroundTexturesActive || terrainLayerTexturesActive) {
    namespace fs = std::filesystem;
    auto isLossyImage = [](const std::string &path) {
      std::string ext = fs::path(path).extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c) { return (char)std::tolower(c); });
      return ext == ".jpg" || ext == ".jpeg";
    };
    if (terrainGroundTexturesActive) {
      if (tm.groundAlbedoPath.empty()) {
        logTerrainMaterialWarningOnce(
            "ground-albedo-missing",
            "Ground textures are enabled but Ground Albedo is empty.");
      } else if (!fs::exists(tm.groundAlbedoPath)) {
        logTerrainMaterialWarningOnce(
            "ground-albedo-missing-file:" + tm.groundAlbedoPath,
            "Ground albedo texture does not exist: " + tm.groundAlbedoPath);
      }
      if (!tm.groundNormalPath.empty() && !fs::exists(tm.groundNormalPath)) {
        logTerrainMaterialWarningOnce(
            "ground-normal-missing-file:" + tm.groundNormalPath,
            "Ground normal texture does not exist: " + tm.groundNormalPath);
      }
      if (!tm.groundRoughnessPath.empty() &&
          !fs::exists(tm.groundRoughnessPath)) {
        logTerrainMaterialWarningOnce(
            "ground-roughness-missing-file:" + tm.groundRoughnessPath,
            "Ground roughness texture does not exist: " +
                tm.groundRoughnessPath);
      }
      if (!tm.groundHeightPath.empty() && !fs::exists(tm.groundHeightPath)) {
        logTerrainMaterialWarningOnce(
            "ground-height-missing-file:" + tm.groundHeightPath,
            "Ground height texture does not exist: " + tm.groundHeightPath);
      }
      if (isLossyImage(tm.groundNormalPath)) {
        logTerrainMaterialWarningOnce(
            "ground-normal-lossy:" + tm.groundNormalPath,
            "Ground normal uses a JPG texture. PNG is strongly recommended for normal maps: " +
                tm.groundNormalPath);
      }
      if (isLossyImage(tm.groundRoughnessPath)) {
        logTerrainMaterialWarningOnce(
            "ground-roughness-lossy:" + tm.groundRoughnessPath,
            "Ground roughness uses a JPG texture. PNG is recommended for data maps: " +
                tm.groundRoughnessPath);
      }
      if (isLossyImage(tm.groundHeightPath)) {
        logTerrainMaterialWarningOnce(
            "ground-height-lossy:" + tm.groundHeightPath,
            "Ground height uses a JPG texture. PNG is recommended for height data: " +
                tm.groundHeightPath);
      }
    }
    if (terrainLayerTexturesActive) {
      if (!tm.grassAlbedoPath.empty() && !fs::exists(tm.grassAlbedoPath)) {
        logTerrainMaterialWarningOnce(
            "grass-albedo-missing-file:" + tm.grassAlbedoPath,
            "Grass albedo texture does not exist: " + tm.grassAlbedoPath);
      }
      if (!tm.grassNormalPath.empty() && !fs::exists(tm.grassNormalPath)) {
        logTerrainMaterialWarningOnce(
            "grass-normal-missing-file:" + tm.grassNormalPath,
            "Grass normal texture does not exist: " + tm.grassNormalPath);
      }
      if (!tm.grassRoughnessPath.empty() &&
          !fs::exists(tm.grassRoughnessPath)) {
        logTerrainMaterialWarningOnce(
            "grass-roughness-missing-file:" + tm.grassRoughnessPath,
            "Grass roughness texture does not exist: " +
                tm.grassRoughnessPath);
      }
      if (!tm.dirtAlbedoPath.empty() && !fs::exists(tm.dirtAlbedoPath)) {
        logTerrainMaterialWarningOnce(
            "dirt-albedo-missing-file:" + tm.dirtAlbedoPath,
            "Dirt albedo texture does not exist: " + tm.dirtAlbedoPath);
      }
      if (!tm.dirtNormalPath.empty() && !fs::exists(tm.dirtNormalPath)) {
        logTerrainMaterialWarningOnce(
            "dirt-normal-missing-file:" + tm.dirtNormalPath,
            "Dirt normal texture does not exist: " + tm.dirtNormalPath);
      }
      if (!tm.dirtRoughnessPath.empty() && !fs::exists(tm.dirtRoughnessPath)) {
        logTerrainMaterialWarningOnce(
            "dirt-roughness-missing-file:" + tm.dirtRoughnessPath,
            "Dirt roughness texture does not exist: " + tm.dirtRoughnessPath);
      }
      if (isLossyImage(tm.grassNormalPath)) {
        logTerrainMaterialWarningOnce(
            "grass-normal-lossy:" + tm.grassNormalPath,
            "Grass normal uses a JPG texture. PNG is strongly recommended for normal maps: " +
                tm.grassNormalPath);
      }
      if (isLossyImage(tm.grassRoughnessPath)) {
        logTerrainMaterialWarningOnce(
            "grass-roughness-lossy:" + tm.grassRoughnessPath,
            "Grass roughness uses a JPG texture. PNG is recommended for data maps: " +
                tm.grassRoughnessPath);
      }
      if (isLossyImage(tm.dirtNormalPath)) {
        logTerrainMaterialWarningOnce(
            "dirt-normal-lossy:" + tm.dirtNormalPath,
            "Dirt normal uses a JPG texture. PNG is strongly recommended for normal maps: " +
                tm.dirtNormalPath);
      }
      if (isLossyImage(tm.dirtRoughnessPath)) {
        logTerrainMaterialWarningOnce(
            "dirt-roughness-lossy:" + tm.dirtRoughnessPath,
            "Dirt roughness uses a JPG texture. PNG is recommended for data maps: " +
                tm.dirtRoughnessPath);
      }
    }
  }
  mState.renderer.shader().setBool("uTerrainMaterialEnabled", tm.enableCustom);
  mState.renderer.shader().setFloat("uTerrainMacroScale", tm.macroScale);
  mState.renderer.shader().setFloat("uTerrainDetailScale", tm.detailScale);
  mState.renderer.shader().setFloat("uTerrainNormalDetailScale",
                                    tm.normalDetailScale);
  mState.renderer.shader().setFloat("uTerrainNormalStrength",
                                    tm.normalStrength);
  mState.renderer.shader().setFloat("uTerrainCliffStart", tm.cliffStart);
  mState.renderer.shader().setFloat("uTerrainCliffEnd", tm.cliffEnd);
  mState.renderer.shader().setFloat("uTerrainSnowStart", tm.snowStartHeight);
  mState.renderer.shader().setFloat("uTerrainSnowEnd", tm.snowEndHeight);
  mState.renderer.shader().setFloat("uTerrainLowStart", tm.lowStartHeight);
  mState.renderer.shader().setFloat("uTerrainLowEnd", tm.lowEndHeight);
  mState.renderer.shader().setFloat("uTerrainMacroVariationStrength",
                                    tm.macroVariationStrength);
  mState.renderer.shader().setFloat("uTerrainCliffDesatStrength",
                                    tm.cliffDesatStrength);
  mState.renderer.shader().setVec3("uTerrainGrassA", tm.grassA);
  mState.renderer.shader().setVec3("uTerrainGrassB", tm.grassB);
  mState.renderer.shader().setVec3("uTerrainDirtA", tm.dirtA);
  mState.renderer.shader().setVec3("uTerrainDirtB", tm.dirtB);
  mState.renderer.shader().setVec3("uTerrainRockA", tm.rockA);
  mState.renderer.shader().setVec3("uTerrainRockB", tm.rockB);
  mState.renderer.shader().setVec3("uTerrainSandA", tm.sandA);
  mState.renderer.shader().setVec3("uTerrainSandB", tm.sandB);
  mState.renderer.shader().setVec3("uTerrainSnowA", tm.snowA);
  mState.renderer.shader().setVec3("uTerrainSnowB", tm.snowB);
  mState.renderer.shader().setFloat("uTerrainRoughGrass", tm.roughGrass);
  mState.renderer.shader().setFloat("uTerrainRoughDirt", tm.roughDirt);
  mState.renderer.shader().setFloat("uTerrainRoughRock", tm.roughRock);
  mState.renderer.shader().setFloat("uTerrainRoughSand", tm.roughSand);
  mState.renderer.shader().setFloat("uTerrainRoughSnow", tm.roughSnow);
  const GLuint terrainGrassAlbedo =
      terrainLayerTexturesActive
          ? LoadTexture2DCached(tm.grassAlbedoPath, true, TextureUsage::Color)
          : 0;
  const GLuint terrainGrassNormal =
      terrainLayerTexturesActive
          ? LoadTexture2DCached(tm.grassNormalPath, true, TextureUsage::Data)
          : 0;
  const GLuint terrainGrassRoughness =
      terrainLayerTexturesActive
          ? LoadTexture2DCached(tm.grassRoughnessPath, true,
                                TextureUsage::Data)
          : 0;
  const GLuint terrainDirtAlbedo =
      terrainLayerTexturesActive
          ? LoadTexture2DCached(tm.dirtAlbedoPath, true, TextureUsage::Color)
          : 0;
  const GLuint terrainDirtNormal =
      terrainLayerTexturesActive
          ? LoadTexture2DCached(tm.dirtNormalPath, true, TextureUsage::Data)
          : 0;
  const GLuint terrainDirtRoughness =
      terrainLayerTexturesActive
          ? LoadTexture2DCached(tm.dirtRoughnessPath, true,
                                TextureUsage::Data)
          : 0;
  mState.renderer.shader().setBool("uTerrainUseLayerTextures",
                                   terrainLayerTexturesActive);
  mState.renderer.shader().setBool("uTerrainHasGrassAlbedo",
                                   terrainGrassAlbedo != 0);
  mState.renderer.shader().setBool("uTerrainHasGrassNormal",
                                   terrainGrassNormal != 0);
  mState.renderer.shader().setBool("uTerrainHasGrassRoughness",
                                   terrainGrassRoughness != 0);
  mState.renderer.shader().setBool("uTerrainHasDirtAlbedo",
                                   terrainDirtAlbedo != 0);
  mState.renderer.shader().setBool("uTerrainHasDirtNormal",
                                   terrainDirtNormal != 0);
  mState.renderer.shader().setBool("uTerrainHasDirtRoughness",
                                   terrainDirtRoughness != 0);
  mState.renderer.shader().setFloat("uTerrainLayerTextureTiling",
                                    tm.layerTextureTiling);
  mState.renderer.shader().setFloat("uTerrainLayerTextureStrength",
                                    tm.layerTextureStrength);
  mState.renderer.shader().setFloat("uTerrainLayerNormalStrength",
                                    tm.layerNormalStrength);
  mState.renderer.shader().setFloat("uTerrainLayerRoughnessStrength",
                                    tm.layerRoughnessStrength);
  const GLuint terrainGroundAlbedo =
      terrainGroundTexturesActive
          ? LoadTexture2DCached(tm.groundAlbedoPath, true,
                                TextureUsage::Color)
          : 0;
  const GLuint terrainGroundNormal =
      terrainGroundTexturesActive
          ? LoadTexture2DCached(tm.groundNormalPath, true,
                                TextureUsage::Data)
          : 0;
  const GLuint terrainGroundRoughness =
      terrainGroundTexturesActive
          ? LoadTexture2DCached(tm.groundRoughnessPath, true,
                                TextureUsage::Data)
          : 0;
  const GLuint terrainGroundHeight =
      terrainGroundTexturesActive
          ? LoadTexture2DCached(tm.groundHeightPath, true,
                                TextureUsage::Data)
          : 0;
  mState.renderer.shader().setBool(
      "uTerrainUseGroundTextures",
      terrainGroundTexturesActive && terrainGroundAlbedo != 0);
  mState.renderer.shader().setBool(
      "uTerrainGroundFullOverride",
      terrainGroundTexturesActive && terrainGroundAlbedo != 0 &&
          tm.groundFullOverride);
  mState.renderer.shader().setBool("uTerrainHasGroundNormal",
                                   terrainGroundNormal != 0);
  mState.renderer.shader().setBool("uTerrainHasGroundRoughness",
                                   terrainGroundRoughness != 0);
  mState.renderer.shader().setBool("uTerrainHasGroundHeight",
                                   terrainGroundHeight != 0);
  mState.renderer.shader().setFloat("uTerrainGroundTiling", tm.groundTiling);
  mState.renderer.shader().setFloat("uTerrainGroundBlendStrength",
                                    tm.groundBlendStrength);
  mState.renderer.shader().setFloat("uTerrainGroundRoughnessValue",
                                    tm.groundRoughness);
  mState.renderer.shader().setFloat("uTerrainGroundHeightStrength",
                                    tm.groundHeightStrength);
  mState.renderer.shader().setBool("uTerrainGroundPseudoHeightEnabled",
                                   tm.groundPseudoHeightEnabled);
  mState.renderer.shader().setInt("uTerrainGroundPseudoHeightSource",
                                  tm.groundPseudoHeightSource);
  mState.renderer.shader().setFloat("uTerrainGroundPseudoHeightContrast",
                                    tm.groundPseudoHeightContrast);
  mState.renderer.shader().setFloat("uTerrainGroundPseudoHeightBias",
                                    tm.groundPseudoHeightBias);
  mState.renderer.shader().setBool("uTerrainGroundGradeEnabled",
                                   tm.groundGradeEnabled);
  mState.renderer.shader().setFloat("uTerrainGroundGradeSaturation",
                                    tm.groundGradeSaturation);
  mState.renderer.shader().setFloat("uTerrainGroundGradeContrast",
                                    tm.groundGradeContrast);
  mState.renderer.shader().setFloat("uTerrainGroundGradeGamma",
                                    tm.groundGradeGamma);
  mState.renderer.shader().setVec3("uTerrainGroundGradeTint",
                                   tm.groundGradeTint);
  mState.renderer.shader().setFloat("uTerrainGroundBrightness",
                                    tm.groundBrightness);
  mState.renderer.shader().setFloat("uTerrainGroundVariationStrength",
                                    tm.groundVariationStrength);
  mState.renderer.shader().setFloat("uTerrainGroundVariationScale",
                                    tm.groundVariationScale);
  mState.renderer.shader().setBool("uTerrainSunGlintEnabled",
                                   tm.sunGlintEnabled);
  mState.renderer.shader().setFloat("uTerrainSunGlintIntensity",
                                    tm.sunGlintIntensity);
  mState.renderer.shader().setFloat("uTerrainSunGlintSharpness",
                                    tm.sunGlintSharpness);
  mState.renderer.shader().setFloat("uTerrainSunGlintMaskScale",
                                    tm.sunGlintMaskScale);
  mState.renderer.shader().setFloat("uTerrainSunGlintMaskStrength",
                                    tm.sunGlintMaskStrength);
  mState.renderer.shader().setFloat("uTerrainSunGlintBaseSpecular",
                                    tm.sunGlintBaseSpecular);
  const float glintAzimuth = glm::radians(tm.sunGlintDirectionAzimuth);
  const float glintElevation = glm::radians(tm.sunGlintDirectionElevation);
  const glm::vec3 manualTerrainGlintDir = glm::normalize(glm::vec3(
      std::cos(glintElevation) * std::sin(glintAzimuth),
      std::sin(glintElevation),
      std::cos(glintElevation) * std::cos(glintAzimuth)));
  const glm::vec3 sceneSunGlintDir = glm::normalize(-lightDir);
  const glm::vec3 terrainGlintDir =
      tm.sunGlintUseSceneSun ? sceneSunGlintDir : manualTerrainGlintDir;
  mState.renderer.shader().setVec3("uTerrainSunGlintDirection",
                                   terrainGlintDir);
  mState.renderer.shader().setFloat("uTerrainSunGlintBandWidth",
                                    tm.sunGlintBandWidth);
  mState.renderer.shader().setInt("uTerrainGroundAlbedo", 8);
  mState.renderer.shader().setInt("uTerrainGroundRoughness", 9);
  mState.renderer.shader().setInt("uTerrainGroundNormal", 10);
  mState.renderer.shader().setInt("uTerrainGroundHeight", 11);
  mState.renderer.shader().setInt("uTerrainGrassAlbedo", 12);
  mState.renderer.shader().setInt("uTerrainDirtAlbedo", 13);
  mState.renderer.shader().setInt("uTerrainGrassNormal", 8);
  mState.renderer.shader().setInt("uTerrainGrassRoughness", 9);
  mState.renderer.shader().setInt("uTerrainDirtNormal", 10);
  mState.renderer.shader().setInt("uTerrainDirtRoughness", 11);
  glActiveTexture(GL_TEXTURE8);
  glBindTexture(GL_TEXTURE_2D, terrainLayerTexturesActive
                                   ? terrainGrassNormal
                                   : terrainGroundAlbedo);
  glActiveTexture(GL_TEXTURE9);
  glBindTexture(GL_TEXTURE_2D, terrainLayerTexturesActive
                                   ? terrainGrassRoughness
                                   : terrainGroundRoughness);
  glActiveTexture(GL_TEXTURE10);
  glBindTexture(GL_TEXTURE_2D, terrainLayerTexturesActive
                                   ? terrainDirtNormal
                                   : terrainGroundNormal);
  glActiveTexture(GL_TEXTURE11);
  glBindTexture(GL_TEXTURE_2D, terrainLayerTexturesActive
                                   ? terrainDirtRoughness
                                   : terrainGroundHeight);
  glActiveTexture(GL_TEXTURE12);
  glBindTexture(GL_TEXTURE_2D, terrainGrassAlbedo);
  glActiveTexture(GL_TEXTURE13);
  glBindTexture(GL_TEXTURE_2D, terrainDirtAlbedo);
  glActiveTexture(GL_TEXTURE0);
  mState.renderer.shader().setBool("uTerrainFlatGreenEnabled",
                                   tm.flatGreenEnabled &&
                                       !terrainLayerTexturesActive);
  mState.renderer.shader().setVec3("uTerrainFlatGreenColor", tm.flatGreenColor);
  mState.renderer.shader().setInt("uTerrainMaterialQuality",
                                  (int)mState.terrainSettings.materialQuality);

  const bool gpuTerrain = mState.terrainSystem.gpuTerrainActive();
  const bool useFlatTerrainShader = tm.flatGreenEnabled &&
                                    !terrainGroundTexturesActive &&
                                    !terrainLayerTexturesActive &&
                                    mState.terrainFlatShader != nullptr &&
                                    !gpuTerrain;
  const bool keepObjTerrainFallback =
      gpuTerrain && mState.terrainSystem.stats().gpuTerrainFallback;
  const bool terrainGpuTimerActive = beginMainGpuTimer(MainGpuTimerTerrain);
  if (useFlatTerrainShader) {
    Shader &terrainSh = *mState.terrainFlatShader;
    terrainSh.activate();
    terrainSh.setMat4("view", view);
    terrainSh.setMat4("projection", projection);
    terrainSh.setMat4("uViewMatrix", view);
    terrainSh.setVec3("uSunColor", litSunColor);
    terrainSh.setFloat("uSunIntensity", litSunIntensity);
    terrainSh.setFloat("uAmbient", litAmbient);
    terrainSh.setVec3("uLightDir", lightDir);
    terrainSh.setMat4("uLightSpaceMatrix", mState.render.lightSpaceMatrix);
    terrainSh.setFloat("uShadowStrength", mState.render.shadowStrength);
    applyShadowUniforms(terrainSh);
    terrainSh.setVec3("uCameraPos", cameraPos);
    terrainSh.setVec3("uFogColor", fogColor);
    terrainSh.setFloat("uFogDensity", fogDensity);
    terrainSh.setFloat("uFogHeightFalloff", mState.render.fogHeightFalloff);
    applyAtmosphereUniforms(terrainSh);
    applyAmbientUniforms(terrainSh);
    terrainSh.setBool("uHasFire", torchLightActive);
    terrainSh.setVec3("uFirePos", torchLightPos);
    terrainSh.setVec3("uFireDir", torchLightDir);
    terrainSh.setVec3("uFireColor", torchLightColor);
    terrainSh.setFloat("uFireIntensity", torchFireIntensity);
    terrainSh.setFloat("uFireConstant", torchFireConstant);
    terrainSh.setFloat("uFireLinear", torchFireLinear);
    terrainSh.setFloat("uFireQuadratic", torchFireQuadratic);
    terrainSh.setFloat("uFireFlicker", torchFireFlicker);
    terrainSh.setFloat("uFireAmbient", torchFireAmbient);
    terrainSh.setFloat("uFireAmbientRadius", torchFireAmbientRadius);
    terrainSh.setFloat("uTime", nowT);
    terrainSh.setVec3("uTerrainFlatGreenColor", tm.flatGreenColor);
    terrainSh.setInt("shadowMap", 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mState.renderer.shadowTex());
    glActiveTexture(GL_TEXTURE0);

    mState.renderSystem.update(
        mState.scene.registry(), terrainSh, false, 0, false, false,
        RenderSystem::TerrainFilter::OnlyTerrain);

    // Restore main shader after terrain-only pass so non-terrain draws use the
    // correct program (terrain shader has no instancing path).
    mState.renderer.shader().activate();
  }

  if (gpuTerrain) {
    mState.terrainSystem.renderGpuTerrain(mState.renderer.shader(),
                                          projection * view, cameraPos, false);
  }
  if (!gpuTerrain || keepObjTerrainFallback) {
    mState.renderSystem.update(mState.scene.registry(), mState.renderer.shader(),
                               false, 0, false, false,
                               RenderSystem::TerrainFilter::OnlyTerrain);
  }
  endMainGpuTimer(terrainGpuTimerActive);

  const bool sceneGpuTimerActive = beginMainGpuTimer(MainGpuTimerScene);
  mState.renderSystem.update(
      mState.scene.registry(), mState.renderer.shader(), false,
      mState.editorSubsystem->selection().selectedEntityId, false, false,
      RenderSystem::TerrainFilter::ExcludeTerrain);

  if (fireflyFactor > 0.0f) {
    mState.fireflies.draw(view, projection, nowT, fireflyFactor,
                          mState.skyUI.fireflySize,
                          mState.skyUI.fireflyIntensity,
                          mState.skyUI.fireflyColor);
  }

  mState.projectiles.draw(view, projection, cameraPos, 0.25f);

  if (mState.editorSubsystem->selection().selectedEntityId != 0 && mState.outlineShader) {
    glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    glStencilMask(0x00);

    mState.outlineShader->activate();
    mState.outlineShader->setMat4("view", view);
    mState.outlineShader->setMat4("projection", projection);
    mState.renderSystem.update(mState.scene.registry(), *mState.outlineShader,
                               false, mState.editorSubsystem->selection().selectedEntityId, true,
                               false, RenderSystem::TerrainFilter::All);

    glStencilMask(0xFF);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
  }
  glDisable(GL_STENCIL_TEST);

  // Viewmodel pass — screen-space, no camera correlation.
  const bool drawViewmodelAxe =
      mState.gameplay.viewmodel.axeEnabled &&
      mState.gameplay.activeSlot == GameplayState::HotbarSlot::Axe &&
      mState.gameplay.axeEntity != 0;
  const bool drawViewmodelTorch =
      mState.gameplay.viewmodel.torchEnabled &&
      mState.gameplay.activeSlot == GameplayState::HotbarSlot::Torch &&
      mState.gameplay.torchEntity != 0;
  if (drawViewmodelAxe || drawViewmodelTorch) {
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    float aspect = (float)mState.scrW / (float)mState.scrH;
    glm::mat4 vmView(1.0f);
    glm::mat4 vmProj = glm::perspective(glm::radians(mState.input.fov), aspect,
                                        0.01f, 50.0f);

    mState.renderer.shader().activate();
    mState.renderer.setFrameUniforms(
        vmView, vmProj, mState.render.mixVal, nowT, litSunColor, litAmbient,
        glm::vec3(0.0f), litSunIntensity, lightDir, far_plane,
        mState.render.shadowStrength, mState.render.exposure, mState.render.gamma,
        fogColor, fogDensity,
        mState.render.fogHeightFalloff,
        mState.render.ambientHemisphereEnabled,
        mState.render.ambientHemisphereIntensity,
        mState.render.ambientHorizonStrength,
        mState.render.ambientTerrainBoost,
        ambientSkyColor, ambientHorizonColor, ambientGroundColor,
        mState.render.toonEnabled,
        mState.render.toonSteps, mState.render.toonMin,
        mState.render.shadowBandEnabled, mState.render.shadowBandSteps,
        mState.render.shadowBandSoftness, mState.render.ambientRampEnabled,
        ambientRampStrength, ambientRampTop, ambientRampBottom,
        mState.render.rimEnabled, mState.render.rimPower,
        mState.render.rimStrength, mState.render.rimColor, emissiveBoost,
        emissiveFlicker);

    mState.renderSystem.update(mState.scene.registry(), mState.renderer.shader(),
                               false, 0, false, true,
                               RenderSystem::TerrainFilter::All);

    if (drawViewmodelTorch &&
        mState.scene.registry().has<TransformComponent>(mState.gameplay.torchEntity)) {
      const auto &torchTr =
          mState.scene.registry().get<TransformComponent>(mState.gameplay.torchEntity);
      const glm::vec3 firePos = glm::vec3(
          torchTr.getMatrix() * glm::vec4(0.0f, 0.72f, 0.0f, 1.0f));
      FireFXParams savedParams = mState.fire.params();
      auto &fireParams = mState.fire.params();
      fireParams.enabled = true;
      fireParams.offset = glm::vec3(0.0f);
      fireParams.size = 0.16f;
      fireParams.intensity = 1.65f;
      fireParams.smokeOpacity = 0.28f;
      fireParams.smokeScaleXY = 1.35f;
      fireParams.smokeScaleY = 1.8f;
      fireParams.smokeLift = 0.65f;
      mState.fire.draw(vmView, vmProj, glm::vec3(0.0f), firePos, nowT);
      fireParams = savedParams;
    }

    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
  }

  if (mState.playState != AppState::PlayState::Playing) {
    // GL-side collider wireframes (extracted from PhysicsSystem so the
    // simulation stays graphics-API-free).
    static PhysicsDebugRenderer sPhysicsDebugRenderer;
    sPhysicsDebugRenderer.drawColliders(mState.scene.registry(), view,
                                        projection, mState.renderer.shader());
  }
  endMainGpuTimer(sceneGpuTimerActive);

  // Finish post-processing (bloom + SSAO + volumetrics) and blit to screen.
  auto &pp = mState.postProcessor;
  pp.displayGamma = std::clamp(mState.render.gamma, 0.8f, 4.0f);
  pp.setForceSsaoFullRes(
      mState.terrainMaterial.flatGreenEnabled && pp.ssaoFullResTerrain);
  const bool baseEnableColorGrade = pp.enableColorGrade;
  const float baseBloomIntensity = pp.bloomIntensity;
  const float baseBloomThreshold = pp.bloomThreshold;
  const float baseBrightness = pp.brightness;
  const bool baseEnableDistanceTint = pp.enableDistanceTint;
  const glm::vec3 baseDistanceTintColor = pp.distanceTintColor;
  const float baseVolumetricDensity = pp.volumetricFogDensity;
  const float baseGradeSaturation = pp.gradeSaturation;
  const float baseGradeContrast = pp.gradeContrast;
  const float baseGradeLift = pp.gradeLift;
  const float baseGradeGamma = pp.gradeGamma;
  const float baseGradeGain = pp.gradeGain;
  const glm::vec3 baseGradeTint = pp.gradeTint;

  if (nightFactor > 0.001f) {
    pp.enableColorGrade = true;
    pp.gradeSaturation = glm::mix(baseGradeSaturation, 0.85f, nightFactor);
    pp.gradeContrast = glm::mix(baseGradeContrast, 1.08f, nightFactor);
    pp.gradeLift = glm::mix(baseGradeLift, -0.03f, nightFactor);
    pp.gradeGamma = glm::mix(baseGradeGamma, 1.12f, nightFactor);
    pp.gradeGain = glm::mix(baseGradeGain, 0.95f, nightFactor);
    pp.gradeTint =
        glm::mix(baseGradeTint, glm::vec3(0.75f, 0.85f, 1.05f), nightFactor);

    pp.bloomIntensity = glm::mix(baseBloomIntensity, 1.6f, nightFactor);
    pp.bloomThreshold = glm::mix(baseBloomThreshold, 0.6f, nightFactor);
    pp.brightness = glm::mix(baseBrightness, 0.95f, nightFactor);

    pp.enableDistanceTint = true;
    pp.distanceTintColor =
        glm::mix(baseDistanceTintColor, glm::vec3(0.07f, 0.10f, 0.16f),
                 nightFactor);
    pp.volumetricFogDensity =
        glm::mix(baseVolumetricDensity, baseVolumetricDensity * 1.3f,
                 nightFactor);
  }

  const bool postGpuTimerActive = beginMainGpuTimer(MainGpuTimerPost);
  pp.endRenderPass(view, projection, cameraPos, lightDir, litSunColor, 0.1f,
                   500.0f, nowT);
  endMainGpuTimer(postGpuTimerActive);

  // Restore post-process settings so UI values stay stable.
  pp.enableColorGrade = baseEnableColorGrade;
  pp.bloomIntensity = baseBloomIntensity;
  pp.bloomThreshold = baseBloomThreshold;
  pp.brightness = baseBrightness;
  pp.enableDistanceTint = baseEnableDistanceTint;
  pp.distanceTintColor = baseDistanceTintColor;
  pp.volumetricFogDensity = baseVolumetricDensity;
  pp.gradeSaturation = baseGradeSaturation;
  pp.gradeContrast = baseGradeContrast;
  pp.gradeLift = baseGradeLift;
  pp.gradeGamma = baseGradeGamma;
  pp.gradeGain = baseGradeGain;
  pp.gradeTint = baseGradeTint;
}
