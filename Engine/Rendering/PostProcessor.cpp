#include "PostProcessor.h"
#include "Logger.h"
#include "Shader.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <string>

namespace {
std::string pathDir(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  return (slash == std::string::npos) ? std::string("./")
                                      : path.substr(0, slash + 1);
}

std::string pathJoin(const std::string &a, const std::string &b) {
  if (a.empty())
    return b;
  char last = a.back();
  if (last == '/' || last == '\\')
    return a + b;
  return a + "/" + b;
}

float halton(uint64_t index, int base) {
  float f = 1.0f;
  float r = 0.0f;
  while (index > 0) {
    f /= (float)base;
    r += f * (float)(index % (uint64_t)base);
    index /= (uint64_t)base;
  }
  return r;
}
} // namespace

PostProcessor::PostProcessor() = default;
PostProcessor::~PostProcessor() = default;

void PostProcessor::init(
    const std::string &vertPath, const std::string &extFragPath,
    const std::string &blurFragPath, const std::string &ssaoFragPath,
    const std::string &ssaoBlurFragPath, const std::string &volumetricFragPath,
    const std::string &compFragPath, int width, int height) {
  mExtractShader =
      std::make_unique<Shader>(vertPath.c_str(), extFragPath.c_str());
  mBlurShader =
      std::make_unique<Shader>(vertPath.c_str(), blurFragPath.c_str());
  mSSAOShader =
      std::make_unique<Shader>(vertPath.c_str(), ssaoFragPath.c_str());
  mSSAOBlurShader =
      std::make_unique<Shader>(vertPath.c_str(), ssaoBlurFragPath.c_str());
  mVolumetricShader =
      std::make_unique<Shader>(vertPath.c_str(), volumetricFragPath.c_str());
  mCompositeShader =
      std::make_unique<Shader>(vertPath.c_str(), compFragPath.c_str());
  const std::string shaderDir = pathDir(compFragPath);
  const std::string taaFragPath = pathJoin(shaderDir, "taa_resolve.frag");
  const std::string fxaaFragPath = pathJoin(shaderDir, "fxaa.frag");
  mTAAShader = std::make_unique<Shader>(vertPath.c_str(), taaFragPath.c_str());
  mFXAAShader =
      std::make_unique<Shader>(vertPath.c_str(), fxaaFragPath.c_str());

  mExtractShader->activate();
  mExtractShader->setInt("scene", 0);

  mBlurShader->activate();
  mBlurShader->setInt("image", 0);

  mSSAOShader->activate();
  mSSAOShader->setInt("depthTex", 0);
  mSSAOShader->setInt("noiseTex", 1);

  mSSAOBlurShader->activate();
  mSSAOBlurShader->setInt("ssaoInput", 0);

  mVolumetricShader->activate();
  mVolumetricShader->setInt("depthTex", 0);

  mCompositeShader->activate();
  mCompositeShader->setInt("scene", 0);
  mCompositeShader->setInt("bloomBlur", 1);
  mCompositeShader->setInt("ssaoTex", 2);
  mCompositeShader->setInt("volumetricTex", 3);

  mTAAShader->activate();
  mTAAShader->setInt("uCurrentColor", 0);
  mTAAShader->setInt("uHistoryColor", 1);
  mTAAShader->setInt("uDepthTex", 2);

  mFXAAShader->activate();
  mFXAAShader->setInt("uSceneTex", 0);

  float quadVertices[] = {
      // positions        // texCoords
      -1.0f, 1.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f,
      1.0f,  1.0f, 1.0f, 1.0f, 1.0f,  -1.0f, 1.0f, 0.0f,
  };

  glGenVertexArrays(1, &mQuadVAO);
  glGenBuffers(1, &mQuadVBO);
  glBindVertexArray(mQuadVAO);
  glBindBuffer(GL_ARRAY_BUFFER, mQuadVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), &quadVertices,
               GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        (void *)(2 * sizeof(float)));
  glBindVertexArray(0);

  buildSSAOKernel_();

  mSSAOShader->activate();
  for (size_t i = 0; i < mSSAOKernel.size(); ++i) {
    mSSAOShader->setVec3("samples[" + std::to_string(i) + "]", mSSAOKernel[i]);
  }
  // Remove the static initialization here since it will be updated dynamically

  createBuffers_(width, height);
  resetTemporalHistory();
}

