#include "RenderLoopSubsystem.h"
#include "AppState.h"
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
  if (!mState.render.disableShadows) {
    mState.renderGraph.addPass({"ShadowPass", {}, [&]() {
                                  const int qIdx = mShadowQueryIndex;
                                  if (mShadowQueries[qIdx] != 0)
                                    glBeginQuery(GL_TIME_ELAPSED,
                                                 mShadowQueries[qIdx]);
                                  renderShadowPass(
                                      cameraPos, mState.sun.sunDir, 1.0f,
                                      mState.render.shadowFarPlane);
                                  if (mShadowQueries[qIdx] != 0)
                                    glEndQuery(GL_TIME_ELAPSED);
                                }});
  }
  mState.renderGraph.addPass(
      {"MainPass",
       mState.render.disableShadows ? std::vector<std::string>{}
                                    : std::vector<std::string>{"ShadowPass"},
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

void RenderLoopSubsystem::renderShadowPass(const glm::vec3 &cameraPos,
                                           const glm::vec3 &sunDirRaw,
                                           float nearPlane, float farPlane) {
  // Use Orthographic projection for directional light (Sun)
  float orthoSize = 100.0f;
  glm::mat4 shadowProj = glm::ortho(-orthoSize, orthoSize, -orthoSize,
                                    orthoSize, nearPlane, farPlane);

  // Position the light camera to look exactly along the sun's direction towards
  // the player/center.
  glm::vec3 sunDir = glm::normalize(sunDirRaw);
  glm::vec3 target = cameraPos;
  glm::vec3 lightViewPos = target - sunDir * (farPlane * 0.5f);

  // Avoid Gimbal Lock if looking straight up/down
  glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
  if (std::abs(glm::dot(sunDir, up)) > 0.999f) {
    up = glm::vec3(0.0f, 0.0f, 1.0f);
  }

  glm::mat4 shadowView = glm::lookAt(lightViewPos, target, up);
  mState.render.lightSpaceMatrix = shadowProj * shadowView;

  mState.renderer.beginShadowPass();

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                         mState.renderer.shadowTex(), 0);

  glClear(GL_DEPTH_BUFFER_BIT);

  Shader &depthSh = mState.renderer.shadowShader();
  depthSh.activate();
  depthSh.setMat4("uLightSpaceMatrix", mState.render.lightSpaceMatrix);

  mState.renderSystem.update(mState.scene.registry(), depthSh, true);

  mState.renderer.endShadowPass();
}

void RenderLoopSubsystem::renderMainPass(const glm::mat4 &view,
                                         const glm::mat4 &projection,
                                         const glm::vec3 &cameraPos,
                                         const glm::vec3 &cameraFront,
                                         const glm::vec3 &cameraUp,
                                         float nowT) {
  glm::vec3 lightDir = mState.sun.sunDir;
  float far_plane = mState.render.shadowFarPlane;

  // Elevation-driven lighting model for more natural day/dusk look.
  float daylight = glm::clamp(-lightDir.y, 0.0f, 1.0f);
  float dayBlend = glm::smoothstep(0.03f, 0.30f, daylight);
  glm::vec3 sunriseTint(1.20f, 0.70f, 0.45f);
  glm::vec3 litSunColor =
      mState.sun.sunColor * glm::mix(sunriseTint, glm::vec3(1.0f), dayBlend);
  float litSunIntensity =
      mState.sun.glowStrength * glm::mix(0.16f, 1.0f, daylight);
  float litAmbient =
      glm::max(0.02f, mState.sun.ambientStrength * (0.25f + 0.75f * daylight));

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
    mState.sky.setSkyColors(glm::make_vec3(mState.skyUI.skyHorizon),
                            glm::make_vec3(mState.skyUI.skyTop));
  } else {
    mState.sky.setSolidSky(false);
  }

  bool o_skyCloudsEnabled = mState.sky.skyCloudsEnabled;
  if (mState.render.disableClouds) {
    mState.sky.skyCloudsEnabled = false;
  }

  mState.sky.draw(view, projection, mState.render.exposure, mState.render.gamma,
                  mState.sun.sunDir, litSunColor, mState.sun.sunSize, nowT);

  mState.sky.skyCloudsEnabled = o_skyCloudsEnabled;

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);

  if (!mState.render.disableClouds) {
    mState.renderer.shader().activate();
    mState.cloud.draw(mState.renderer.shader(), cameraPos);
  }

  mState.renderer.shader().activate();

  mState.renderer.shader().setInt("texture1", 0);
  mState.renderer.shader().setInt("shadowCube", 1);

  mState.renderer.setFrameUniforms(
      view, projection, mState.render.mixVal, nowT, litSunColor, litAmbient,
      cameraPos, litSunIntensity, lightDir, far_plane,
      mState.render.shadowStrength, mState.render.fogColor,
      mState.render.fogDensity);

  mState.renderer.shader().setMat4("uLightSpaceMatrix",
                                   mState.render.lightSpaceMatrix);
  mState.renderer.shader().setBool("uHasFire", false);

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
  mState.renderer.shader().setBool("uTerrainFlatGreenEnabled",
                                   tm.flatGreenEnabled);
  mState.renderer.shader().setVec3("uTerrainFlatGreenColor", tm.flatGreenColor);

  mState.renderSystem.update(mState.scene.registry(), mState.renderer.shader(),
                             false, mState.selection.selectedEntityId, false);

  mState.projectiles.draw(view, projection, 0.25f);

  if (mState.selection.selectedEntityId != 0 && mState.outlineShader) {
    glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    glStencilMask(0x00);

    mState.outlineShader->activate();
    mState.outlineShader->setMat4("view", view);
    mState.outlineShader->setMat4("projection", projection);
    mState.renderSystem.update(mState.scene.registry(), *mState.outlineShader,
                               false, mState.selection.selectedEntityId, true);

    glStencilMask(0xFF);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
  }
  glDisable(GL_STENCIL_TEST);

  if (mState.playState != AppState::PlayState::Playing) {
    mState.physicsSystem.drawDebugColliders(
        mState.scene.registry(), view, projection, mState.renderer.shader());
  }

  // Finish post-processing (bloom + SSAO + volumetrics) and blit to screen.
  mState.postProcessor.endRenderPass(view, projection, cameraPos, lightDir,
                                     litSunColor, 0.1f, 500.0f, nowT);
}
