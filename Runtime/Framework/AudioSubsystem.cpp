#include "AudioSubsystem.h"

#include "AppState.h"
#include "ECS/Components.h"
#include "ECS/Systems/PhysicsSystem.h"
#include "Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

namespace {
struct EngineHolder {
  ma_engine engine{};
  ma_sound_group footstepsGroup{};
  bool footstepsGroupReady = false;
};

bool isVorbisPath(const std::string &path) {
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos)
    return false;
  std::string ext = path.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return ext == "ogg" || ext == "oga";
}
} // namespace

struct AudioSubsystem::ManagedSound {
  ma_sound sound{};
  ma_decoder decoder{};
  bool loaded = false;
  bool decoderLoaded = false;
  std::string path;
  bool looped = false;
  bool warnedMissing = false;
  bool warnedLoad = false;
  bool startedLogged = false;
  std::vector<ma_uint8> encodedData;
};

AudioSubsystem::AudioSubsystem(AppState &state) : mState(state) {
  mAmbient = new ManagedSound();
  mFootsteps = new ManagedSound();
}

AudioSubsystem::~AudioSubsystem() {
  shutdown();
  delete mAmbient;
  delete mFootsteps;
}

bool AudioSubsystem::initialize() { return initEngine_(); }

void AudioSubsystem::shutdown() {
  if (mAmbient)
    unloadSound_(*mAmbient);
  if (mFootsteps)
    unloadSound_(*mFootsteps);

  if (mInitialized && mEngineStorage) {
    auto *holder = static_cast<EngineHolder *>(mEngineStorage);
    if (holder->footstepsGroupReady) {
      ma_sound_group_uninit(&holder->footstepsGroup);
      holder->footstepsGroupReady = false;
    }
    ma_engine_uninit(&holder->engine);
    delete holder;
  }

  mEngineStorage = nullptr;
  mInitialized = false;
  mAudioAvailable = false;
  mState.audioBackendAvailable = false;
  mState.audioStatus = "Audio offline";
  mHasLastPlayerPos = false;
  mLastHorizontalSpeed = 0.0f;
  mFootstepTimer = 0.0f;
  mFootstepWasMoving = false;
  mFootstepClipPaths.clear();
  mFootstepClipIndex = 0;
}

void AudioSubsystem::update(float dt, const glm::vec3 &listenerPos,
                            const glm::vec3 &listenerForward) {
  if (!mAudioAvailable || !mEngineStorage)
    return;

  auto *holder = static_cast<EngineHolder *>(mEngineStorage);
  mState.audioBackendAvailable = true;
  ma_engine_listener_set_position(&holder->engine, 0, listenerPos.x,
                                  listenerPos.y, listenerPos.z);
  ma_engine_listener_set_direction(&holder->engine, 0, listenerForward.x,
                                   listenerForward.y, listenerForward.z);
  ma_engine_listener_set_world_up(&holder->engine, 0, 0.0f, 1.0f, 0.0f);

  syncAmbient_();
  syncFootsteps_();
  updateFootstepMotion_(dt);
  applyVolumes_();
}

void AudioSubsystem::playTestFootstep() {
  if (!mAudioAvailable) {
    LOG_ERROR("Audio", "Audio backend unavailable for footstep test");
    mState.audioBackendAvailable = false;
    mState.audioStatus = "Audio backend unavailable";
    return;
  }
  refreshFootstepClipPool_();
  const std::string resolvedPath = nextFootstepClip_();
  if (resolvedPath.empty() || !std::filesystem::exists(resolvedPath)) {
    LOG_ERROR("Audio", "Footstep test clip not found");
    mState.audioStatus = "Footstep test file missing";
    return;
  }
  playFootstepOneShot_(resolvedPath);
  mState.audioStatus = "Test playback triggered";
  LOG_INFO("Audio", "Manual footstep test triggered: " + resolvedPath);
}

