#pragma once

#include "IEngineSubsystem.h"
#include "AudioSettings.h"
#include <glm/glm.hpp>
#include <string>

struct VkAppState;

class VkAudioSubsystem final : public IEngineSubsystem {
public:
  explicit VkAudioSubsystem(VkAppState &state);
  ~VkAudioSubsystem() override;

  std::string name() const override { return "VkAudioSubsystem"; }
  SubsystemPhase phase() const override { return SubsystemPhase::Foundation; }
  std::vector<std::string> dependencies() const override { return {"Window"}; }

  bool initialize() override;
  void shutdown() override;

  void update(float dt, const glm::vec3 &listenerPos,
              const glm::vec3 &listenerForward);
  void playTestFootstep();

  // --- Owned state (moved out of AppState) ---
  AudioSettings& settings() { return mSettings; }
  const AudioSettings& settings() const { return mSettings; }
  bool& backendAvailable() { return mBackendAvailable; }
  bool backendAvailable() const { return mBackendAvailable; }
  std::string& status() { return mStatus; }
  const std::string& status() const { return mStatus; }

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

  VkAppState &mState;
  AudioSettings mSettings;
  bool mBackendAvailable = false;
  std::string mStatus;
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
