#pragma once

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace audio {
inline bool isAudioClip(const std::filesystem::path &path) {
  const auto name=path.filename().string();
  if(name.empty() || name.front()=='.') return false; // AppleDouble is not audio.
  auto ext=path.extension().string();
  std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
  return ext==".ogg" || ext==".oga" || ext==".wav";
}

inline std::vector<std::string> footstepClips(const std::filesystem::path &assetRoot,
                                            const std::filesystem::path &configured,
                                            const std::filesystem::path &ambient) {
  namespace fs=std::filesystem;
  std::vector<std::string> clips;
  auto add=[&](const fs::path &p) {
    if(!isAudioClip(p)) return;
    std::error_code samePath;
    if(!ambient.empty() && fs::equivalent(p,ambient,samePath)) return;
    clips.push_back(p.lexically_normal().string());
  };
  std::error_code ec;
  if(!configured.empty()) {
    if(fs::is_regular_file(configured,ec)) add(configured);
    else if(fs::is_directory(configured,ec)) {
      fs::recursive_directory_iterator it(configured,fs::directory_options::skip_permission_denied,ec),end;
      for(;it!=end && !ec;it.increment(ec)) if(it->is_regular_file(ec)) add(it->path());
    }
    // A broken explicit setting must not fall back to unrelated game sounds.
  } else {
    // The old fallback recursively treated *every* asset as a footstep, including
    // gunfire, the ambient loop and macOS metadata. Only use the stock foot bank.
    for(const char *surface:{"Sand","Stone"}) {
      for(int variant=1;variant<=3;++variant) for(char foot:{'L','R'}) {
        const auto p=assetRoot/("Fantozzi-"+std::string(surface)+foot+std::to_string(variant)+".ogg");
        if(fs::is_regular_file(p,ec)) add(p);
      }
      if(!clips.empty()) return clips; // Softer sand steps suit the woodland ground.
    }
  }
  std::sort(clips.begin(),clips.end());
  clips.erase(std::unique(clips.begin(),clips.end()),clips.end());
  return clips;
}

// Advance from distance actually travelled on the ground, not WASD intent.
// A half stride delays the first contact so rapid key taps cannot spam sounds.
struct FootstepClock {
  float distance=0;
  bool moving=false;
  void reset() { distance=0;moving=false; }
  bool tick(float dt,float travelled,bool active,float speed,float cadence) {
    if(!active || !std::isfinite(dt) || dt<=0 || !std::isfinite(travelled) ||
       travelled<=.0001f || !std::isfinite(speed) || speed<=0 ||
       !std::isfinite(cadence) || cadence<=0 || travelled>speed*dt*3+.5f) {
      reset();return false;
    }
    const float stride=speed*cadence;
    if(!moving) {distance=stride*.5f;moving=true;}
    distance+=travelled;
    if(distance<stride) return false;
    // Keep phase at low frame rates, but never burst overdue contacts after a hitch.
    distance=std::fmod(distance,stride);
    return true;
  }
};
} // namespace audio