bool AudioSubsystem::initEngine_() {
  if (mInitialized)
    return true;

  auto *holder = new EngineHolder();
  ma_result res = ma_engine_init(nullptr, &holder->engine);
  if (res != MA_SUCCESS) {
    delete holder;
    LOG_WARN("Audio", "Audio device initialization failed; continuing without sound");
    mInitialized = true;
    mAudioAvailable = false;
    mState.audioBackendAvailable = false;
    mState.audioStatus =
        "Audio init failed (" + std::to_string((int)res) + ")";
    return true;
  }

  mEngineStorage = holder;
  res = ma_sound_group_init(&holder->engine, MA_SOUND_FLAG_NO_SPATIALIZATION,
                            nullptr, &holder->footstepsGroup);
  if (res == MA_SUCCESS) {
    holder->footstepsGroupReady = true;
  } else {
    LOG_WARN("Audio", "Footstep group init failed; footsteps will use engine master only");
  }
  mInitialized = true;
  mAudioAvailable = true;
  mState.audioBackendAvailable = true;
  mState.audioStatus = "Audio initialized";
  LOG_INFO("Audio", "Audio subsystem initialized");
  return true;
}

bool AudioSubsystem::loadSound_(ManagedSound &slot, const std::string &path,
                                bool looped, bool streamed) {
  if (!mAudioAvailable || !mEngineStorage)
    return false;

  const std::string resolvedPath = resolvePath_(path);
  if (resolvedPath.empty() || !std::filesystem::exists(resolvedPath)) {
    if (!slot.warnedMissing) {
      LOG_ERROR("Audio", "Sound file not found: " + path);
      slot.warnedMissing = true;
      mState.audioStatus = "Missing audio file";
    }
    unloadSound_(slot);
    slot.warnedMissing = true;
    return false;
  }
  slot.warnedMissing = false;

  if (slot.loaded && slot.path == resolvedPath && slot.looped == looped)
    return true;

  unloadSound_(slot);

  std::ifstream file(resolvedPath, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    LOG_ERROR("Audio", "Failed to open sound file: " + resolvedPath);
    mState.audioStatus = "Failed to open audio";
    return false;
  }

  const std::streamsize size = file.tellg();
  if (size <= 0) {
    LOG_ERROR("Audio", "Audio file is empty: " + resolvedPath);
    mState.audioStatus = "Audio file is empty";
    return false;
  }

  slot.encodedData.resize(static_cast<size_t>(size));
  file.seekg(0, std::ios::beg);
  if (!file.read(reinterpret_cast<char *>(slot.encodedData.data()), size)) {
    LOG_ERROR("Audio", "Failed to read sound file: " + resolvedPath);
    slot.encodedData.clear();
    mState.audioStatus = "Failed to read audio";
    return false;
  }

  ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 0, 0);
  if (isVorbisPath(resolvedPath)) {
    decoderConfig.encodingFormat = ma_encoding_format_vorbis;
  }
  ma_result res = ma_decoder_init_memory(slot.encodedData.data(),
                                         slot.encodedData.size(),
                                         &decoderConfig, &slot.decoder);
  if (res != MA_SUCCESS) {
    LOG_ERROR("Audio", "Failed to decode sound: " + resolvedPath +
                           " (" + std::to_string((int) res) + ")");
    slot.encodedData.clear();
    mState.audioStatus =
        "Failed to decode audio (" + std::to_string((int) res) + "): " +
        resolvedPath;
    return false;
  }
  slot.decoderLoaded = true;

  auto *holder = static_cast<EngineHolder *>(mEngineStorage);
  ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION;
  if (streamed)
    flags |= MA_SOUND_FLAG_STREAM;

  res = ma_sound_init_from_data_source(&holder->engine, &slot.decoder, flags,
                                       nullptr, &slot.sound);
  if (res != MA_SUCCESS) {
    if (!slot.warnedLoad) {
      LOG_ERROR("Audio", "Failed to create sound: " + resolvedPath +
                             " (" + std::to_string((int)res) + ")");
      slot.warnedLoad = true;
      mState.audioStatus =
          "Failed to create audio (" + std::to_string((int)res) + "): " +
          resolvedPath;
    }
    ma_decoder_uninit(&slot.decoder);
    slot.decoderLoaded = false;
    slot.encodedData.clear();
    return false;
  }
  slot.warnedLoad = false;

  ma_sound_set_looping(&slot.sound, looped ? MA_TRUE : MA_FALSE);
  slot.loaded = true;
  slot.path = resolvedPath;
  slot.looped = looped;
  slot.startedLogged = false;
  mState.audioStatus = "Loaded audio";
  LOG_INFO("Audio", "Loaded sound: " + resolvedPath);
  return true;
}

