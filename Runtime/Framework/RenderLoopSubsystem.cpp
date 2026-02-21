#include "RenderLoopSubsystem.h"
#include "AppState.h"
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

bool RenderLoopSubsystem::initialize() { return true; }

void RenderLoopSubsystem::shutdown() {}

void RenderLoopSubsystem::executeRenderPasses(const glm::mat4 &view,
                                              const glm::mat4 &projection,
                                              const glm::vec3 &cameraPos,
                                              const glm::vec3 &cameraFront,
                                              const glm::vec3 &cameraUp,
                                              float renderTime) {
  mState.renderGraph.clear();
  if (!mState.disableShadows) {
    mState.renderGraph.addPass({"ShadowPass", {}, [&]() {
                                  renderShadowPass(mState.sun.sunPos, 1.0f,
                                                   mState.shadowFarPlane);
                                }});
  }
  mState.renderGraph.addPass(
      {"MainPass",
       mState.disableShadows ? std::vector<std::string>{}
                             : std::vector<std::string>{"ShadowPass"},
       [&]() {
         renderMainPass(view, projection, cameraPos, cameraFront, cameraUp,
                        renderTime);
       }});
  (void)mState.renderGraph.execute();
  mState.lastRenderPassOrder = mState.renderGraph.lastExecutionOrder();
}

void RenderLoopSubsystem::renderShadowPass(const glm::vec3 &lightPos,
                                           float nearPlane, float farPlane) {
  glm::mat4 shadowProj =
      glm::perspective(glm::radians(90.0f), 1.0f, nearPlane, farPlane);

  glm::mat4 shadowMats[6] = {
      shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(1, 0, 0),
                               glm::vec3(0, -1, 0)),
      shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(-1, 0, 0),
                               glm::vec3(0, -1, 0)),
      shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(0, 1, 0),
                               glm::vec3(0, 0, 1)),
      shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(0, -1, 0),
                               glm::vec3(0, 0, -1)),
      shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(0, 0, 1),
                               glm::vec3(0, -1, 0)),
      shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(0, 0, -1),
                               glm::vec3(0, -1, 0))};

  mState.renderer.beginShadowPass();

  for (int face = 0; face < 6; ++face) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                           mState.renderer.shadowCubeTex(), 0);

    glClear(GL_DEPTH_BUFFER_BIT);

    Shader &depthSh = mState.renderer.shadowShader();
    depthSh.activate();
    depthSh.setMat4("shadowMatrix", shadowMats[face]);
    depthSh.setVec3("lightPos", lightPos);
    depthSh.setFloat("far_plane", farPlane);

    mState.renderSystem.update(mState.scene.registry(), depthSh, true);
  }

  mState.renderer.endShadowPass();
}

void RenderLoopSubsystem::renderMainPass(const glm::mat4 &view,
                                         const glm::mat4 &projection,
                                         const glm::vec3 &cameraPos,
                                         const glm::vec3 &cameraFront,
                                         const glm::vec3 &cameraUp,
                                         float nowT) {
  glm::vec3 lightPos = mState.sun.sunPos;
  float far_plane = mState.shadowFarPlane;

  glEnable(GL_STENCIL_TEST);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  glStencilFunc(GL_ALWAYS, 1, 0xFF);
  glStencilMask(0xFF);

  mState.renderer.beginFrame(0.2f, 0.3f, 0.3f, 1.0f);
  glStencilMask(0x00);

  if (mState.disableHDR) {
    mState.sky.setSolidSky(true);
    mState.sky.setSkyColors(glm::make_vec3(mState.skyHorizon),
                            glm::make_vec3(mState.skyTop));
  } else {
    mState.sky.setSolidSky(false);
  }

  mState.sky.draw(view, projection, mState.exposure, mState.gamma);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);

  mState.renderer.shader().activate();

  mState.renderer.shader().setInt("texture1", 0);
  mState.renderer.shader().setInt("shadowCube", 1);

  mState.renderer.setFrameUniforms(
      view, projection, mState.mixVal, nowT, mState.sun.sunColor,
      mState.sun.ambientStrength, cameraPos, mState.sun.glowStrength, lightPos,
      far_plane, mState.shadowStrength);

  mState.renderer.shader().setBool("uHasFire", false);

  mState.renderSystem.update(mState.scene.registry(), mState.renderer.shader(),
                             false, mState.selectedEntityId, false);

  mState.projectiles.draw(view, projection, 0.25f);

  mState.renderer.shader().activate();
  mState.sun.draw(mState.renderer.shader(), cameraFront, cameraUp);

  if (mState.selectedEntityId != 0 && mState.outlineShader) {
    glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    glStencilMask(0x00);
    glDisable(GL_DEPTH_TEST);

    mState.outlineShader->activate();
    mState.outlineShader->setMat4("view", view);
    mState.outlineShader->setMat4("projection", projection);
    mState.renderSystem.update(mState.scene.registry(), *mState.outlineShader,
                               false, mState.selectedEntityId, true);

    glStencilMask(0xFF);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glEnable(GL_DEPTH_TEST);
  }
  glDisable(GL_STENCIL_TEST);
}
