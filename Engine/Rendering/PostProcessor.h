#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

class Shader;

class PostProcessor {
public:
  PostProcessor();
  ~PostProcessor();

  void init(const std::string &vertPath, const std::string &extFragPath,
            const std::string &blurFragPath, const std::string &ssaoFragPath,
            const std::string &ssaoBlurFragPath,
            const std::string &volumetricFragPath,
            const std::string &compFragPath, int width, int height);

  void shutdown();

  void resize(int width, int height);

  void beginRenderPass();
  void endRenderPass(const glm::mat4 &view, const glm::mat4 &projection,
                     const glm::vec3 &cameraPos, const glm::vec3 &lightDir,
                     const glm::vec3 &sunColor, float nearPlane, float farPlane,
                     float timeSec);
  void setForceSsaoFullRes(bool enabled) { mForceSsaoFullRes = enabled; }
  glm::mat4 jitteredProjection(const glm::mat4 &projection, bool enabled);
  void resetTemporalHistory();

  float bloomThreshold = 1.0f;
  int blurIterations = 5;
  float bloomScale = 0.5f;
  float bloomIntensity = 1.0f;
  float brightness = 1.0f;
  bool enableSSAO = true;
  int ssaoQuality = 1; // 0=Low, 1=Medium, 2=High, 3=Ultra, 4=Custom
  float ssaoRadius = 0.65f;
  float ssaoBias = 0.02f;
  float ssaoPower = 1.2f;
  int ssaoSamples = 16;
  float ssaoScale = 0.5f;
  bool ssaoScaleRadius = true;
  bool ssaoFullResTerrain = true;
  bool enableVolumetricFog = true;
  int volumetricQuality = 1; // 0=Low, 1=Medium, 2=High, 3=Ultra, 4=Custom
  float volumetricFogDensity = 0.018f;
  float volumetricLightExposure = 0.28f;
  float volumetricLightDecay = 0.95f;
  float volumetricLightWeight = 0.095f;
  int volumetricSamples = 16;
  float volumetricScale = 0.5f;
  bool enableTAA = true;
  float taaHistoryBlend = 0.88f;
  float taaJitterScale = 1.0f;
  float taaMotionReset = 0.45f;
  bool enableFXAA = true;
  float fxaaSpanMax = 8.0f;
  float fxaaReduceMin = 1.0f / 128.0f;
  float fxaaReduceMul = 1.0f / 8.0f;
  bool enableOutline = false;
  float outlineStrength = 0.8f;
  float outlineThreshold = 0.002f;
  float outlineThickness = 1.0f;
  glm::vec3 outlineColor = glm::vec3(0.02f, 0.02f, 0.02f);
  bool enableDistanceTint = false;
  float distanceTintStart = 80.0f;
  float distanceTintEnd = 220.0f;
  glm::vec3 distanceTintColor = glm::vec3(0.65f, 0.75f, 0.90f);
  bool enableColorGrade = false;
  float gradeSaturation = 1.1f;
  float gradeContrast = 1.05f;
  float gradeLift = 0.0f;
  float gradeGamma = 1.0f;
  float gradeGain = 1.0f;
  glm::vec3 gradeTint = glm::vec3(1.0f);
  bool enablePaletteQuantize = false;
  int paletteSteps = 6;

private:
  void createBuffers_(int width, int height);
  void buildSSAOKernel_();
  void destroyBuffers_();
  void renderQuad_();

  int mWidth = 0;
  int mHeight = 0;
  int mBloomWidth = 0;
  int mBloomHeight = 0;
  int mSSAOWidth = 0;
  int mSSAOHeight = 0;
  int mVolumetricWidth = 0;
  int mVolumetricHeight = 0;
  float mLastBloomScale = 1.0f;
  float mLastSSAOScale = 1.0f;
  float mLastVolumetricScale = 1.0f;
  bool mForceSsaoFullRes = false;

  GLuint mQuadVAO = 0;
  GLuint mQuadVBO = 0;

  GLuint mHDRFBO = 0;
  GLuint mColorTex = 0;
  GLuint mDepthTex = 0;
  GLuint mStencilRBO = 0;

  GLuint mPingPongFBO[2] = {0, 0};
  GLuint mPingPongTex[2] = {0, 0};
  GLuint mSSAOFBO = 0;
  GLuint mSSAOTex = 0;
  GLuint mSSAOBlurFBO = 0;
  GLuint mSSAOBlurTex = 0;
  GLuint mVolumetricFBO = 0;
  GLuint mVolumetricTex = 0;
  GLuint mCompositeFBO = 0;
  GLuint mCompositeTex = 0;
  GLuint mAATempFBO = 0;
  GLuint mAATempTex = 0;
  GLuint mTAAHistoryTex[2] = {0, 0};
  int mTAAHistoryRead = 0;
  bool mHasTAAHistory = false;
  GLuint mSSAONoiseTex = 0;
  std::vector<glm::vec3> mSSAOKernel;
  uint64_t mFrameIndex = 0;
  glm::vec2 mCurrentJitterNDC = glm::vec2(0.0f);
  glm::vec2 mPrevJitterNDC = glm::vec2(0.0f);
  glm::vec3 mPrevCameraPos = glm::vec3(0.0f);
  bool mHasPrevCameraPos = false;

  std::unique_ptr<Shader> mExtractShader;
  std::unique_ptr<Shader> mBlurShader;
  std::unique_ptr<Shader> mSSAOShader;
  std::unique_ptr<Shader> mSSAOBlurShader;
  std::unique_ptr<Shader> mVolumetricShader;
  std::unique_ptr<Shader> mCompositeShader;
  std::unique_ptr<Shader> mTAAShader;
  std::unique_ptr<Shader> mFXAAShader;
};