void AudioSubsystem::unloadSound_(ManagedSound &slot) {
  if (slot.loaded) {
    ma_sound_stop(&slot.sound);
    ma_sound_uninit(&slot.sound);
  }
  if (slot.decoderLoaded) {
    ma_decoder_uninit(&slot.decoder);
  }
  slot = ManagedSound{};
}

std::string AudioSubsystem::resolvePath_(const std::string &path) const {
  namespace fs = std::filesystem;
  if (path.empty())
    return {};

  fs::path p(path);
  if (p.is_absolute())
    return p.lexically_normal().string();

  if (fs::exists(p))
    return p.lexically_normal().string();

  const std::string projectResolved = mState.projectConfig.projectPath(path);
  if (fs::exists(projectResolved))
    return fs::path(projectResolved).lexically_normal().string();

  std::string rel = path;
  const std::string assetPrefix = "assets/";
  const std::string assetPrefixCaps = "Assets/";
  if (rel.rfind(assetPrefix, 0) == 0) {
    rel = rel.substr(assetPrefix.size());
  } else if (rel.rfind(assetPrefixCaps, 0) == 0) {
    rel = rel.substr(assetPrefixCaps.size());
  }

  const std::string assetResolved = mState.projectConfig.assetPath(rel);
  if (fs::exists(assetResolved))
    return fs::path(assetResolved).lexically_normal().string();

  static const fs::path kSourceRoot =
      fs::path(__FILE__).parent_path().parent_path().parent_path();
  const fs::path sourceResolved = kSourceRoot / path;
  if (fs::exists(sourceResolved))
    return sourceResolved.lexically_normal().string();

  const fs::path sourceAssetResolved =
      kSourceRoot / "assets" / fs::path(rel);
  if (fs::exists(sourceAssetResolved))
    return sourceAssetResolved.lexically_normal().string();

  return p.lexically_normal().string();
}

void AudioSubsystem::syncAmbient_() {
  const auto &audio = mState.audio;
  if (!audio.enabled || !audio.ambientEnabled || audio.ambientPath.empty()) {
    if (mAmbient->loaded)
      ma_sound_stop(&mAmbient->sound);
    return;
  }

  if (!loadSound_(*mAmbient, audio.ambientPath, true, true))
    return;

  if (!ma_sound_is_playing(&mAmbient->sound)) {
    ma_sound_start(&mAmbient->sound);
    if (!mAmbient->startedLogged) {
      LOG_INFO("Audio", "Started ambient loop: " + mAmbient->path);
      mAmbient->startedLogged = true;
    }
  }
}

void AudioSubsystem::syncFootsteps_() {
  const auto &audio = mState.audio;
  if (!audio.enabled || !audio.footstepsEnabled) {
    mFootstepTimer = 0.0f;
    mFootstepWasMoving = false;
    return;
  }
  if (mFootstepClipPaths.empty())
    refreshFootstepClipPool_();
}

