#include "BlackHoleRenderer.h"

#include "BlackHolePrecompute.h"
#include "GLStateCache.h"
#include "Logger.h"
#include "Shader.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace {
int qualityToInt(BlackHoleQuality quality) {
  return std::clamp(static_cast<int>(quality), 0, 2);
}

float qualityRenderScale(BlackHoleQuality quality) {
  switch (quality) {
  case BlackHoleQuality::Performance:
    return 0.50f;
  case BlackHoleQuality::Ultra:
    return 1.00f;
  case BlackHoleQuality::High:
  default:
    return 0.75f;
  }
}

int qualityIterations(BlackHoleQuality quality) {
  switch (quality) {
  case BlackHoleQuality::Performance:
    return 256;
  case BlackHoleQuality::Ultra:
    return 768;
  case BlackHoleQuality::High:
  default:
    return 512;
  }
}
} // namespace

BlackHoleRenderer::BlackHoleRenderer() = default;
BlackHoleRenderer::~BlackHoleRenderer() { shutdown(); }

bool BlackHoleRenderer::init(const std::string &vertexPath,
                             const std::string &fragmentPath) {
  if (!GLAD_GL_VERSION_4_3) {
    LOG_ERROR("Render",
              "BlackHoleRenderer raymarch path requires OpenGL 4.3+ compute shaders");
    return false;
  }

  mShader = std::make_unique<Shader>(vertexPath.c_str(), fragmentPath.c_str());
  if (!mShader || !mShader->isValid()) {
    mShader.reset();
    return false;
  }

  const std::string computePath =
      fragmentPath.substr(0, fragmentPath.find_last_of("/\\") + 1) +
      "black_hole_raymarch.comp";
  mComputeProgram = compileComputeProgram_(computePath);
  if (mComputeProgram == 0) {
    shutdown();
    return false;
  }

  createFullscreenQuad_();
  if (!createRaymarchResources_()) {
    shutdown();
    return false;
  }

  LOG_INFO("Render", "BlackHoleRenderer raymarch compute path initialized");
  return isReady();
}

void BlackHoleRenderer::shutdown() {
  deleteTexture_(mOutputTex);
  deleteTexture_(mBlackBodyTex);
  if (mComputeProgram != 0) {
    glDeleteProgram(mComputeProgram);
    mComputeProgram = 0;
  }
  if (mVBO)
    glDeleteBuffers(1, &mVBO);
  if (mVAO)
    glDeleteVertexArrays(1, &mVAO);

  mVBO = 0;
  mVAO = 0;
  mOutputWidth = 0;
  mOutputHeight = 0;
  mShader.reset();
}

bool BlackHoleRenderer::isReady() const {
  return mShader && mShader->isValid() && mVAO != 0 && mComputeProgram != 0 &&
         mBlackBodyTex != 0;
}

BlackHoleRendererStats BlackHoleRenderer::stats() const {
  BlackHoleRendererStats s;
  s.ready = isReady();
  s.computeReady = mComputeProgram != 0;
  s.quality = mLastQuality;
  s.blackBodyReady = mBlackBodyTex != 0;
  s.outputReady = mOutputTex != 0;
  s.outputWidth = mOutputWidth;
  s.outputHeight = mOutputHeight;
  s.iterations = mLastIterations;
  s.renderScale = mLastRenderScale;
  return s;
}

void BlackHoleRenderer::createFullscreenQuad_() {
  const float quad[] = {
      -1.f, -1.f, 0.f, 0.f, 1.f,  -1.f, 1.f, 0.f,
      1.f,  1.f,  1.f, 1.f, -1.f, -1.f, 0.f, 0.f,
      1.f,  1.f,  1.f, 1.f, -1.f, 1.f,  0.f, 1.f,
  };

  glGenVertexArrays(1, &mVAO);
  glGenBuffers(1, &mVBO);

  glBindVertexArray(mVAO);
  glBindBuffer(GL_ARRAY_BUFFER, mVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        reinterpret_cast<void *>(0));
  glEnableVertexAttribArray(0);

  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        reinterpret_cast<void *>(2 * sizeof(float)));
  glEnableVertexAttribArray(1);
  glBindVertexArray(0);
}

void BlackHoleRenderer::deleteTexture_(GLuint &texture) {
  if (texture)
    glDeleteTextures(1, &texture);
  texture = 0;
}

