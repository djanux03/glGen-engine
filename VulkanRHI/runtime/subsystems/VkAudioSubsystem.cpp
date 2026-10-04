#include "VkAudioSubsystem.h"

// miniaudio (included below) does `#include <windows.h>` itself on Windows,
// AFTER `stb_vorbis.c` -- which #defines single-letter macros L/C/R for
// channel-mapping constants and never undefines them. Those collide with
// bitfield member names in <winnt.h> (RUNTIME_FUNCTION's `R`/`L`/`C`/`H`
// fields), corrupting the parse. The old GL AudioSubsystem.cpp got a clean
// windows.h parse "for free" via AppState.h's glad/GLFW include chain, which
// runs (and fully parses+guards windows.h) before stb_vorbis.c ever defines
// those macros. VkAppState.h has no such chain, so pull in windows.h here
// first, before anything else, so its include guard makes miniaudio's later
// `#include <windows.h>` a no-op. NOMINMAX/WIN32_LEAN_AND_MEAN are already
// project-wide compile definitions (root CMakeLists.txt), so a plain
// #include is all that's needed here.
#if defined(_WIN32)
#include <windows.h>
#endif

#include "VkAppState.h"
#include "ECS/Components.h"
#include "Keyboard.h"
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
#include "AudioVoice.h"

namespace {
struct EngineHolder {
  ma_engine engine{};
  ma_sound_group footstepsGroup{};
  bool footstepsGroupReady = false;
};

} // namespace

struct VkAudioSubsystem::ManagedSound : audio::Voice {
  std::string path;
  std::string requestedPath;
  bool looped = false;
  bool warnedMissing = false;
  bool warnedLoad = false;
  bool startedLogged = false;
};

VkAudioSubsystem::VkAudioSubsystem(VkAppState &state) : mState(state) {
  mAmbient = new ManagedSound();
}

VkAudioSubsystem::~VkAudioSubsystem() {
  shutdown();
  delete mAmbient;
}

bool VkAudioSubsystem::initialize() {
  if(std::getenv("GLGEN_MUTE"))mSettings.mute=true;
  const bool ok=initEngine_();
  applyVolumes_();
  if(mAudioAvailable) {syncFootsteps_();syncAmbient_();applyVolumes_();}
  return ok;
}

void VkAudioSubsystem::shutdown() {
  for(auto &[path,voices]:mOneShots)
    for(auto *voice:voices){unloadSound_(*voice);delete voice;}
  mOneShots.clear();mOneShotCursor.clear();
  for(auto &[path,voices]:mFootstepVoices)
    for(auto *voice:voices) delete voice;
  mFootstepVoices.clear();mFootstepPoolReady=false;
  mConfiguredFootstepPath.clear();
  if (mAmbient)
    unloadSound_(*mAmbient);

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
  mBackendAvailable = false;
  mStatus = "Audio offline";
  mHasLastPlayerPos = false;
  mLastPlayerId = 0;
  mFootstepClock.reset();
  mFootstepClipPaths.clear();
  mFootstepClipIndex = 0;
}

