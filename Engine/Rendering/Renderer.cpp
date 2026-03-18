#include "Renderer.h"
#include "EngineAssert.h"
#include "GLStateCache.h"
#include "Logger.h"
#include "Shader.h"
#include <GLFW/glfw3.h> // Needed for glfwExtensionSupported check if you add Anisotropy
#include <glm/gtc/matrix_transform.hpp>

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::init(const char *vertexPath, const char *fragmentPath,
                    const char *sidePath, const char *topPath,
                    const char *bottomPath, const char *shadowVertPath,
                    const char *shadowFragPath, int shadowMapRes) {
  (void)sidePath;
  (void)topPath;
  (void)bottomPath;

  return initWithShadows(vertexPath, fragmentPath, sidePath, topPath,
                         bottomPath, shadowVertPath, shadowFragPath,
                         shadowMapRes);
}

bool Renderer::initWithShadows(const char *vertexPath, const char *fragmentPath,
                               const char *sidePath, const char *topPath,
                               const char *bottomPath,
                               const char *shadowVertPath,
                               const char *shadowFragPath, int shadowMapRes) {
  (void)sidePath;
  (void)topPath;
  (void)bottomPath;

  mShader = std::make_unique<Shader>(vertexPath, fragmentPath);
  mShader->activate();

  mShader->setFloat("uGamma", 2.2f);
  mShader->setFloat("uSpecStrength", 0.5f); // Bumped up slightly
  mShader->setFloat("uShininess", 32.0f); // Lower shininess = larger highlights

  mShader->setInt("texture1", 0);
  mShader->setInt("shadowMap", 1);
  mShader->setFloat("uSunIntensity", 1.0f);
  mShader->setFloat("uShadowStrength", 1.5f);

  if (!initShadowResources_(shadowVertPath, shadowFragPath, shadowMapRes))
    LOG_WARN("Render",
             "Shadow resources init failed. Continuing without shadows.");

  return true;
}

bool Renderer::initShadowResources_(const char *shadowVertPath,
                                    const char *shadowFragPath,
                                    int shadowMapRes) {
  mShadowRes = shadowMapRes;

  glGenTextures(1, &mShadowTex);
  glBindTexture(GL_TEXTURE_2D, mShadowTex);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, mShadowRes, mShadowRes, 0,
               GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);

  // VISUAL UPGRADE: Use GL_LINEAR for PCF (soft shadows) in shader
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
  float borderColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
  glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

  glBindTexture(GL_TEXTURE_2D, 0);

  glGenFramebuffers(1, &mShadowFBO);
  glBindFramebuffer(GL_FRAMEBUFFER, mShadowFBO);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                         mShadowTex, 0);
  glDrawBuffer(GL_NONE);
  glReadBuffer(GL_NONE);

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  if (status != GL_FRAMEBUFFER_COMPLETE) {
    LOG_ERROR("Render",
              "Shadow FBO incomplete: status=" + std::to_string((int)status));
    shutdownShadowResources_();
    return false;
  }

  mShadowShader = std::make_unique<Shader>(shadowVertPath, shadowFragPath);
  return true;
}

void Renderer::shutdownShadowResources_() {
  if (mShadowTex)
    glDeleteTextures(1, &mShadowTex);
  if (mShadowFBO)
    glDeleteFramebuffers(1, &mShadowFBO);

  mShadowTex = 0;
  mShadowFBO = 0;
  mShadowShader.reset();
}

void Renderer::shutdown() {
  shutdownShadowResources_();
  mShader.reset();
}

