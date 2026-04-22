#pragma once

#include "IEngineSubsystem.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>

struct AppState;

class RenderLoopSubsystem : public IEngineSubsystem {
public:
  RenderLoopSubsystem(AppState &state) : mState(state) {}

  std::string name() const override { return "RenderLoopSubsystem"; }
  SubsystemPhase phase() const override {
    return SubsystemPhase::Application;
  }
  std::vector<std::string> dependencies() const override {
    return {"RuntimeSystems"};
  }

  bool initialize() override;
  void shutdown() override;

  void executeRenderPasses(const glm::mat4 &view, const glm::mat4 &projection,
                           const glm::vec3 &cameraPos,
                           const glm::vec3 &cameraFront,
                           const glm::vec3 &cameraUp, float renderTime);

private:
  enum MainGpuTimerSlot {
    MainGpuTimerSky = 0,
    MainGpuTimerTerrain,
    MainGpuTimerScene,
    MainGpuTimerPost,
    MainGpuTimerCount
  };

  void renderShadowPass(const glm::mat4 &view, const glm::mat4 &projection,
                        const glm::vec3 &cameraPos,
                        const glm::vec3 &sunDirRaw, float nearPlane,
                        float farPlane);
  void renderMainPass(const glm::mat4 &view, const glm::mat4 &projection,
                      const glm::vec3 &cameraPos, const glm::vec3 &cameraFront,
                      const glm::vec3 &cameraUp, float nowT);

  AppState &mState;
  unsigned int mShadowQueries[2] = {0, 0};
  int mShadowQueryIndex = 0;
  bool mShadowQuerySubmitted[2] = {false, false};
  bool mShadowValid = false;
  glm::vec3 mLastShadowCamPos{0.0f};
  glm::vec3 mLastShadowSunDir{0.0f, -1.0f, 0.0f};
  glm::vec3 mLastCascadeCamPos[4] = {glm::vec3(0.0f), glm::vec3(0.0f),
                                     glm::vec3(0.0f), glm::vec3(0.0f)};
  glm::vec3 mLastCascadeSunDir[4] = {glm::vec3(0.0f, -1.0f, 0.0f),
                                     glm::vec3(0.0f, -1.0f, 0.0f),
                                     glm::vec3(0.0f, -1.0f, 0.0f),
                                     glm::vec3(0.0f, -1.0f, 0.0f)};
  uint64_t mCascadeLastUpdateFrame[4] = {0, 0, 0, 0};
  bool mCascadeValid[4] = {false, false, false, false};
  uint64_t mLastShadowConfigHash = 0;
  uint64_t mShadowFrameIndex = 0;

  unsigned int mMainQueries[MainGpuTimerCount][2] = {};
  int mMainQueryIndex = 0;
  bool mMainQuerySubmitted[MainGpuTimerCount][2] = {};
  bool mMainQueryPrimed = false;
};
