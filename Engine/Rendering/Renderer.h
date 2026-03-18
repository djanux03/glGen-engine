#pragma once
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
                        const glm::vec3 &fogColor, float fogDensity,
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
  void beginShadowPass();
  void endShadowPass();

  Shader &shader();
  Shader &shadowShader();

  // Needed in App.cpp for the 6-face loop
  GLuint shadowFBO() const { return mShadowFBO; }
  GLuint shadowTex() const { return mShadowTex; }
  int shadowRes() const { return mShadowRes; }

private:
  bool initShadowResources_(const char *shadowVertPath,
                            const char *shadowFragPath, int shadowMapRes);

  void shutdownShadowResources_();

private:
  std::unique_ptr<Shader> mShader;

  std::unique_ptr<Shader> mShadowShader;
  GLuint mShadowTex = 0;
  GLuint mShadowFBO = 0;
  int mShadowRes = 2048;
  GLint mPrevViewport[4] = {0, 0, 0, 0};
};
