#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>

#include <memory>
#include <string>

class Shader;

enum class BlackHoleQuality {
  Performance = 0,
  High = 1,
  Ultra = 2,
};

struct BlackHoleSettings {
  bool enabled = false;
  glm::vec3 direction = glm::normalize(glm::vec3(-0.60f, 0.47f, -0.60f));
  float viewPitchDeg = 0.0f;
  float eventHorizonSizeDeg = 6.0f;
  float diskTiltDeg = -32.0f;
  float diskInclinationDeg = 76.0f;
  float innerDiskRadius = 1.25f;
  float outerDiskRadius = 6.8f;
  float diskTemperature = 1.12f;
  float diskDensity = 1.15f;
  float diskTurbulence = 0.78f;
  float diskSpinSpeed = 5.40f;
  float diskFlowShear = 0.22f;
  float dopplerStrength = 0.82f;
  float lensingStrength = 0.90f;
  float photonRingIntensity = 2.6f;
  float ringWidth = 0.18f;
  float coronaIntensity = 0.34f;
  float shadowStrength = 0.94f;
  float backgroundStarIntensity = 1.15f;
  float exposure = 1.0f;
  glm::vec3 diskColor = glm::vec3(1.0f, 0.56f, 0.12f);
  BlackHoleQuality quality = BlackHoleQuality::High;
};

struct BlackHoleRendererStats {
  bool ready = false;
  bool computeReady = false;
  int quality = 1;
  bool blackBodyReady = false;
  bool outputReady = false;
  int outputWidth = 0;
  int outputHeight = 0;
  int iterations = 0;
  float renderScale = 1.0f;
};

class BlackHoleRenderer {
public:
  BlackHoleRenderer();
  ~BlackHoleRenderer();

  bool init(const std::string &vertexPath, const std::string &fragmentPath);
  void shutdown();
  void draw(const glm::mat4 &view, const glm::mat4 &projection,
            const BlackHoleSettings &settings, float timeSec);

  bool isReady() const;
  BlackHoleRendererStats stats() const;

private:
  void createFullscreenQuad_();
  bool createRaymarchResources_();
  bool ensureOutputTexture_(int viewportWidth, int viewportHeight,
                            BlackHoleQuality quality);
  GLuint compileComputeProgram_(const std::string &computePath);
  GLuint compileShader_(const std::string &path, GLenum type);
  void deleteTexture_(GLuint &texture);

  std::unique_ptr<Shader> mShader;
  GLuint mVAO = 0;
  GLuint mVBO = 0;
  GLuint mBlackBodyTex = 0;
  GLuint mOutputTex = 0;
  GLuint mComputeProgram = 0;
  int mOutputWidth = 0;
  int mOutputHeight = 0;
  int mLastIterations = 0;
  float mLastRenderScale = 1.0f;
  int mLastQuality = 1;
};
