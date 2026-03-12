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
  glm::mat4 jitteredProjection(const glm::mat4 &projection, bool enabled);
  void resetTemporalHistory();

  float bloomThreshold = 1.0f;
  int blurIterations = 10;
  float bloomIntensity = 1.0f;
  float brightness = 1.0f;
  bool enableSSAO = true;
  float ssaoRadius = 0.65f;
  float ssaoBias = 0.02f;
  float ssaoPower = 1.2f;
  int ssaoSamples = 32;
  bool enableVolumetricFog = true;
  float volumetricFogDensity = 0.018f;
  float volumetricLightExposure = 0.28f;
  float volumetricLightDecay = 0.95f;
  float volumetricLightWeight = 0.095f;
  int volumetricSamples = 32;
  bool enableTAA = true;
  float taaHistoryBlend = 0.88f;
  float taaJitterScale = 1.0f;
  float taaMotionReset = 0.45f;
  bool enableFXAA = true;
  float fxaaSpanMax = 8.0f;
  float fxaaReduceMin = 1.0f / 128.0f;
  float fxaaReduceMul = 1.0f / 8.0f;

private:
  void createBuffers_(int width, int height);
  void buildSSAOKernel_();
  void destroyBuffers_();
  void renderQuad_();

  int mWidth = 0;
  int mHeight = 0;

  GLuint mQuadVAO = 0;
  GLuint mQuadVBO = 0;

  GLuint mHDRFBO = 0;
  GLuint mColorTex = 0;
  GLuint mDepthTex = 0;

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
