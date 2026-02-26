#include "PostProcessor.h"
#include "Logger.h"
#include "Shader.h"

PostProcessor::PostProcessor() = default;
PostProcessor::~PostProcessor() = default;

void PostProcessor::init(const std::string &vertPath,
                         const std::string &extFragPath,
                         const std::string &blurFragPath,
                         const std::string &compFragPath, int width,
                         int height) {
  mExtractShader =
      std::make_unique<Shader>(vertPath.c_str(), extFragPath.c_str());
  mBlurShader =
      std::make_unique<Shader>(vertPath.c_str(), blurFragPath.c_str());
  mCompositeShader =
      std::make_unique<Shader>(vertPath.c_str(), compFragPath.c_str());

  mExtractShader->activate();
  mExtractShader->setInt("scene", 0);

  mBlurShader->activate();
  mBlurShader->setInt("image", 0);

  mCompositeShader->activate();
  mCompositeShader->setInt("scene", 0);
  mCompositeShader->setInt("bloomBlur", 1);

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

  createBuffers_(width, height);
}

void PostProcessor::shutdown() {
  destroyBuffers_();

  if (mQuadVAO)
    glDeleteVertexArrays(1, &mQuadVAO);
  if (mQuadVBO)
    glDeleteBuffers(1, &mQuadVBO);

  mExtractShader.reset();
  mBlurShader.reset();
  mCompositeShader.reset();
}

void PostProcessor::resize(int width, int height) {
  if (mWidth == width && mHeight == height)
    return;
  destroyBuffers_();
  createBuffers_(width, height);
}

void PostProcessor::beginRenderPass() {
  glBindFramebuffer(GL_FRAMEBUFFER, mHDRFBO);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void PostProcessor::endRenderPass() {
  // 1. Extract Brightness
  glBindFramebuffer(GL_FRAMEBUFFER, mPingPongFBO[0]);
  mExtractShader->activate();
  mExtractShader->setFloat("threshold", bloomThreshold);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, mColorTex);
  renderQuad_();

  // 2. Gaussian Blur
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

  // 3. Composite and render to Default Framebuffer
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  mCompositeShader->activate();
  mCompositeShader->setFloat("bloomIntensity", bloomIntensity);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, mColorTex);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, mPingPongTex[!horizontal]);

  // In OpenGL, depth testing might be on. We don't need it for full screen
  // quad.
  glDisable(GL_DEPTH_TEST);
  renderQuad_();
  glEnable(GL_DEPTH_TEST);
}

void PostProcessor::createBuffers_(int width, int height) {
  mWidth = width;
  mHeight = height;

  glGenFramebuffers(1, &mHDRFBO);
  glBindFramebuffer(GL_FRAMEBUFFER, mHDRFBO);

  glGenTextures(1, &mColorTex);
  glBindTexture(GL_TEXTURE_2D, mColorTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA,
               GL_FLOAT, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         mColorTex, 0);

  GLuint attachments[1] = {GL_COLOR_ATTACHMENT0};
  glDrawBuffers(1, attachments);

  glGenRenderbuffers(1, &mDepthRBO);
  glBindRenderbuffer(GL_RENDERBUFFER, mDepthRBO);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                            GL_RENDERBUFFER, mDepthRBO);

  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    LOG_ERROR("PostProcessor", "HDR Framebuffer not complete!");

  glGenFramebuffers(2, mPingPongFBO);
  glGenTextures(2, mPingPongTex);
  for (unsigned int i = 0; i < 2; i++) {
    glBindFramebuffer(GL_FRAMEBUFFER, mPingPongFBO[i]);
    glBindTexture(GL_TEXTURE_2D, mPingPongTex[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA,
                 GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           mPingPongTex[i], 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      LOG_ERROR("PostProcessor", "PingPong Framebuffer not complete!");
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void PostProcessor::destroyBuffers_() {
  if (mHDRFBO)
    glDeleteFramebuffers(1, &mHDRFBO);
  if (mDepthRBO)
    glDeleteRenderbuffers(1, &mDepthRBO);
  if (mColorTex)
    glDeleteTextures(1, &mColorTex);

  if (mPingPongFBO[0])
    glDeleteFramebuffers(2, mPingPongFBO);
  if (mPingPongTex[0])
    glDeleteTextures(2, mPingPongTex);

  mHDRFBO = mDepthRBO = 0;
  mColorTex = 0;
  mPingPongFBO[0] = mPingPongFBO[1] = 0;
  mPingPongTex[0] = mPingPongTex[1] = 0;
}

void PostProcessor::renderQuad_() {
  glBindVertexArray(mQuadVAO);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
}
