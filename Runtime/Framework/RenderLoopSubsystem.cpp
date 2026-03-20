#include "RenderLoopSubsystem.h"
#include "AppState.h"
#include "Texture.h"
#include <cfloat>
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

bool RenderLoopSubsystem::initialize() { return true; }

void RenderLoopSubsystem::shutdown() {
  if (mGpuQueries[0] != 0 || mGpuQueries[1] != 0) {
    glDeleteQueries(2, mGpuQueries);
    mGpuQueries[0] = 0;
    mGpuQueries[1] = 0;
  }
  if (mShadowQueries[0] != 0 || mShadowQueries[1] != 0) {
    glDeleteQueries(2, mShadowQueries);
    mShadowQueries[0] = 0;
    mShadowQueries[1] = 0;
  }
  if (mMainQueries[0] != 0 || mMainQueries[1] != 0) {
    glDeleteQueries(2, mMainQueries);
    mMainQueries[0] = 0;
    mMainQueries[1] = 0;
  }
}

void RenderLoopSubsystem::executeRenderPasses(const glm::mat4 &view,
                                              const glm::mat4 &projection,
                                              const glm::vec3 &cameraPos,
                                              const glm::vec3 &cameraFront,
                                              const glm::vec3 &cameraUp,
                                              float renderTime) {
  ++mShadowFrameIndex;
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
  if (mGpuQueries[0] == 0 && mGpuQueries[1] == 0) {
    glGenQueries(2, mGpuQueries);
  }
  if (mShadowQueries[0] == 0 && mShadowQueries[1] == 0) {
    glGenQueries(2, mShadowQueries);
  }
  if (mMainQueries[0] == 0 && mMainQueries[1] == 0) {
    glGenQueries(2, mMainQueries);
  }

  const int queryIndex = mGpuQueryIndex;
  if (mGpuQueries[queryIndex] != 0)
    glBeginQuery(GL_TIME_ELAPSED, mGpuQueries[queryIndex]);

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

  if (shouldRenderShadowPass) {
    mState.renderGraph.addPass({"ShadowPass", {}, [&]() {
                                  const int qIdx = mShadowQueryIndex;
                                  if (mShadowQueries[qIdx] != 0)
                                    glBeginQuery(GL_TIME_ELAPSED,
                                                 mShadowQueries[qIdx]);
                                  renderShadowPass(
                                      view, projection, cameraPos,
                                      mState.sun.sunDir, 1.0f,
                                      mState.render.shadowFarPlane);
                                  if (mShadowQueries[qIdx] != 0)
                                    glEndQuery(GL_TIME_ELAPSED);
                                }});
  }
  mState.renderGraph.addPass(
      {"MainPass",
       shouldRenderShadowPass ? std::vector<std::string>{"ShadowPass"}
                              : std::vector<std::string>{},
       [&]() {
         const int qIdx = mMainQueryIndex;
         if (mMainQueries[qIdx] != 0)
           glBeginQuery(GL_TIME_ELAPSED, mMainQueries[qIdx]);
         renderMainPass(view, projection, cameraPos, cameraFront, cameraUp,
                        renderTime);
         if (mMainQueries[qIdx] != 0)
           glEndQuery(GL_TIME_ELAPSED);
       }});
  (void)mState.renderGraph.execute();
  mState.lastRenderPassOrder = mState.renderGraph.lastExecutionOrder();

  if (mGpuQueries[queryIndex] != 0)
    glEndQuery(GL_TIME_ELAPSED);

  const int readIndex = (queryIndex + 1) % 2;
  if (mGpuQueries[readIndex] != 0 && mGpuQueryPrimed) {
    GLuint available = 0;
    glGetQueryObjectuiv(mGpuQueries[readIndex], GL_QUERY_RESULT_AVAILABLE,
                        &available);
    if (available) {
      GLuint64 timeNs = 0;
      glGetQueryObjectui64v(mGpuQueries[readIndex], GL_QUERY_RESULT, &timeNs);
      mState.gpuFrameMs = static_cast<float>(timeNs / 1000000.0);
    }
  }
  mGpuQueryPrimed = true;
  mGpuQueryIndex = readIndex;

  const int shadowRead = (mShadowQueryIndex + 1) % 2;
  if (mShadowQueries[shadowRead] != 0 && mShadowQueryPrimed) {
    GLuint available = 0;
    glGetQueryObjectuiv(mShadowQueries[shadowRead],
                        GL_QUERY_RESULT_AVAILABLE, &available);
    if (available) {
      GLuint64 timeNs = 0;
      glGetQueryObjectui64v(mShadowQueries[shadowRead], GL_QUERY_RESULT,
                            &timeNs);
      mState.gpuShadowMs = static_cast<float>(timeNs / 1000000.0);
    }
  }
  mShadowQueryPrimed = true;
  mShadowQueryIndex = shadowRead;

  const int mainRead = (mMainQueryIndex + 1) % 2;
  if (mMainQueries[mainRead] != 0 && mMainQueryPrimed) {
    GLuint available = 0;
    glGetQueryObjectuiv(mMainQueries[mainRead], GL_QUERY_RESULT_AVAILABLE,
                        &available);
    if (available) {
      GLuint64 timeNs = 0;
      glGetQueryObjectui64v(mMainQueries[mainRead], GL_QUERY_RESULT, &timeNs);
      mState.gpuMainMs = static_cast<float>(timeNs / 1000000.0);
    }
  }
  mMainQueryPrimed = true;
  mMainQueryIndex = mainRead;
}

