#include "Renderer.h"
#include "EngineAssert.h"
#include "GLStateCache.h"
#include "Logger.h"
#include "Shader.h"
#include <GLFW/glfw3.h> // Needed for glfwExtensionSupported check if you add Anisotropy
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

namespace {
RendererCapabilities detectRendererCapabilities() {
  RendererCapabilities caps{};

  GLint major = 0;
  GLint minor = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);
  caps.glMajor = major;
  caps.glMinor = minor;

  caps.supportsOpenGL43 =
      (major > 4) || (major == 4 && minor >= 3) || GLAD_GL_VERSION_4_3;
#ifdef GLAD_GL_ARB_multi_draw_indirect
  caps.supportsMultiDrawIndirect =
      caps.supportsOpenGL43 || GLAD_GL_ARB_multi_draw_indirect != 0;
#else
  caps.supportsMultiDrawIndirect = caps.supportsOpenGL43;
#endif
  caps.supportsMapBufferRange = (glMapBufferRange != nullptr);
#if defined(GLAD_GL_VERSION_4_4) || defined(GLAD_GL_ARB_buffer_storage)
  caps.supportsBufferStorage =
      (GLAD_GL_VERSION_4_4 != 0) || (GLAD_GL_ARB_buffer_storage != 0);
#else
  caps.supportsBufferStorage = false;
#endif

  caps.preferredSubmissionBackend =
      (caps.supportsOpenGL43 && caps.supportsMultiDrawIndirect &&
       caps.supportsMapBufferRange)
          ? RenderSubmissionBackend::Modern
          : RenderSubmissionBackend::Direct;

  return caps;
}
} // namespace

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

  mCapabilities = detectRendererCapabilities();

  mShader = std::make_unique<Shader>(vertexPath, fragmentPath);
  if (!mShader || !mShader->isValid()) {
    LOG_ERROR("Render",
              "Renderer init failed: main shader program could not be created");
    mShader.reset();
    return false;
  }
  mShader->activate();

  mShader->setFloat("uGamma", 2.2f);
  mShader->setFloat("uSpecStrength", 0.5f); // Bumped up slightly
  mShader->setFloat("uShininess", 32.0f); // Lower shininess = larger highlights

  mShader->setInt("texture1", 0);
  mShader->setInt("shadowMap", 1);
  mShader->setInt("uEnvMap", 2);
  mShader->setInt("uShadowMapArray", 16);
  mShader->setFloat("uSunIntensity", 1.0f);
  mShader->setFloat("uShadowStrength", 1.5f);
  mShader->setFloat("uSceneExposure", 1.0f);

  if (!initShadowResources_(shadowVertPath, shadowFragPath, shadowMapRes))
    LOG_WARN("Render",
             "Shadow resources init failed. Continuing without shadows.");

  return true;
}

bool Renderer::initShadowResources_(const char *shadowVertPath,
                                    const char *shadowFragPath,
                                    int shadowMapRes) {
  mShadowShader = std::make_unique<Shader>(shadowVertPath, shadowFragPath);
  if (!mShadowShader || !mShadowShader->isValid()) {
    LOG_ERROR("Render",
              "Shadow resources init failed: shadow shader program could not be created");
    shutdownShadowResources_();
    return false;
  }
  return ensureShadowResources(shadowMapRes, 1);
}

bool Renderer::allocateShadowTextures_(int shadowMapRes, int cascadeLayers) {
  shadowMapRes = std::clamp(shadowMapRes, 512, 8192);
  cascadeLayers = std::clamp(cascadeLayers, 1, 4);

  releaseShadowTextures_();

  mShadowRes = shadowMapRes;
  mShadowLayers = cascadeLayers;

  glGenTextures(1, &mShadowTex);
  glBindTexture(GL_TEXTURE_2D, mShadowTex);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, mShadowRes, mShadowRes, 0,
               GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);

  // VISUAL UPGRADE: Use GL_LINEAR for PCF (soft shadows) in shader
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

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
    releaseShadowTextures_();
    return false;
  }

  if (mShadowLayers <= 1)
    return true;

  glGenTextures(1, &mShadowArrayTex);
  glBindTexture(GL_TEXTURE_2D_ARRAY, mShadowArrayTex);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT32F, mShadowRes,
               mShadowRes, mShadowLayers, 0, GL_DEPTH_COMPONENT, GL_FLOAT,
               nullptr);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

  glBindFramebuffer(GL_FRAMEBUFFER, mShadowFBO);
  glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            mShadowArrayTex, 0, 0);
  status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    LOG_ERROR("Render", "Shadow array FBO incomplete: status=" +
                            std::to_string((int)status));
    releaseShadowTextures_();
    return false;
  }

  return true;
}