void AudioSubsystem::updateFootstepMotion_(float dt) {
  if (!mAudioAvailable || !mEngineStorage) {
    mFootstepTimer = 0.0f;
    mFootstepWasMoving = false;
    return;
  }

  bool moveIntent = false;
  if (mState.playState == AppState::PlayState::Playing && mState.window != nullptr) {
    moveIntent =
        glfwGetKey(mState.window, GLFW_KEY_W) == GLFW_PRESS ||
        glfwGetKey(mState.window, GLFW_KEY_A) == GLFW_PRESS ||
        glfwGetKey(mState.window, GLFW_KEY_S) == GLFW_PRESS ||
        glfwGetKey(mState.window, GLFW_KEY_D) == GLFW_PRESS;
  }

  const bool shouldStep = moveIntent;
  if (!shouldStep) {
    mFootstepTimer = 0.0f;
    mFootstepWasMoving = false;
    return;
  }

  const bool running =
      mState.window != nullptr &&
      (glfwGetKey(mState.window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
       glfwGetKey(mState.window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
  const float cadence = running
                            ? std::clamp(mState.audio.footstepRunCadence, 0.08f, 0.60f)
                            : std::clamp(mState.audio.footstepWalkCadence, 0.10f, 0.80f);
  if (!mFootstepWasMoving) {
    mFootstepTimer = 0.0f;
  } else {
    mFootstepTimer = std::max(0.0f, mFootstepTimer - dt);
  }

  if (mFootstepTimer > 0.0f) {
    mFootstepWasMoving = true;
    return;
  }

  const std::string resolvedPath = nextFootstepClip_();
  if (resolvedPath.empty() || !std::filesystem::exists(resolvedPath)) {
    mState.audioStatus = "Footstep file missing";
    return;
  }

  playFootstepOneShot_(resolvedPath);
  mFootstepTimer = cadence;
  mFootstepWasMoving = true;
  mState.audioStatus = "Footstep playback active";
}

void AudioSubsystem::applyVolumes_() {
  if (!mAudioAvailable || !mEngineStorage)
    return;

  const auto &audio = mState.audio;
  const float master = (audio.enabled && !audio.mute)
                           ? std::clamp(audio.masterVolume, 0.0f, 1.5f)
                           : 0.0f;
  auto *holder = static_cast<EngineHolder *>(mEngineStorage);
  ma_engine_set_volume(&holder->engine, master);

  if (mAmbient->loaded) {
    ma_sound_set_volume(&mAmbient->sound,
                        master * std::clamp(audio.ambientVolume, 0.0f, 1.5f));
  }
  if (holder->footstepsGroupReady) {
    ma_sound_group_set_volume(
        &holder->footstepsGroup,
        master * std::clamp(audio.footstepVolume, 0.0f, 1.5f));
  }
}

void AudioSubsystem::refreshFootstepClipPool_() {
  namespace fs = std::filesystem;
  mFootstepClipPaths.clear();
  mFootstepClipIndex = 0;

  const fs::path assetRoot = fs::path(mState.projectConfig.assetPath(""));
  if (!fs::exists(assetRoot) || !fs::is_directory(assetRoot))
    return;

  const std::string ambientResolved = resolvePath_(mState.audio.ambientPath);

  for (const auto &entry : fs::directory_iterator(assetRoot)) {
    if (!entry.is_regular_file())
      continue;
    std::string ext = entry.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (ext == ".ogg" || ext == ".oga") {
      const std::string clipPath = entry.path().lexically_normal().string();
      if (!ambientResolved.empty() && clipPath == ambientResolved)
        continue;
      mFootstepClipPaths.push_back(clipPath);
    }
  }
  std::sort(mFootstepClipPaths.begin(), mFootstepClipPaths.end());
}

std::string AudioSubsystem::nextFootstepClip_() {
  if (mFootstepClipPaths.empty())
    refreshFootstepClipPool_();
  if (mFootstepClipPaths.empty())
    return {};
  const size_t idx = mFootstepClipIndex % mFootstepClipPaths.size();
  ++mFootstepClipIndex;
  return mFootstepClipPaths[idx];
}

void AudioSubsystem::playFootstepOneShot_(const std::string &path) {
  if (!mAudioAvailable || !mEngineStorage || path.empty())
    return;
  auto *holder = static_cast<EngineHolder *>(mEngineStorage);
  ma_sound_group *group =
      holder->footstepsGroupReady ? &holder->footstepsGroup : nullptr;
  const ma_result res = ma_engine_play_sound(&holder->engine, path.c_str(), group);
  if (res != MA_SUCCESS) {
    LOG_ERROR("Audio", "Footstep one-shot failed: " + path + " (" +
                           std::to_string((int)res) + ")");
    mState.audioStatus =
        "Footstep one-shot failed (" + std::to_string((int)res) + ")";
  }
}
