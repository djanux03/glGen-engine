#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

struct VkAppState;
// Owns presentation only. RifleComponent owns saveable ammo; no global singleton
// or world entities survive Stop and accidentally fire in the editor.
class VkRifleSystem {
public:
  void update(VkAppState &, float dt, bool inPlay, bool running);
  void addFrameInstances(VkAppState &);
  void drawHud(VkAppState &);
  bool manualInput = false; // Lua/hidden regression uses the same simulation.
  bool trigger = false, aim = false, reloadRequested = false;
  float aimBlend() const { return mAim; }
  float shotAge() const { return mShotAge; }
private:
  struct Part { std::string name; uint32_t mesh = UINT32_MAX; glm::vec3 pivot{0}; };
  struct Effect {
    glm::vec3 position{0}, velocity{0}, normal{0,1,0};
    float age=0, spin=0;
    uint32_t parent=0;
    glm::vec3 localPosition{0}, localNormal{0,1,0};
  };
  bool load(VkAppState &);
  std::vector<Part> mParts;
  std::vector<Effect> mCasings, mImpacts;
  uint32_t mCasingMesh=UINT32_MAX, mImpactMesh=UINT32_MAX, mFlashMesh=UINT32_MAX;
  uint32_t mPlayer=0;
  glm::mat4 mRoot{1};
  glm::vec3 mLastPosition{0};
  float mAim=0, mRecoil=0, mRecoilVelocity=0, mTime=0, mShotAge=10;
  float mEquip=0, mHitTime=0, mFov=60;
  float mSwayX=0,mSwayY=0;
  bool mLoaded=false,mVisible=false,mAttempted=false,mHadPosition=false;
  bool mRequireRelease=true;
};