void PostProcessor::shutdown() {
  destroyBuffers_();

  if (mQuadVAO)
    glDeleteVertexArrays(1, &mQuadVAO);
  if (mQuadVBO)
    glDeleteBuffers(1, &mQuadVBO);

  if (mSSAONoiseTex)
    glDeleteTextures(1, &mSSAONoiseTex);
  mSSAONoiseTex = 0;

  mExtractShader.reset();
  mBlurShader.reset();
  mSSAOShader.reset();
  mSSAOBlurShader.reset();
  mVolumetricShader.reset();
  mCompositeShader.reset();
  mTAAShader.reset();
  mFXAAShader.reset();
  resetTemporalHistory();
}

void PostProcessor::resize(int width, int height) {
  if (mWidth == width && mHeight == height)
    return;
  destroyBuffers_();
  createBuffers_(width, height);
  resetTemporalHistory();
}

glm::mat4 PostProcessor::jitteredProjection(const glm::mat4 &projection,
                                            bool enabled) {
  if (!enabled || !enableTAA || mWidth <= 0 || mHeight <= 0) {
    mCurrentJitterNDC = glm::vec2(0.0f);
    return projection;
  }

  const uint64_t sample = (mFrameIndex % 1024ull) + 1ull;
  const float jx = halton(sample, 2) - 0.5f;
  const float jy = halton(sample, 3) - 0.5f;

  mCurrentJitterNDC =
      glm::vec2((2.0f * jx / (float)mWidth), (2.0f * jy / (float)mHeight)) *
      taaJitterScale;

  glm::mat4 out = projection;
  out[2][0] += mCurrentJitterNDC.x;
  out[2][1] += mCurrentJitterNDC.y;
  return out;
}

void PostProcessor::resetTemporalHistory() {
  mHasTAAHistory = false;
  mTAAHistoryRead = 0;
  mFrameIndex = 0;
  mPrevJitterNDC = glm::vec2(0.0f);
  mCurrentJitterNDC = glm::vec2(0.0f);
  mHasPrevCameraPos = false;
}