void RenderLoopSubsystem::renderShadowPass(const glm::mat4 &view,
                                           const glm::mat4 &projection,
                                           const glm::vec3 &cameraPos,
                                           const glm::vec3 &sunDirRaw,
                                           float nearPlane, float farPlane) {
  // Fit an orthographic frustum to the camera view for better shadow quality.
  const glm::mat4 invVP = glm::inverse(projection * view);
  glm::vec3 frustumCorners[8];
  int idx = 0;
  for (int z = 0; z < 2; ++z) {
    const float ndcZ = (z == 0) ? -1.0f : 1.0f;
    for (int y = 0; y < 2; ++y) {
      const float ndcY = (y == 0) ? -1.0f : 1.0f;
      for (int x = 0; x < 2; ++x) {
        const float ndcX = (x == 0) ? -1.0f : 1.0f;
        glm::vec4 corner = invVP * glm::vec4(ndcX, ndcY, ndcZ, 1.0f);
        corner /= std::max(corner.w, 0.0001f);
        frustumCorners[idx++] = glm::vec3(corner);
      }
    }
  }

  glm::vec3 frustumCenter(0.0f);
  for (const glm::vec3 &c : frustumCorners)
    frustumCenter += c;
  frustumCenter /= 8.0f;

  glm::vec3 sunDir = glm::normalize(sunDirRaw);
  glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
  if (std::abs(glm::dot(sunDir, up)) > 0.999f)
    up = glm::vec3(0.0f, 0.0f, 1.0f);

  glm::vec3 lightViewPos =
      frustumCenter - sunDir * (farPlane * 0.5f);
  glm::mat4 shadowView = glm::lookAt(lightViewPos, frustumCenter, up);

  glm::vec3 minL(FLT_MAX);
  glm::vec3 maxL(-FLT_MAX);
  for (const glm::vec3 &corner : frustumCorners) {
    glm::vec4 lightSpace = shadowView * glm::vec4(corner, 1.0f);
    minL = glm::min(minL, glm::vec3(lightSpace));
    maxL = glm::max(maxL, glm::vec3(lightSpace));
  }

  const float pad = 10.0f;
  minL.x -= pad;
  minL.y -= pad;
  maxL.x += pad;
  maxL.y += pad;

  float minZ = minL.z - pad;
  float maxZ = maxL.z + pad;
  float lightNear = std::max(0.1f, -maxZ);
  float lightFar = std::max(lightNear + 1.0f, -minZ);

  glm::mat4 shadowProj =
      glm::ortho(minL.x, maxL.x, minL.y, maxL.y, lightNear, lightFar);
  mState.render.lightSpaceMatrix = shadowProj * shadowView;

  mState.renderer.beginShadowPass();

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                         mState.renderer.shadowTex(), 0);

  glClear(GL_DEPTH_BUFFER_BIT);

  Shader &depthSh = mState.renderer.shadowShader();
  depthSh.activate();
  depthSh.setMat4("uLightSpaceMatrix", mState.render.lightSpaceMatrix);

  mState.renderSystem.update(mState.scene.registry(), depthSh, true, 0, false,
                             false, RenderSystem::TerrainFilter::All);

  mState.renderer.endShadowPass();

  mShadowValid = true;
  mLastShadowCamPos = cameraPos;
  mLastShadowSunDir = sunDir;
}