bool Renderer::ensureShadowResources(int shadowMapRes, int cascadeLayers) {
  shadowMapRes = std::clamp(shadowMapRes, 512, 8192);
  cascadeLayers = std::clamp(cascadeLayers, 1, 4);
  if (!mShadowShader)
    return false;
  const bool hasRequestedTextures =
      cascadeLayers <= 1 ? (mShadowFBO && mShadowTex)
                         : (mShadowFBO && mShadowTex && mShadowArrayTex);
  if (hasRequestedTextures && mShadowRes == shadowMapRes &&
      mShadowLayers == cascadeLayers) {
    return true;
  }
  return allocateShadowTextures_(shadowMapRes, cascadeLayers);
}

void Renderer::releaseShadowTextures_() {
  if (mShadowTex)
    glDeleteTextures(1, &mShadowTex);
  if (mShadowArrayTex)
    glDeleteTextures(1, &mShadowArrayTex);
  if (mShadowFBO)
    glDeleteFramebuffers(1, &mShadowFBO);

  mShadowTex = 0;
  mShadowArrayTex = 0;
  mShadowFBO = 0;
  mShadowLayers = 1;
}

void Renderer::shutdownShadowResources_() {
  releaseShadowTextures_();
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
                                float shadowStrength, float sceneExposure,
                                float gamma, const glm::vec3 &fogColor,
                                float fogDensity, float fogHeightFalloff,
                                bool ambientHemisphereEnabled,
                                float ambientHemisphereIntensity,
                                float ambientHorizonStrength,
                                float ambientTerrainBoost,
                                const glm::vec3 &ambientSkyColor,
                                const glm::vec3 &ambientHorizonColor,
                                const glm::vec3 &ambientGroundColor,
                                bool toonEnabled, int toonSteps, float toonMin,
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
  mShader->setMat4("uViewMatrix", view);
  mShader->setInt("shadowMap", 1);
  mShader->setInt("uEnvMap", 2);
  mShader->setInt("uShadowMapArray", 16);
  mShader->setFloat("uTime", timeSec);
  mShader->setFloat("uMixVal", mixVal);

  mShader->setVec3("uSunColor", sunColor);
  mShader->setFloat("uSunIntensity", sunIntensity);
  mShader->setFloat("uAmbient", ambientStrength);
  mShader->setVec3("uCameraPos", cameraPos);

  mShader->setVec3("uLightDir", lightDir);
  mShader->setFloat("uFarPlane", farPlane);
  mShader->setFloat("uShadowStrength", shadowStrength);
  mShader->setFloat("uSceneExposure", sceneExposure);
  mShader->setFloat("uGamma", gamma);

  // Fog
  mShader->setVec3("uFogColor", fogColor);
  mShader->setFloat("uFogDensity", fogDensity);
  mShader->setFloat("uFogHeightFalloff", fogHeightFalloff);

  // Environment ambient
  mShader->setBool("uAmbientHemiEnabled", ambientHemisphereEnabled);
  mShader->setFloat("uAmbientHemiIntensity", ambientHemisphereIntensity);
  mShader->setFloat("uAmbientHorizonStrength", ambientHorizonStrength);
  mShader->setFloat("uAmbientTerrainBoost", ambientTerrainBoost);
  mShader->setVec3("uAmbientSkyColor", ambientSkyColor);
  mShader->setVec3("uAmbientHorizonColor", ambientHorizonColor);
  mShader->setVec3("uAmbientGroundColor", ambientGroundColor);

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
  glActiveTexture(GL_TEXTURE16);
  glBindTexture(GL_TEXTURE_2D_ARRAY, mShadowArrayTex);
  glActiveTexture(GL_TEXTURE0);
}

void Renderer::beginShadowPass(int cascadeLayer) {
  ENGINE_ASSERT(mShader != nullptr,
                "Renderer::beginShadowPass called before init");
  if (!mShadowFBO || !mShadowTex || !mShadowShader)
    return;

  glGetIntegerv(GL_VIEWPORT, mPrevViewport);
  glViewport(0, 0, mShadowRes, mShadowRes);

  glBindFramebuffer(GL_FRAMEBUFFER, mShadowFBO);
  if (cascadeLayer >= 0 && mShadowArrayTex) {
    const int layer = std::clamp(cascadeLayer, 0, std::max(0, mShadowLayers - 1));
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              mShadowArrayTex, 0, layer);
  } else {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           mShadowTex, 0);
  }
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