void PostProcessor::beginRenderPass() {
  glBindFramebuffer(GL_FRAMEBUFFER, mHDRFBO);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void PostProcessor::endRenderPass(const glm::mat4 &view,
                                  const glm::mat4 &projection,
                                  const glm::vec3 &cameraPos,
                                  const glm::vec3 &lightDir,
                                  const glm::vec3 &sunColor, float nearPlane,
                                  float farPlane, float timeSec) {
  // 1) Bloom extraction
  glBindFramebuffer(GL_FRAMEBUFFER, mPingPongFBO[0]);
  mExtractShader->activate();
  mExtractShader->setFloat("threshold", bloomThreshold);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, mColorTex);
  renderQuad_();

  // 2) Bloom blur
  bool horizontal = true, first_iteration = true;
  mBlurShader->activate();
  for (int i = 0; i < blurIterations; i++) {
    glBindFramebuffer(GL_FRAMEBUFFER, mPingPongFBO[horizontal]);
    mBlurShader->setBool("horizontal", horizontal);
    glBindTexture(GL_TEXTURE_2D, first_iteration ? mPingPongTex[0]
                                                 : mPingPongTex[!horizontal]);
    renderQuad_();
    horizontal = !horizontal;
    if (first_iteration)
      first_iteration = false;
  }

  // 3) SSAO generation + blur
  if (enableSSAO) {
    glBindFramebuffer(GL_FRAMEBUFFER, mSSAOFBO);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    mSSAOShader->activate();
    mSSAOShader->setFloat("radius", ssaoRadius);
    mSSAOShader->setFloat("bias", ssaoBias);
    mSSAOShader->setFloat("power", ssaoPower);
    mSSAOShader->setInt("sampleCount", ssaoSamples);
    mSSAOShader->setMat4("uProjection", projection);
    mSSAOShader->setMat4("uInvProjection", glm::inverse(projection));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mDepthTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mSSAONoiseTex);
    renderQuad_();

    glBindFramebuffer(GL_FRAMEBUFFER, mSSAOBlurFBO);
    glClear(GL_COLOR_BUFFER_BIT);
    mSSAOBlurShader->activate();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mSSAOTex);
    renderQuad_();
  } else {
    glBindFramebuffer(GL_FRAMEBUFFER, mSSAOBlurFBO);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
  }

  // 4) Volumetric fog + shafts
  if (enableVolumetricFog) {
    glm::vec3 sunWorldPos = cameraPos - glm::normalize(lightDir) * 1000.0f;
    glm::vec4 sunClip = projection * view * glm::vec4(sunWorldPos, 1.0f);
    glm::vec2 sunUV(0.5f);
    float sunVisible = 0.0f;
    if (sunClip.w > 0.0001f) {
      glm::vec3 ndc = glm::vec3(sunClip) / sunClip.w;
      sunUV = glm::vec2(ndc.x, ndc.y) * 0.5f + 0.5f;
      if (sunUV.x >= 0.0f && sunUV.x <= 1.0f && sunUV.y >= 0.0f &&
          sunUV.y <= 1.0f && ndc.z >= -1.0f && ndc.z <= 1.0f) {
        sunVisible = 1.0f;
      }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, mVolumetricFBO);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    mVolumetricShader->activate();
    mVolumetricShader->setFloat("nearPlane", nearPlane);
    mVolumetricShader->setFloat("farPlane", farPlane);
    mVolumetricShader->setFloat("fogDensity", volumetricFogDensity);
    mVolumetricShader->setFloat("lightExposure", volumetricLightExposure);
    mVolumetricShader->setFloat("lightDecay", volumetricLightDecay);
    mVolumetricShader->setFloat("lightWeight", volumetricLightWeight);
    mVolumetricShader->setInt("sampleCount", std::max(8, volumetricSamples));
    mVolumetricShader->setFloat("sunVisible", sunVisible);
    mVolumetricShader->setFloat("uTime", timeSec);
    mVolumetricShader->setVec3("sunColor", sunColor);
    mVolumetricShader->setVec3("uLightDir", glm::normalize(lightDir));
    mVolumetricShader->setFloat("sunPosX", sunUV.x);
    mVolumetricShader->setFloat("sunPosY", sunUV.y);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mDepthTex);
    renderQuad_();
  } else {
    glBindFramebuffer(GL_FRAMEBUFFER, mVolumetricFBO);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
  }

  // 5) Composite to intermediate HDR buffer
  glBindFramebuffer(GL_FRAMEBUFFER, mCompositeFBO);
  glClear(GL_COLOR_BUFFER_BIT);
  mCompositeShader->activate();
  mCompositeShader->setFloat("bloomIntensity", bloomIntensity);
  mCompositeShader->setFloat("uBrightness", brightness);
  mCompositeShader->setBool("uEnableSSAO", enableSSAO);
  mCompositeShader->setBool("uEnableVolumetric", enableVolumetricFog);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, mColorTex);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, mPingPongTex[!horizontal]);
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, mSSAOBlurTex);
  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_2D, mVolumetricTex);

  glDisable(GL_DEPTH_TEST);
  renderQuad_();

  GLuint aaInputTex = mCompositeTex;

  // 6) Temporal AA resolve
  if (enableTAA) {
    glBindFramebuffer(GL_FRAMEBUFFER, mAATempFBO);
    glClear(GL_COLOR_BUFFER_BIT);

    mTAAShader->activate();
    float historyBlend = std::clamp(taaHistoryBlend, 0.0f, 0.98f);
    if (!mHasTAAHistory)
      historyBlend = 0.0f;
    if (mHasPrevCameraPos) {
      const float motion = glm::length(cameraPos - mPrevCameraPos);
      const float motionDenom = std::max(0.0001f, taaMotionReset);
      const float motionFade = std::clamp(motion / motionDenom, 0.0f, 1.0f);
      historyBlend *= (1.0f - motionFade);
    }
    const glm::vec2 historyUVOffset =
        (mPrevJitterNDC - mCurrentJitterNDC) * 0.5f;
    mTAAShader->setFloat("uHistoryBlend", historyBlend);
    mTAAShader->setFloat("uInvResolutionX", 1.0f / std::max(1, mWidth));
    mTAAShader->setFloat("uInvResolutionY", 1.0f / std::max(1, mHeight));
    mTAAShader->setFloat("uHistoryUVOffsetX", historyUVOffset.x);
    mTAAShader->setFloat("uHistoryUVOffsetY", historyUVOffset.y);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, aaInputTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mHasTAAHistory
                                     ? mTAAHistoryTex[mTAAHistoryRead]
                                     : aaInputTex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, mDepthTex);
    renderQuad_();

    aaInputTex = mAATempTex;

    const int writeIdx = 1 - mTAAHistoryRead;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, mAATempFBO);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindTexture(GL_TEXTURE_2D, mTAAHistoryTex[writeIdx]);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, mWidth, mHeight);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    mTAAHistoryRead = writeIdx;
    mHasTAAHistory = true;
  } else {
    mHasTAAHistory = false;
  }

  // 7) Final output (FXAA or pass-through) to backbuffer
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  mFXAAShader->activate();
  mFXAAShader->setBool("uEnableFXAA", enableFXAA);
  mFXAAShader->setFloat("uInvResolutionX", 1.0f / std::max(1, mWidth));
  mFXAAShader->setFloat("uInvResolutionY", 1.0f / std::max(1, mHeight));
  mFXAAShader->setFloat("uSpanMax", fxaaSpanMax);
  mFXAAShader->setFloat("uReduceMin", fxaaReduceMin);
  mFXAAShader->setFloat("uReduceMul", fxaaReduceMul);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, aaInputTex);
  renderQuad_();

  mPrevJitterNDC = mCurrentJitterNDC;
  mPrevCameraPos = cameraPos;
  mHasPrevCameraPos = true;
  ++mFrameIndex;

  glEnable(GL_DEPTH_TEST);
}

