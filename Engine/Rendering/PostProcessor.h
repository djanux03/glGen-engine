#pragma once
#include <glad/glad.h>
#include <memory>
#include <string>

class Shader;

class PostProcessor {
public:
  PostProcessor();
  ~PostProcessor();

  void init(const std::string &vertPath, const std::string &extFragPath,
            const std::string &blurFragPath, const std::string &compFragPath,
            int width, int height);

  void shutdown();

  void resize(int width, int height);

  void beginRenderPass();
  void endRenderPass();

  float bloomThreshold = 1.0f;
  int blurIterations = 10;
  float bloomIntensity = 1.0f;

private:
  void createBuffers_(int width, int height);
  void destroyBuffers_();
  void renderQuad_();

  int mWidth = 0;
  int mHeight = 0;

  GLuint mQuadVAO = 0;
  GLuint mQuadVBO = 0;

  GLuint mHDRFBO = 0;
  GLuint mColorTex = 0;
  GLuint mDepthRBO = 0;

  GLuint mPingPongFBO[2] = {0, 0};
  GLuint mPingPongTex[2] = {0, 0};

  std::unique_ptr<Shader> mExtractShader;
  std::unique_ptr<Shader> mBlurShader;
  std::unique_ptr<Shader> mCompositeShader;
};
