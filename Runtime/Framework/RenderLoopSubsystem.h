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
  void renderShadowPass(const glm::vec3 &lightPos, float nearPlane,
                        float farPlane);
  void renderMainPass(const glm::mat4 &view, const glm::mat4 &projection,
                      const glm::vec3 &cameraPos, const glm::vec3 &cameraFront,
                      const glm::vec3 &cameraUp, float nowT);

  AppState &mState;
};
