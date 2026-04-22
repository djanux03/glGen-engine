#pragma once
#include "RenderCapabilities.h"
#include <glad/glad.h>
#include <glm/glm.hpp>

#include <memory>

class Shader;

class Renderer {
public:
  Renderer();
  ~Renderer();

  // Kept for compatibility with your existing calls; side/top/bottom are now
  // unused.
  bool init(const char *vertexPath, const char *fragmentPath,
            const char *sidePath, const char *topPath, const char *bottomPath,
            const char *shadowVertPath, const char *shadowFragPath,
            int shadowMapRes = 2048);

  bool initWithShadows(const char *vertexPath, const char *fragmentPath,
                       const char *sidePath, const char *topPath,
                       const char *bottomPath, const char *shadowVertPath,
                       const char *shadowFragPath, int shadowMapRes = 2048);

  void shutdown();

  void beginFrame(float r, float g, float b, float a);

  void setFrameUniforms(const glm::mat4 &view, const glm::mat4 &projection,
                        float mixVal, float timeSec, const glm::vec3 &sunColor,
                        float ambientStrength, const glm::vec3 &cameraPos,
                        float sunIntensity, const glm::vec3 &lightDir,
                        float far_plane, float shadowStrength,
                        float sceneExposure, float gamma,
                        const glm::vec3 &fogColor, float fogDensity,
                        float fogHeightFalloff,
                        bool ambientHemisphereEnabled,
                        float ambientHemisphereIntensity,
                        float ambientHorizonStrength,
                        float ambientTerrainBoost,
                        const glm::vec3 &ambientSkyColor,
                        const glm::vec3 &ambientHorizonColor,
                        const glm::vec3 &ambientGroundColor,
                        bool toonEnabled, int toonSteps, float toonMin,
                        bool shadowBandEnabled, int shadowBandSteps,
                        float shadowBandSoftness, bool ambientRampEnabled,
                        float ambientRampStrength,
                        const glm::vec3 &ambientRampTop,
                        const glm::vec3 &ambientRampBottom,
                        bool rimEnabled, float rimPower, float rimStrength,
                        const glm::vec3 &rimColor, float emissiveBoost,
                        float emissiveFlicker);

  // Shadow pass (depth cubemap)
  bool ensureShadowResources(int shadowMapRes, int cascadeLayers = 1);
  void beginShadowPass(int cascadeLayer = -1);
  void endShadowPass();

  Shader &shader();
  Shader &shadowShader();
  const RendererCapabilities &capabilities() const { return mCapabilities; }

  // Needed in App.cpp for the 6-face loop
  GLuint shadowFBO() const { return mShadowFBO; }
  GLuint shadowTex() const { return mShadowTex; }
  GLuint shadowArrayTex() const { return mShadowArrayTex; }
  int shadowRes() const { return mShadowRes; }
  int shadowLayers() const { return mShadowLayers; }

private:
  bool initShadowResources_(const char *shadowVertPath,
                            const char *shadowFragPath, int shadowMapRes);
  bool allocateShadowTextures_(int shadowMapRes, int cascadeLayers);

  void releaseShadowTextures_();
  void shutdownShadowResources_();

private:
  std::unique_ptr<Shader> mShader;

  std::unique_ptr<Shader> mShadowShader;
  GLuint mShadowTex = 0;
  GLuint mShadowArrayTex = 0;
  GLuint mShadowFBO = 0;
  int mShadowRes = 2048;
  int mShadowLayers = 1;
  GLint mPrevViewport[4] = {0, 0, 0, 0};
  RendererCapabilities mCapabilities{};
};
