#pragma once

#include "IEngineSubsystem.h"
#include <glm/glm.hpp>
#include <string>

struct AppState;

class AudioSubsystem final : public IEngineSubsystem {
public:
  explicit AudioSubsystem(AppState &state);
  ~AudioSubsystem() override;

  std::string name() const override { return "AudioSubsystem"; }
  SubsystemPhase phase() const override { return SubsystemPhase::Foundation; }
  std::vector<std::string> dependencies() const override { return {"Window"}; }

  bool initialize() override;
  void shutdown() override;

  void update(float dt, const glm::vec3 &listenerPos,
              const glm::vec3 &listenerForward);
  void playTestFootstep();

private:
  struct ManagedSound;

  bool initEngine_();
  void syncAmbient_();
  void syncFootsteps_();
  void updateFootstepMotion_(float dt);
  void applyVolumes_();
  void refreshFootstepClipPool_();
  std::string nextFootstepClip_();
  void playFootstepOneShot_(const std::string &path);
  std::string resolvePath_(const std::string &path) const;
  bool loadSound_(ManagedSound &slot, const std::string &path, bool looped,
                  bool streamed);
  void unloadSound_(ManagedSound &slot);

  AppState &mState;
  void *mEngineStorage = nullptr;
  ManagedSound *mAmbient = nullptr;
  ManagedSound *mFootsteps = nullptr;
  bool mInitialized = false;
  bool mAudioAvailable = false;
  glm::vec3 mLastPlayerPos{0.0f};
  bool mHasLastPlayerPos = false;
  float mLastHorizontalSpeed = 0.0f;
  float mFootstepTimer = 0.0f;
  bool mFootstepWasMoving = false;
  std::vector<std::string> mFootstepClipPaths;
  size_t mFootstepClipIndex = 0;
};