bool BlackHoleRenderer::createRaymarchResources_() {
  const BlackHolePrecomputeData data = BlackHolePrecompute::generate();
  glGenTextures(1, &mBlackBodyTex);
  glBindTexture(GL_TEXTURE_2D, mBlackBodyTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, data.blackBodyWidth, 1, 0, GL_RGB,
               GL_FLOAT, data.blackBodyRGB.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  return mBlackBodyTex != 0;
}

bool BlackHoleRenderer::ensureOutputTexture_(int viewportWidth, int viewportHeight,
                                             BlackHoleQuality quality) {
  mLastRenderScale = qualityRenderScale(quality);
  const int width =
      std::max(16, static_cast<int>(viewportWidth * mLastRenderScale));
  const int height =
      std::max(16, static_cast<int>(viewportHeight * mLastRenderScale));
  if (mOutputTex != 0 && width == mOutputWidth && height == mOutputHeight)
    return true;

  deleteTexture_(mOutputTex);
  glGenTextures(1, &mOutputTex);
  glBindTexture(GL_TEXTURE_2D, mOutputTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA,
               GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  mOutputWidth = width;
  mOutputHeight = height;
  return mOutputTex != 0;
}

GLuint BlackHoleRenderer::compileShader_(const std::string &path, GLenum type) {
  std::ifstream file(path);
  if (!file.is_open()) {
    LOG_ERROR("Render", "Could not open shader source: " + path);
    return 0;
  }
  std::stringstream ss;
  ss << file.rdbuf();
  const std::string source = ss.str();
  const char *src = source.c_str();

  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &src, nullptr);
  glCompileShader(shader);

  GLint success = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
  if (!success) {
    char infoLog[2048];
    glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
    LOG_ERROR("Render", "Error compiling compute shader " + path + ": " +
                            std::string(infoLog));
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

GLuint BlackHoleRenderer::compileComputeProgram_(const std::string &computePath) {
  GLuint computeShader = compileShader_(computePath, GL_COMPUTE_SHADER);
  if (computeShader == 0)
    return 0;

  GLuint program = glCreateProgram();
  glAttachShader(program, computeShader);
  glLinkProgram(program);
  glDeleteShader(computeShader);

  GLint success = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &success);
  if (!success) {
    char infoLog[2048];
    glGetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
    LOG_ERROR("Render", "Compute shader link error: " + std::string(infoLog));
    glDeleteProgram(program);
    return 0;
  }
  return program;
}

void BlackHoleRenderer::draw(const glm::mat4 &view, const glm::mat4 &projection,
                             const BlackHoleSettings &settings,
                             float timeSec) {
  if (!isReady() || !settings.enabled)
    return;

  GLint viewport[4] = {0, 0, 1, 1};
  glGetIntegerv(GL_VIEWPORT, viewport);
  const int viewportWidth = std::max(1, viewport[2]);
  const int viewportHeight = std::max(1, viewport[3]);
  if (!ensureOutputTexture_(viewportWidth, viewportHeight, settings.quality))
    return;

  mLastQuality = qualityToInt(settings.quality);
  mLastIterations = qualityIterations(settings.quality);

  const glm::vec3 direction = glm::normalize(settings.direction);
  glm::vec3 refUp = std::abs(direction.y) > 0.94f ? glm::vec3(1.0f, 0.0f, 0.0f)
                                                  : glm::vec3(0.0f, 1.0f, 0.0f);
  const glm::vec3 skyRight = glm::normalize(glm::cross(refUp, direction));
  const glm::vec3 skyUp = glm::normalize(glm::cross(direction, skyRight));
  const glm::mat4 pitchRotation =
      glm::rotate(glm::mat4(1.0f), glm::radians(settings.viewPitchDeg),
                  skyRight);
  const glm::vec3 cameraPos =
      glm::vec3(pitchRotation * glm::vec4(-direction * 15.0f, 1.0f));
  refUp = glm::normalize(glm::vec3(pitchRotation * glm::vec4(refUp, 0.0f)));
  const glm::mat4 raymarchView =
      glm::lookAt(cameraPos, glm::vec3(0.0f), refUp);
  const glm::vec3 cameraForward = glm::normalize(-cameraPos);
  const glm::vec3 cameraRight =
      glm::normalize(glm::cross(refUp, cameraForward));
  const glm::vec3 cameraUp =
      glm::normalize(glm::cross(cameraForward, cameraRight));
  const glm::mat4 invProj = glm::inverse(projection);
  const glm::mat4 invView = glm::inverse(view);
  const float diskInnerRadius =
      glm::clamp(settings.innerDiskRadius, 0.85f, 8.0f) * 2.4f;
  const float diskOuterRadius =
      std::max(diskInnerRadius + 0.75f,
               glm::clamp(settings.outerDiskRadius, 2.5f, 24.0f) * 2.4f);

  GLStateCache::instance().useProgram(mComputeProgram);
  auto loc = [&](const char *name) { return glGetUniformLocation(mComputeProgram, name); };
  glUniform2f(loc("uResolution"), static_cast<float>(mOutputWidth),
              static_cast<float>(mOutputHeight));
  glUniform1f(loc("uTime"), timeSec);
  glUniformMatrix4fv(loc("uView"), 1, GL_FALSE, glm::value_ptr(raymarchView));
  glUniform3fv(loc("uCameraPos"), 1, glm::value_ptr(cameraPos));
  glUniformMatrix4fv(loc("uEngineInvProj"), 1, GL_FALSE,
                     glm::value_ptr(invProj));
  glUniformMatrix4fv(loc("uEngineInvView"), 1, GL_FALSE,
                     glm::value_ptr(invView));
  glUniform3fv(loc("uCameraRight"), 1, glm::value_ptr(cameraRight));
  glUniform3fv(loc("uCameraUp"), 1, glm::value_ptr(cameraUp));
  glUniform3fv(loc("uCameraForward"), 1, glm::value_ptr(cameraForward));
  glUniform3fv(loc("uSkyDirection"), 1, glm::value_ptr(direction));
  glUniform3fv(loc("uSkyRight"), 1, glm::value_ptr(skyRight));
  glUniform3fv(loc("uSkyUp"), 1, glm::value_ptr(skyUp));
  glUniform1f(loc("uAngularRadius"),
              glm::radians(glm::clamp(settings.eventHorizonSizeDeg, 0.10f,
                                      20.0f)));
  glUniform1i(loc("uIntegrationType"), 0);
  glUniform1i(loc("uDisk"), 1);
  glUniform1i(loc("uDopplerShift"), 1);
  glUniform1i(loc("uGravitationalRedShift"), 1);
  glUniform1i(loc("uBeaming"), 1);
  glUniform1i(loc("uRealisticTemperature"), 1);
  glUniform1f(loc("uDiskInnerRadius"), diskInnerRadius);
  glUniform1f(loc("uDiskOuterRadius"), diskOuterRadius);
  glUniform1f(loc("uDiskTiltDeg"), settings.diskTiltDeg);
  glUniform1f(loc("uDiskInclinationDeg"), settings.diskInclinationDeg);
  glUniform1f(loc("uDiskDensity"),
              glm::clamp(settings.diskDensity, 0.0f, 3.0f));
  glUniform1f(loc("uDiskTurbulence"),
              glm::clamp(settings.diskTurbulence, 0.0f, 1.0f));
  glUniform1f(loc("uDiskSpinSpeed"),
              glm::clamp(settings.diskSpinSpeed, 0.0f, 8.0f));
  glUniform1f(loc("uDiskFlowShear"),
              glm::clamp(settings.diskFlowShear, 0.0f, 1.0f));
  const float diskSpinPhase = std::fmod(std::max(timeSec, 0.0f), 4096.0f);
  glUniform1f(loc("uDiskSpinPhase"), diskSpinPhase);
  glUniform3fv(loc("uDiskColor"), 1, glm::value_ptr(glm::max(
                                         settings.diskColor, glm::vec3(0.0f))));
  glUniform1f(loc("uDopplerStrength"),
              glm::clamp(settings.dopplerStrength, 0.0f, 2.0f));
  glUniform1f(loc("uLensingStrength"),
              glm::clamp(settings.lensingStrength, 0.0f, 1.5f));
  glUniform1f(loc("uPhotonRingIntensity"),
              glm::clamp(settings.photonRingIntensity, 0.0f, 12.0f));
  glUniform1f(loc("uRingWidth"), glm::clamp(settings.ringWidth, 0.02f, 0.90f));
  glUniform1f(loc("uCoronaIntensity"),
              glm::clamp(settings.coronaIntensity, 0.0f, 2.0f));
  glUniform1f(loc("uShadowStrength"),
              glm::clamp(settings.shadowStrength, 0.0f, 1.0f));
  glUniform1f(loc("uAccretionTemp"),
              2000.0f + glm::clamp(settings.diskTemperature, 0.15f, 3.0f) *
                            6000.0f);
  glUniform1f(loc("uBackgroundStarIntensity"),
              glm::clamp(settings.backgroundStarIntensity, 0.0f, 4.0f));
  glUniform1f(loc("uExposure"), glm::clamp(settings.exposure, 0.05f, 6.0f));

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, mBlackBodyTex);
  glBindImageTexture(0, mOutputTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
  glDispatchCompute((GLuint)((mOutputWidth + 15) / 16),
                    (GLuint)((mOutputHeight + 15) / 16), 1);
  glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                  GL_TEXTURE_FETCH_BARRIER_BIT);

  glDepthMask(GL_FALSE);
  glDepthFunc(GL_LEQUAL);

  mShader->activate();
  mShader->setInt("uRaymarchTex", 0);
  mShader->setFloat("uExposure", glm::clamp(settings.exposure, 0.05f, 6.0f));
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, mOutputTex);

  glBindVertexArray(mVAO);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glBindVertexArray(0);

  glDepthMask(GL_TRUE);
  glDepthFunc(GL_LESS);
}