void PostProcessor::buildSSAOKernel_() {
  mSSAOKernel.clear();
  mSSAOKernel.reserve(64);

  std::uniform_real_distribution<float> randomFloats(0.0f, 1.0f);
  std::uniform_real_distribution<float> randomSigned(-1.0f, 1.0f);
  std::default_random_engine generator;

  for (int i = 0; i < 64; ++i) {
    glm::vec3 sample(randomSigned(generator), randomSigned(generator),
                     randomFloats(generator));
    sample = glm::normalize(sample);
    sample *= randomFloats(generator);
    float scale = static_cast<float>(i) / 63.0f;
    scale = 0.1f + (1.0f - 0.1f) * (scale * scale);
    sample *= scale;
    mSSAOKernel.push_back(sample);
  }

  std::vector<glm::vec3> ssaoNoise;
  ssaoNoise.reserve(16);
  for (int i = 0; i < 16; ++i) {
    ssaoNoise.emplace_back(randomSigned(generator), randomSigned(generator),
                           0.0f);
  }

  if (mSSAONoiseTex == 0)
    glGenTextures(1, &mSSAONoiseTex);
  glBindTexture(GL_TEXTURE_2D, mSSAONoiseTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, 4, 4, 0, GL_RGB, GL_FLOAT,
               ssaoNoise.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void PostProcessor::createBuffers_(int width, int height) {
  mWidth = width;
  mHeight = height;

  glGenFramebuffers(1, &mHDRFBO);
  glBindFramebuffer(GL_FRAMEBUFFER, mHDRFBO);

  glGenTextures(1, &mColorTex);
  glBindTexture(GL_TEXTURE_2D, mColorTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA,
               GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         mColorTex, 0);

  glGenTextures(1, &mDepthTex);
  glBindTexture(GL_TEXTURE_2D, mDepthTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0,
               GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                         GL_TEXTURE_2D, mDepthTex, 0);

  GLuint attachments[1] = {GL_COLOR_ATTACHMENT0};
  glDrawBuffers(1, attachments);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    LOG_ERROR("PostProcessor", "HDR Framebuffer not complete!");

  glGenFramebuffers(2, mPingPongFBO);
  glGenTextures(2, mPingPongTex);
  for (unsigned int i = 0; i < 2; i++) {
    glBindFramebuffer(GL_FRAMEBUFFER, mPingPongFBO[i]);
    glBindTexture(GL_TEXTURE_2D, mPingPongTex[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA,
                 GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           mPingPongTex[i], 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      LOG_ERROR("PostProcessor", "PingPong Framebuffer not complete!");
  }

  glGenFramebuffers(1, &mSSAOFBO);
  glBindFramebuffer(GL_FRAMEBUFFER, mSSAOFBO);
  glGenTextures(1, &mSSAOTex);
  glBindTexture(GL_TEXTURE_2D, mSSAOTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED,
               GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         mSSAOTex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    LOG_ERROR("PostProcessor", "SSAO Framebuffer not complete!");

  glGenFramebuffers(1, &mSSAOBlurFBO);
  glBindFramebuffer(GL_FRAMEBUFFER, mSSAOBlurFBO);
  glGenTextures(1, &mSSAOBlurTex);
  glBindTexture(GL_TEXTURE_2D, mSSAOBlurTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED,
               GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         mSSAOBlurTex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    LOG_ERROR("PostProcessor", "SSAO Blur Framebuffer not complete!");

  glGenFramebuffers(1, &mVolumetricFBO);
  glBindFramebuffer(GL_FRAMEBUFFER, mVolumetricFBO);
  glGenTextures(1, &mVolumetricTex);
  glBindTexture(GL_TEXTURE_2D, mVolumetricTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA,
               GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         mVolumetricTex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    LOG_ERROR("PostProcessor", "Volumetric Framebuffer not complete!");

  glGenFramebuffers(1, &mCompositeFBO);
  glBindFramebuffer(GL_FRAMEBUFFER, mCompositeFBO);
  glGenTextures(1, &mCompositeTex);
  glBindTexture(GL_TEXTURE_2D, mCompositeTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA,
               GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         mCompositeTex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    LOG_ERROR("PostProcessor", "Composite Framebuffer not complete!");

  glGenFramebuffers(1, &mAATempFBO);
  glBindFramebuffer(GL_FRAMEBUFFER, mAATempFBO);
  glGenTextures(1, &mAATempTex);
  glBindTexture(GL_TEXTURE_2D, mAATempTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA,
               GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         mAATempTex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    LOG_ERROR("PostProcessor", "AA Temp Framebuffer not complete!");

  glGenTextures(2, mTAAHistoryTex);
  for (int i = 0; i < 2; ++i) {
    glBindTexture(GL_TEXTURE_2D, mTAAHistoryTex[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA,
                 GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void PostProcessor::destroyBuffers_() {
  if (mHDRFBO)
    glDeleteFramebuffers(1, &mHDRFBO);
  if (mColorTex)
    glDeleteTextures(1, &mColorTex);
  if (mDepthTex)
    glDeleteTextures(1, &mDepthTex);

  if (mPingPongFBO[0])
    glDeleteFramebuffers(2, mPingPongFBO);
  if (mPingPongTex[0])
    glDeleteTextures(2, mPingPongTex);

  if (mSSAOFBO)
    glDeleteFramebuffers(1, &mSSAOFBO);
  if (mSSAOTex)
    glDeleteTextures(1, &mSSAOTex);
  if (mSSAOBlurFBO)
    glDeleteFramebuffers(1, &mSSAOBlurFBO);
  if (mSSAOBlurTex)
    glDeleteTextures(1, &mSSAOBlurTex);
  if (mVolumetricFBO)
    glDeleteFramebuffers(1, &mVolumetricFBO);
  if (mVolumetricTex)
    glDeleteTextures(1, &mVolumetricTex);
  if (mCompositeFBO)
    glDeleteFramebuffers(1, &mCompositeFBO);
  if (mCompositeTex)
    glDeleteTextures(1, &mCompositeTex);
  if (mAATempFBO)
    glDeleteFramebuffers(1, &mAATempFBO);
  if (mAATempTex)
    glDeleteTextures(1, &mAATempTex);
  if (mTAAHistoryTex[0])
    glDeleteTextures(2, mTAAHistoryTex);

  mHDRFBO = 0;
  mColorTex = 0;
  mDepthTex = 0;
  mPingPongFBO[0] = mPingPongFBO[1] = 0;
  mPingPongTex[0] = mPingPongTex[1] = 0;
  mSSAOFBO = 0;
  mSSAOTex = 0;
  mSSAOBlurFBO = 0;
  mSSAOBlurTex = 0;
  mVolumetricFBO = 0;
  mVolumetricTex = 0;
  mCompositeFBO = 0;
  mCompositeTex = 0;
  mAATempFBO = 0;
  mAATempTex = 0;
  mTAAHistoryTex[0] = 0;
  mTAAHistoryTex[1] = 0;
}

void PostProcessor::renderQuad_() {
  glBindVertexArray(mQuadVAO);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
}