void RenderLoopSubsystem::renderMainPass(const glm::mat4 &view,
                                         const glm::mat4 &projection,
                                         const glm::vec3 &cameraPos,
                                         const glm::vec3 &cameraFront,
                                         const glm::vec3 &cameraUp,
                                         float nowT) {
  glm::vec3 lightDir = mState.sun.sunDir;
  float far_plane = mState.render.shadowFarPlane;

  glm::vec3 skyHorizon = glm::make_vec3(mState.skyUI.skyHorizon);
  glm::vec3 skyTop = glm::make_vec3(mState.skyUI.skyTop);
  glm::vec3 sunColor = mState.sun.sunColor;
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

    // Slightly tint the horizon at dusk for nicer gradients.
    skyHorizon = glm::mix(skyHorizon, sunColor, duskFactor * 0.25f);

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

  const bool torchLightActive =
      mState.torchEnabled &&
      mState.activeSlot == AppState::HotbarSlot::Torch &&
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

  // Replace beginFrame with PostProcessor integration
  mState.postProcessor.resize(mState.scrW, mState.scrH);
  mState.postProcessor.beginRenderPass();

  // Clear depth and set clear color (since PostProcessor clear doesn't set
  // color)
  glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  glStencilMask(0x00);

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
  const float baseDisc = mState.sky.sunDiscIntensity;
  const float baseHalo = mState.sky.sunHaloIntensity;
  const float baseRays = mState.sky.sunRaysIntensity;
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
  mState.sky.draw(view, projection, skyExposure, skyGamma, mState.sun.sunDir,
                  litSunColor, mState.sun.sunSize, nowT);
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
                      litSunIntensity);
  }
  mState.cloud.alpha = baseCloudAlpha;
  mState.cloud.color = baseCloudColor;

  mState.renderer.shader().activate();

  mState.renderer.shader().setInt("texture1", 0);
  mState.renderer.shader().setInt("shadowCube", 1);

  mState.renderer.setFrameUniforms(
      view, projection, mState.render.mixVal, nowT, litSunColor, litAmbient,
      cameraPos, litSunIntensity, lightDir, far_plane,
      mState.render.shadowStrength, fogColor, fogDensity,
      mState.render.fogHeightFalloff,
      mState.render.toonEnabled, mState.render.toonSteps, mState.render.toonMin,
      mState.render.shadowBandEnabled, mState.render.shadowBandSteps,
      mState.render.shadowBandSoftness, mState.render.ambientRampEnabled,
      ambientRampStrength, ambientRampTop, ambientRampBottom,
      mState.render.rimEnabled, mState.render.rimPower, mState.render.rimStrength,
      mState.render.rimColor, emissiveBoost, emissiveFlicker);

  mState.renderer.shader().setMat4("uLightSpaceMatrix",
                                   mState.render.lightSpaceMatrix);
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
  const GLuint terrainGroundAlbedo =
      tm.useGroundTextures ? LoadTexture2DCached(tm.groundAlbedoPath) : 0;
  const GLuint terrainGroundRoughness =
      tm.useGroundTextures ? LoadTexture2DCached(tm.groundRoughnessPath) : 0;
  mState.renderer.shader().setBool(
      "uTerrainUseGroundTextures",
      tm.useGroundTextures && terrainGroundAlbedo != 0);
  mState.renderer.shader().setBool("uTerrainHasGroundRoughness",
                                   terrainGroundRoughness != 0);
  mState.renderer.shader().setFloat("uTerrainGroundTiling", tm.groundTiling);
  mState.renderer.shader().setFloat("uTerrainGroundBlendStrength",
                                    tm.groundBlendStrength);
  mState.renderer.shader().setFloat("uTerrainGroundRoughnessValue",
                                    tm.groundRoughness);
  mState.renderer.shader().setInt("uTerrainGroundAlbedo", 8);
  mState.renderer.shader().setInt("uTerrainGroundRoughness", 9);
  glActiveTexture(GL_TEXTURE8);
  glBindTexture(GL_TEXTURE_2D, terrainGroundAlbedo);
  glActiveTexture(GL_TEXTURE9);
  glBindTexture(GL_TEXTURE_2D, terrainGroundRoughness);
  glActiveTexture(GL_TEXTURE0);
  mState.renderer.shader().setBool("uTerrainFlatGreenEnabled",
                                   tm.flatGreenEnabled);
  mState.renderer.shader().setVec3("uTerrainFlatGreenColor", tm.flatGreenColor);

  const bool useFlatTerrainShader = tm.flatGreenEnabled &&
                                    mState.terrainFlatShader != nullptr;
  if (useFlatTerrainShader) {
    Shader &terrainSh = *mState.terrainFlatShader;
    terrainSh.activate();
    terrainSh.setMat4("view", view);
    terrainSh.setMat4("projection", projection);
    terrainSh.setVec3("uSunColor", litSunColor);
    terrainSh.setFloat("uSunIntensity", litSunIntensity);
    terrainSh.setFloat("uAmbient", litAmbient);
    terrainSh.setVec3("uLightDir", lightDir);
    terrainSh.setMat4("uLightSpaceMatrix", mState.render.lightSpaceMatrix);
    terrainSh.setFloat("uShadowStrength", mState.render.shadowStrength);
    terrainSh.setVec3("uCameraPos", cameraPos);
    terrainSh.setVec3("uFogColor", fogColor);
    terrainSh.setFloat("uFogDensity", fogDensity);
    terrainSh.setFloat("uFogHeightFalloff", mState.render.fogHeightFalloff);
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

  mState.renderSystem.update(
      mState.scene.registry(), mState.renderer.shader(), false,
      mState.selection.selectedEntityId, false, false,
      useFlatTerrainShader ? RenderSystem::TerrainFilter::ExcludeTerrain
                           : RenderSystem::TerrainFilter::All);

  if (fireflyFactor > 0.0f) {
    mState.fireflies.draw(view, projection, nowT, fireflyFactor,
                          mState.skyUI.fireflySize,
                          mState.skyUI.fireflyIntensity,
                          mState.skyUI.fireflyColor);
  }

  mState.projectiles.draw(view, projection, 0.25f);

  if (mState.selection.selectedEntityId != 0 && mState.outlineShader) {
    glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    glStencilMask(0x00);

    mState.outlineShader->activate();
    mState.outlineShader->setMat4("view", view);
    mState.outlineShader->setMat4("projection", projection);
    mState.renderSystem.update(mState.scene.registry(), *mState.outlineShader,
                               false, mState.selection.selectedEntityId, true,
                               false, RenderSystem::TerrainFilter::All);

    glStencilMask(0xFF);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
  }
  glDisable(GL_STENCIL_TEST);

  // Viewmodel pass — screen-space, no camera correlation.
  const bool drawViewmodelAxe =
      mState.axeEnabled &&
      mState.activeSlot == AppState::HotbarSlot::Axe &&
      mState.axeEntity != 0;
  const bool drawViewmodelTorch =
      mState.torchEnabled &&
      mState.activeSlot == AppState::HotbarSlot::Torch &&
      mState.torchEntity != 0;
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
        mState.render.shadowStrength, fogColor, fogDensity,
        mState.render.fogHeightFalloff,
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
        mState.scene.registry().has<TransformComponent>(mState.torchEntity)) {
      const auto &torchTr =
          mState.scene.registry().get<TransformComponent>(mState.torchEntity);
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
    mState.physicsSystem.drawDebugColliders(
        mState.scene.registry(), view, projection, mState.renderer.shader());
  }

  // Finish post-processing (bloom + SSAO + volumetrics) and blit to screen.
  auto &pp = mState.postProcessor;
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

  pp.endRenderPass(view, projection, cameraPos, lightDir, litSunColor, 0.1f,
                   500.0f, nowT);

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