void Renderer::beginFrame(float r, float g, float b, float a) {
  glClearColor(r, g, b, a);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void Renderer::setFrameUniforms(const glm::mat4 &view,
                                const glm::mat4 &projection, float mixVal,
                                float timeSec, const glm::vec3 &sunColor,
                                float ambientStrength,
                                const glm::vec3 &cameraPos, float sunIntensity,
                                const glm::vec3 &lightDir, float farPlane,
                                float shadowStrength, const glm::vec3 &fogColor,
                                float fogDensity, bool toonEnabled,
                                int toonSteps, float toonMin,
                                bool shadowBandEnabled, int shadowBandSteps,
                                float shadowBandSoftness,
                                bool ambientRampEnabled,
                                float ambientRampStrength,
                                const glm::vec3 &ambientRampTop,
                                const glm::vec3 &ambientRampBottom,
                                bool rimEnabled,
                                float rimPower, float rimStrength,
                                const glm::vec3 &rimColor,
                                float emissiveBoost, float emissiveFlicker) {
  ENGINE_ASSERT(mShader != nullptr,
                "Renderer::setFrameUniforms called before shader init");
  mShader->activate();

  mShader->setMat4("view", view);
  mShader->setMat4("projection", projection);
  mShader->setFloat("uTime", timeSec);
  mShader->setFloat("uMixVal", mixVal);

  mShader->setVec3("uSunColor", sunColor);
  mShader->setFloat("uSunIntensity", sunIntensity);
  mShader->setFloat("uAmbient", ambientStrength);
  mShader->setVec3("uCameraPos", cameraPos);

  mShader->setVec3("uLightDir", lightDir);
  mShader->setFloat("uFarPlane", farPlane);
  mShader->setFloat("uShadowStrength", shadowStrength);

  // Fog
  mShader->setVec3("uFogColor", fogColor);
  mShader->setFloat("uFogDensity", fogDensity);

  // Toon lighting
  mShader->setBool("uToonEnabled", toonEnabled);
  mShader->setInt("uToonSteps", toonSteps);
  mShader->setFloat("uToonMin", toonMin);

  // Shadow bands
  mShader->setBool("uShadowBandEnabled", shadowBandEnabled);
  mShader->setInt("uShadowBandSteps", shadowBandSteps);
  mShader->setFloat("uShadowBandSoftness", shadowBandSoftness);

  // Ambient ramp
  mShader->setBool("uAmbientRampEnabled", ambientRampEnabled);
  mShader->setFloat("uAmbientRampStrength", ambientRampStrength);
  mShader->setVec3("uAmbientRampTop", ambientRampTop);
  mShader->setVec3("uAmbientRampBottom", ambientRampBottom);

  // Rim lighting
  mShader->setBool("uRimEnabled", rimEnabled);
  mShader->setFloat("uRimPower", rimPower);
  mShader->setFloat("uRimStrength", rimStrength);
  mShader->setVec3("uRimColor", rimColor);

  // Emissive boost/flicker (night look)
  mShader->setFloat("uEmissiveBoost", emissiveBoost);
  mShader->setFloat("uEmissiveFlicker", emissiveFlicker);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, mShadowTex);
  glActiveTexture(GL_TEXTURE0);
}

void Renderer::beginShadowPass() {
  ENGINE_ASSERT(mShader != nullptr,
                "Renderer::beginShadowPass called before init");
  if (!mShadowFBO || !mShadowTex || !mShadowShader)
    return;

  glGetIntegerv(GL_VIEWPORT, mPrevViewport);
  glViewport(0, 0, mShadowRes, mShadowRes);

  glBindFramebuffer(GL_FRAMEBUFFER, mShadowFBO);
  glClear(GL_DEPTH_BUFFER_BIT);

  // VISUAL UPGRADE: Cull Front Faces
  // This solves "peter panning" (floating shadows) better than PolygonOffset
  // usually does. It renders the BACK of the objects into the shadow map.
  glEnable(GL_CULL_FACE);
  GLStateCache::instance().setCullFace(GL_FRONT);
}

void Renderer::endShadowPass() {
  if (!mShadowFBO)
    return;

  // Restore standard rendering state
  GLStateCache::instance().setCullFace(GL_BACK);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(mPrevViewport[0], mPrevViewport[1], mPrevViewport[2],
             mPrevViewport[3]);
}

Shader &Renderer::shader() { return *mShader; }
Shader &Renderer::shadowShader() { return *mShadowShader; }