void VkAudioSubsystem::update(float dt, const glm::vec3 &listenerPos,
                              const glm::vec3 &listenerForward) {
  if (!mAudioAvailable || !mEngineStorage)
    return;

  auto *holder = static_cast<EngineHolder *>(mEngineStorage);
  mBackendAvailable = true;
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

void VkAudioSubsystem::playTestFootstep() {
  if (!mAudioAvailable) {
    LOG_ERROR("Audio", "Audio backend unavailable for footstep test");
    mBackendAvailable = false;
    mStatus = "Audio backend unavailable";
    return;
  }
  syncFootsteps_();
  const std::string resolvedPath = nextFootstepClip_();
  if (resolvedPath.empty() || !std::filesystem::exists(resolvedPath)) {
    LOG_ERROR("Audio", "Footstep test clip not found");
    mStatus = "Footstep test file missing";
    return;
  }
  playFootstepOneShot_(resolvedPath);
  mStatus = "Test playback triggered";
  LOG_INFO("Audio", "Manual footstep test triggered: " + resolvedPath);
}

// Decode and create bounded voices once. Opening/decoding a WAV on every
// automatic shot would hitch the main thread precisely when aiming matters.
void VkAudioSubsystem::warmOneShot(const std::string &path,int voices) {
  if(!mAudioAvailable||mOneShots.count(path))return;
  auto &pool=mOneShots[path];
  for(int i=0;i<std::clamp(voices,1,16);++i) {
    auto *voice=new ManagedSound();
    if(loadSound_(*voice,path,false,false))pool.push_back(voice);
    else {delete voice;break;}
  }
}
void VkAudioSubsystem::playOneShot(const std::string &path,float volume) {
  if(!mAudioAvailable)return;
  warmOneShot(path);
  auto &pool=mOneShots[path];if(pool.empty())return;
  auto *voice=pool[mOneShotCursor[path]++ % pool.size()];
  voice->restart(std::clamp(volume,0.f,1.f));
}

bool VkAudioSubsystem::initEngine_() {
  if (mInitialized)
    return true;

  auto *holder = new EngineHolder();
  ma_result res = ma_engine_init(nullptr, &holder->engine);
  if (res != MA_SUCCESS) {
    delete holder;
    LOG_WARN("Audio", "Audio device initialization failed; continuing without sound");
    mInitialized = true;
    mAudioAvailable = false;
    mBackendAvailable = false;
    mStatus =
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
  mBackendAvailable = true;
  mStatus = "Audio initialized";
  LOG_INFO("Audio", "Audio subsystem initialized");
  return true;
}

bool VkAudioSubsystem::loadSound_(ManagedSound &slot, const std::string &path,
                                  bool looped, bool streamed) {
  if (!mAudioAvailable || !mEngineStorage)
    return false;

  // Keep a failed path cached as well. Retrying a missing or corrupt ambient
  // file every frame flooded the log and repeatedly stalled the main thread.
  if(slot.requestedPath==path && slot.looped==looped) {
    if(slot.loaded) return true;
    if(slot.warnedMissing || slot.warnedLoad) return false;
  }
  unloadSound_(slot);
  slot.requestedPath=path;slot.looped=looped;

  const std::string resolvedPath = resolvePath_(path);
  if (resolvedPath.empty() || !std::filesystem::exists(resolvedPath)) {
    if (!slot.warnedMissing) {
      LOG_ERROR("Audio", "Sound file not found: " + path);
      slot.warnedMissing = true;
      mStatus = "Missing audio file";
    }
    slot.warnedMissing = true;
    return false;
  }
  slot.warnedMissing = false;

  auto *holder = static_cast<EngineHolder *>(mEngineStorage);
  const ma_result res=slot.load(holder->engine,resolvedPath,looped,streamed);
  if(res!=MA_SUCCESS) {
    if(!slot.warnedLoad) LOG_ERROR("Audio","Failed to load sound: "+resolvedPath+
                                  " ("+std::to_string(int(res))+")");
    slot.warnedLoad=true;
    mStatus="Failed to load audio: "+resolvedPath;
    return false;
  }
  slot.warnedLoad = false;
  slot.loaded = true;
  slot.path = resolvedPath;
  slot.looped = looped;
  slot.startedLogged = false;
  mStatus = "Loaded audio";
  LOG_INFO("Audio", "Loaded sound: " + resolvedPath);
  return true;
}

void VkAudioSubsystem::unloadSound_(ManagedSound &slot) {
  slot.unload();
  slot.path.clear();slot.requestedPath.clear();slot.looped=false;slot.startedLogged=false;
  slot.warnedMissing=false;slot.warnedLoad=false;
}

std::string VkAudioSubsystem::resolvePath_(const std::string &path) const {
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

  const fs::path runtimeAsset=fs::path(mState.assetDir)/rel;
  if(fs::exists(runtimeAsset)) return runtimeAsset.lexically_normal().string();

  const std::string assetResolved = mState.projectConfig.assetPath(rel);
  if (fs::exists(assetResolved))
    return fs::path(assetResolved).lexically_normal().string();

  static const fs::path kSourceRoot =
      fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
  const fs::path sourceResolved = kSourceRoot / path;
  if (fs::exists(sourceResolved))
    return sourceResolved.lexically_normal().string();

  const fs::path sourceAssetResolved =
      kSourceRoot / "assets" / fs::path(rel);
  if (fs::exists(sourceAssetResolved))
    return sourceAssetResolved.lexically_normal().string();

  return p.lexically_normal().string();
}

void VkAudioSubsystem::syncAmbient_() {
  const auto &audio = mSettings;
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

void VkAudioSubsystem::syncFootsteps_() {
  if(!mFootstepPoolReady || mConfiguredFootstepPath!=mSettings.footstepPath)
    refreshFootstepClipPool_();
}

void VkAudioSubsystem::updateFootstepMotion_(float dt) {
  auto &reg=mState.scene.registry();
  const uint32_t player=mState.gameplay.playerId;
  if(!player || !reg.has<TransformComponent>(player)) {
    mHasLastPlayerPos=false;mLastPlayerId=0;mFootstepClock.reset();return;
  }
  const auto pos=reg.get<TransformComponent>(player).position;
  if(!mHasLastPlayerPos || player!=mLastPlayerId) {
    mLastPlayerPos=pos;mHasLastPlayerPos=true;mLastPlayerId=player;
    mFootstepClock.reset();return;
  }
  const glm::vec2 travel(pos.x-mLastPlayerPos.x,pos.z-mLastPlayerPos.z);
  mLastPlayerPos=pos;
  // Match the controller's input, not GLFW's independent key state. Sound
  // follows real motion after physics, and cannot play in midair or flight.
  const bool running=Keyboard::key(GLFW_KEY_LEFT_SHIFT)||Keyboard::key(GLFW_KEY_RIGHT_SHIFT);
  const float speed=5.f*(running?std::max(mState.input.runMult,1.f):1.f);
  const float cadence=running?std::clamp(mSettings.footstepRunCadence,.08f,.60f)
                             :std::clamp(mSettings.footstepWalkCadence,.10f,.80f);
  const bool active=mSettings.enabled && !mSettings.mute && mSettings.footstepsEnabled &&
      mState.playState==VkAppState::PlayState::Playing && !mState.input.creativeFlight &&
      mState.playerController.grounded();
  if(!mFootstepClock.tick(dt,glm::length(travel),active,speed,cadence)) return;
  const auto path=nextFootstepClip_();
  if(path.empty()) return;
  playFootstepOneShot_(path);
  mStatus="Footstep playback active";
}

void VkAudioSubsystem::applyVolumes_() {
  if (!mAudioAvailable || !mEngineStorage)
    return;

  auto *holder = static_cast<EngineHolder *>(mEngineStorage);
  audio::applyVolumes(holder->engine,mAmbient,
      holder->footstepsGroupReady?&holder->footstepsGroup:nullptr,mSettings);
}

void VkAudioSubsystem::refreshFootstepClipPool_() {
  for(auto &[path,voices]:mFootstepVoices) for(auto *voice:voices) delete voice;
  mFootstepVoices.clear();mFootstepClipPaths.clear();mFootstepClipIndex=0;
  mFootstepClock.reset();
  mConfiguredFootstepPath=mSettings.footstepPath;
  mFootstepPoolReady=true; // Empty/missing banks must not rescan the disk every frame.
  const auto root=mState.assetDir.empty()?mState.projectConfig.assetPath(""):mState.assetDir;
  const auto configured=mSettings.footstepPath.empty()?std::string{}:resolvePath_(mSettings.footstepPath);
  const auto clips=audio::footstepClips(root,configured,resolvePath_(mSettings.ambientPath));
  auto *holder=static_cast<EngineHolder*>(mEngineStorage);
  for(const auto &path:clips) {
    auto &voices=mFootstepVoices[path];
    // Two voices permit a natural tail, with a strict upper bound on overlap.
    for(int i=0;i<2;++i) {
      auto *voice=new ManagedSound();
      const auto res=voice->load(holder->engine,path,false,false,
          holder->footstepsGroupReady?&holder->footstepsGroup:nullptr);
      if(res!=MA_SUCCESS) {delete voice;break;}
      voice->path=path;voices.push_back(voice);
    }
    if(!voices.empty()) mFootstepClipPaths.push_back(path);
    else LOG_WARN("Audio","Skipping unreadable footstep: "+path);
  }
  LOG_INFO("Audio","Cached "+std::to_string(mFootstepClipPaths.size())+" footstep clips");
}

std::string VkAudioSubsystem::nextFootstepClip_() {
  if (!mFootstepPoolReady)
    refreshFootstepClipPool_();
  if (mFootstepClipPaths.empty())
    return {};
  const size_t idx = mFootstepClipIndex % mFootstepClipPaths.size();
  ++mFootstepClipIndex;
  return mFootstepClipPaths[idx];
}

void VkAudioSubsystem::playFootstepOneShot_(const std::string &path) {
  if(!mAudioAvailable || !mSettings.enabled || mSettings.mute || !mSettings.footstepsEnabled) return;
  auto found=mFootstepVoices.find(path);
  if(found==mFootstepVoices.end() || found->second.empty()) return;
  auto &voices=found->second;
  auto *voice=voices.front();
  for(auto *candidate:voices) if(!ma_sound_is_playing(&candidate->sound)) {voice=candidate;break;}
  auto *holder=static_cast<EngineHolder*>(mEngineStorage);
  const auto res=voice->restart(holder->footstepsGroupReady?1.f:
      std::clamp(mSettings.footstepVolume,0.f,1.5f));
  if(res!=MA_SUCCESS) {
    LOG_ERROR("Audio","Footstep playback failed: "+path+" ("+std::to_string(int(res))+")");
    mStatus="Footstep playback failed";
  }
}
