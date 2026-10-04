#include "VkRifleSystem.h"
#include "VkAppState.h"
#include "Assets/MeshParse.h"
#include "Assets/MeshPrimitives.h"
#include "VulkanRenderer.h"
#include "subsystems/VkAudioSubsystem.h"
#include "subsystems/VkTerrainSubsystem.h"
#include "Keyboard.h"
#include "Mouse.h"
#include "Logger.h"
#include "json.hpp"
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>

namespace {
float smooth(float a,float b,float x) {
  x=glm::clamp((x-a)/(b-a),0.f,1.f);return x*x*(3-2*x);
}
glm::mat4 orient(const glm::vec3 &position,const glm::vec3 &normal) {
  const auto n=glm::normalize(normal);
  auto right=glm::normalize(glm::cross(std::abs(n.y)>.95f?glm::vec3(1,0,0):glm::vec3(0,1,0),n));
  glm::mat4 m(1);m[0]=glm::vec4(right,0);m[1]=glm::vec4(n,0);
  m[2]=glm::vec4(glm::cross(right,n),0);m[3]=glm::vec4(position,1);return m;
}
}

bool VkRifleSystem::load(VkAppState &s) {
  if(mLoaded)return true;
  if(mAttempted||!s.renderer)return false;
  mAttempted=true;
  const std::string path=s.assetDir+"/weapons/ak47/";
  std::ifstream file(path+"rig.json");
  const auto rig=nlohmann::json::parse(file,nullptr,false);
  if(rig.is_discarded()) { LOG_ERROR("Rifle","AK47 rig manifest missing");return false; }
  for(const auto &p:rig["parts"]) {
    auto data=parseMeshGLTF(path+p["file"].get<std::string>());
    if(!data)return false;
    Part part;part.name=p["name"];
    part.pivot={p["pivot"][0],p["pivot"][1],p["pivot"][2]};
    part.mesh=s.renderer->createMeshFromData(*data,"weapon://ak47/"+part.name);
    if(part.mesh==UINT32_MAX)return false;
    mParts.push_back(part);
  }
  auto casing=makePrimitiveMesh("__primitive_cylinder");
  casing->submeshes[0].material.baseColor={.62f,.39f,.10f,1};
  casing->submeshes[0].material.metallic=.85f;casing->submeshes[0].material.roughness=.3f;
  mCasingMesh=s.renderer->createMeshFromData(*casing,"weapon://casing");
  auto impact=makePrimitiveMesh("__primitive_plane");
  impact->submeshes[0].material.baseColor={.045f,.033f,.023f,1};
  mImpactMesh=s.renderer->createMeshFromData(*impact,"weapon://impact");
  auto flash=makePrimitiveMesh("__primitive_cone");
  auto &fm=flash->submeshes[0].material;
  fm.baseColor={.65f,.25f,.025f,1};fm.emissiveColor={12,4.5f,.7f};fm.emissiveStrength=1;
  mFlashMesh=s.renderer->createMeshFromData(*flash,"weapon://flash");
  s.renderer->growScene();
  if(s.audioSubsystem) {
    for(int i=0;i<3;++i)s.audioSubsystem->warmOneShot(path+"audio/shot"+std::to_string(i)+".wav",4);
    s.audioSubsystem->warmOneShot(path+"audio/reload.wav",1);
    s.audioSubsystem->warmOneShot(path+"audio/charge.wav",1);
  }
  mLoaded=true;LOG_INFO("Rifle","AK47 PBR rig and cached audio ready");return true;
}

void VkRifleSystem::update(VkAppState &s,float dt,bool inPlay,bool running) {
  auto &reg=s.scene.registry();const uint32_t player=s.gameplay.playerId;
  const bool wasVisible=mVisible;
  mVisible=inPlay&&reg.valid(player)&&reg.has<RifleComponent>(player)&&
    reg.has<TransformComponent>(player)&&reg.get<RifleComponent>(player).state.enabled;
  if(!mVisible) {
    if(wasVisible&&s.renderer)s.renderer->params().fovDeg=mFov;
    mPlayer=0;mHadPosition=false;mCasings.clear();mImpacts.clear();return;
  }
  if(!load(s))return;
  auto &w=reg.get<RifleComponent>(player).state;
  auto &tr=reg.get<TransformComponent>(player);
  if(mPlayer!=player||!wasVisible) {
    mPlayer=player;mFov=s.renderer->params().fovDeg;
    mEquip=0;mAim=0;mRecoil=0;mRecoilVelocity=0;mShotAge=10;
    mHadPosition=false;mRequireRelease=true;w.triggerWasDown=true;
  }
  dt=running?glm::clamp(dt,0.f,.25f):0.f;
  const bool sprint=!manualInput&&(Keyboard::key(GLFW_KEY_LEFT_SHIFT)||Keyboard::key(GLFW_KEY_RIGHT_SHIFT))&&
    (Keyboard::key(GLFW_KEY_W)||Keyboard::key(GLFW_KEY_A)||Keyboard::key(GLFW_KEY_S)||Keyboard::key(GLFW_KEY_D));
  const bool firing=manualInput?trigger:Mouse::button(GLFW_MOUSE_BUTTON_LEFT);
  // Clicking Play must not discharge the weapon as the mouse is captured.
  if(!firing)mRequireRelease=false;
  const bool aiming=(manualInput?aim:Mouse::button(GLFW_MOUSE_BUTTON_RIGHT))&&!w.reloading()&&!sprint;
  if(!manualInput&&Keyboard::keyWentDown(GLFW_KEY_B)&&running)w.automatic=!w.automatic;
  if(running&&(reloadRequested||(!manualInput&&Keyboard::keyWentDown(GLFW_KEY_R)))) {
    if(w.reload()&&s.audioSubsystem)s.audioSubsystem->playOneShot(s.assetDir+"/weapons/ak47/audio/reload.wav",.7f);
    reloadRequested=false;
  }
  const float previousReload=w.reloadRemaining;
  const float oldRecoil=mRecoil;
  // Substeps keep the critically damped recoil spring stable during hitches.
  float remaining=dt;
  while(remaining>0) {
    const float step=std::min(remaining,1.f/120.f);
    mRecoilVelocity+=(-170.f*mRecoil-26.f*mRecoilVelocity)*step;
    mRecoil+=mRecoilVelocity*step;remaining-=step;
  }
  mTime+=dt;mEquip=std::min(1.f,mEquip+dt*2.6f);mShotAge+=dt;
  mHitTime=std::max(0.f,mHitTime-dt);
  mAim+=(float(aiming)-mAim)*(1-std::exp(-dt*16.f));
  const int shots=w.tick(dt,firing&&!sprint&&!mRequireRelease,running);
  if(previousReload>0 && w.emptyReload && previousReload>.7f && w.reloadRemaining<=.7f && s.audioSubsystem)
    s.audioSubsystem->playOneShot(s.assetDir+"/weapons/ak47/audio/charge.wav",.65f);
  const auto yaw=glm::radians(tr.rotation.y),pitch=glm::radians(tr.rotation.x);
  glm::vec3 forward=glm::normalize(glm::vec3(-std::sin(yaw)*std::cos(pitch),std::sin(pitch),-std::cos(yaw)*std::cos(pitch)));
  const auto right=glm::normalize(glm::cross(forward,glm::vec3(0,1,0)));
  const auto up=glm::normalize(glm::cross(right,forward));
  for(int i=0;i<shots;++i) {
    // Deterministic cone spread; muzzle animation cannot change the centre ray.
    const float phase=float(w.shotsFired-shots+i)*2.39996323f;
    const float spread=(mAim>.9f?.0004f:.004f)*std::sin(phase*1.73f);
    const auto direction=glm::normalize(forward+right*std::cos(phase)*spread+up*std::sin(phase)*spread);
    s.playerInteraction.fireRifle(s,tr.position,direction);
    if(s.gameplay.debug.debugGameplayHit) {
      Effect effect;effect.position=s.gameplay.debug.debugGameplayHitPosition;
      // Reuse the authoritative hit, including the analytic terrain normal;
      // a second raycast missed unloaded heightfields and tilted their marks.
      effect.normal=s.gameplay.debug.debugGameplayHitNormal;
      effect.parent=s.gameplay.debug.debugGameplayHitId;
      if(effect.parent&&reg.has<TransformComponent>(effect.parent)) {
        const auto matrix=reg.get<TransformComponent>(effect.parent).getMatrix();
        effect.localPosition=glm::vec3(glm::inverse(matrix)*glm::vec4(effect.position,1));
        effect.localNormal=glm::transpose(glm::mat3(matrix))*effect.normal;
      }
      if(mImpacts.size()>=24)mImpacts.erase(mImpacts.begin());
      mImpacts.push_back(effect);
      if(effect.parent&&reg.has<DestructibleComponent>(effect.parent))mHitTime=.15f;
    }
    if(mCasings.size()>=32)mCasings.erase(mCasings.begin());
    Effect casing;casing.position=tr.position+right*.2f+forward*.35f-up*.15f;
    casing.velocity=right*2.4f+up*1.3f+forward*.4f;casing.spin=phase;
    mCasings.push_back(casing);
    if(s.audioSubsystem)s.audioSubsystem->playOneShot(s.assetDir+"/weapons/ak47/audio/shot"+std::to_string((w.shotsFired-shots+i)%3)+".wav",.55f);
    mRecoil=std::min(2.3f,mRecoil+.65f);mRecoilVelocity+=4.f;
    tr.rotation.y+=std::sin(phase)*.12f*(1-mAim*.5f);mShotAge=0;
  }
  tr.rotation.x=glm::clamp(tr.rotation.x+(mRecoil-oldRecoil)*.9f,-89.f,89.f);
  s.renderer->params().fovDeg=glm::mix(mFov,mFov*.72f,mAim);
  float speed=0;
  if(mHadPosition&&dt>0)speed=glm::length(glm::vec2(tr.position.x-mLastPosition.x,tr.position.z-mLastPosition.z))/dt;
  mLastPosition=tr.position;mHadPosition=true;
  if(running) {
    mSwayX=glm::mix(mSwayX,glm::clamp(float(Mouse::getDX())*.0004f,-.016f,.016f),1-std::exp(-dt*10));
    mSwayY=glm::mix(mSwayY,glm::clamp(float(Mouse::getDY())*.0004f,-.016f,.016f),1-std::exp(-dt*10));
  }
  const float viewYaw=glm::radians(tr.rotation.y),viewPitch=glm::radians(tr.rotation.x);
  const auto viewForward=glm::normalize(glm::vec3(-std::sin(viewYaw)*std::cos(viewPitch),std::sin(viewPitch),-std::cos(viewYaw)*std::cos(viewPitch)));
  const auto viewRight=glm::normalize(glm::cross(viewForward,glm::vec3(0,1,0)));
  const auto viewUp=glm::normalize(glm::cross(viewRight,viewForward));
  glm::mat4 basis(1);basis[0]=glm::vec4(viewRight,0);basis[1]=glm::vec4(viewUp,0);basis[2]=glm::vec4(-viewForward,0);basis[3]=glm::vec4(tr.position,1);
  auto offset=glm::mix(glm::vec3(.19f,-.255f,-.12f),glm::vec3(0,-.146f,-.18f),mAim);
  const float bob=std::min(speed/5.f,1.5f)*(1-mAim*.9f);
  offset+=glm::vec3(std::sin(mTime*9)*.004f*bob,std::abs(std::cos(mTime*9))*.004f*bob,0);
  offset+=glm::vec3(-mSwayX,-mSwayY+mRecoil*.003f,mRecoil*.028f);
  offset.y-=.3f*(1-smooth(0,1,mEquip));
  const float reload=w.reloadProgress();
  const float tilt=w.reloading()?smooth(0,.15f,reload)*(1-smooth(.82f,1,reload)):0;
  // Bring the receiver and magazine into view for the reload. Leaving the
  // hip pose in place hid the entire magazine exchange below the viewport.
  offset=glm::mix(offset,glm::vec3(.10f,.07f,-.68f),tilt);
  mRoot=glm::translate(basis,offset);
  mRoot=glm::rotate(mRoot,glm::radians(-10.f*tilt-(sprint?22.f:0.f)),glm::vec3(0,0,1));
  mRoot=glm::rotate(mRoot,glm::radians(60.f*tilt),glm::vec3(0,1,0));
  mRoot=glm::rotate(mRoot,glm::radians(mRecoil*2.2f-(sprint?15.f:0.f)),glm::vec3(1,0,0));
  for(auto &c:mCasings) {
    c.age+=dt;c.velocity.y-=9.81f*dt;c.position+=c.velocity*dt;
    if(s.terrainSubsystem&&s.terrainSubsystem->hasTerrain()) {
      const float y=s.terrainSubsystem->heightAt({c.position.x,c.position.z})+.009f;
      if(c.position.y<y){c.position.y=y;c.velocity*=.3f;c.velocity.y=std::abs(c.velocity.y);}
    }
  }
  for(auto &e:mImpacts)e.age+=dt;
  mCasings.erase(std::remove_if(mCasings.begin(),mCasings.end(),[](auto &c){return c.age>2.5f;}),mCasings.end());
  mImpacts.erase(std::remove_if(mImpacts.begin(),mImpacts.end(),[&](auto &c){return c.age>20||(c.parent&&!reg.valid(c.parent));}),mImpacts.end());
}

void VkRifleSystem::addFrameInstances(VkAppState &s) {
  if(!mVisible||!mLoaded||!s.scene.registry().has<RifleComponent>(mPlayer))return;
  const auto &w=s.scene.registry().get<RifleComponent>(mPlayer).state;
  const float t=w.reloadProgress();
  const float out=w.reloading()?smooth(.14f,.31f,t)*(1-smooth(.52f,.70f,t)):0;
  for(const auto &part:mParts) {
    auto model=glm::translate(mRoot,part.pivot);
    if(part.name=="MAGAZINE") {
      model=glm::translate(model,glm::vec3(-.035f,-.16f,.045f)*out);
      model=glm::rotate(model,glm::radians(28.f*out),glm::vec3(1,0,0));
    } else if(part.name=="hand_left") {
      // The support hand follows the magazine out and back, then reaches for
      // the bolt on an empty reload. All geometry is shared between frames.
      const float reach=w.reloading()?smooth(.07f,.18f,t)*(1-smooth(.72f,.91f,t)):0;
      auto handOffset=glm::vec3(.017f,-.055f,.12f)*reach+glm::vec3(-.035f,-.16f,.045f)*out;
      if(w.emptyReload&&w.reloading()) {
        const float bolt=smooth(.72f,.79f,t)*(1-smooth(.87f,.94f,t));
        handOffset=glm::mix(handOffset,glm::vec3(.08f,.045f,.12f),bolt);
      }
      model=glm::translate(model,handOffset);
      model=glm::rotate(model,glm::radians(28.f*out),glm::vec3(1,0,0));
    } else if(part.name=="SLIDE") {
      float cycle= mShotAge<.07f ? std::sin(mShotAge/.07f*3.14159265f):0;
      if(w.reloading()&&w.emptyReload)cycle=std::max(cycle,smooth(.73f,.80f,t)*(1-smooth(.84f,.90f,t)));
      model=glm::translate(model,glm::vec3(0,0,.065f)*cycle);
    } else if(part.name=="TRIGGER") {
      model=glm::rotate(model,glm::radians(mShotAge<.09f?12.f:0.f),glm::vec3(1,0,0));
    } else if(part.name=="SELECTOR") {
      model=glm::rotate(model,glm::radians(w.automatic?0.f:16.f),glm::vec3(1,0,0));
    }
    const uint32_t armFlags=part.name=="hand_left"?(1u<<15):
      (part.name=="hand_right"?((1u<<15)|(1u<<16)):0u);
    s.renderer->addInstance(part.mesh,model,false,nullptr,false,true,armFlags);
  }
  if(mShotAge<.045f) {
    auto model=glm::translate(mRoot,glm::vec3(0,.0957f,-.73f));
    model=glm::rotate(model,glm::radians(-90.f),glm::vec3(1,0,0));
    model=glm::scale(model,glm::vec3(.023f,.12f,.023f));
    s.renderer->addInstance(mFlashMesh,model,false,nullptr,false,true);
  }
  for(const auto &c:mCasings) {
    auto model=glm::translate(glm::mat4(1),c.position);
    model=glm::rotate(model,c.spin+c.age*18,glm::vec3(.6f,.3f,.8f));
    model=glm::scale(model,glm::vec3(.008f,.035f,.008f));
    s.renderer->addInstance(mCasingMesh,model,false,nullptr,false);
  }
  for(const auto &e:mImpacts) {
    glm::vec3 pos=e.position,n=e.normal;
    if(e.parent&&s.scene.registry().has<TransformComponent>(e.parent)) {
      const auto &reg=s.scene.registry();
      if(reg.has<MeshComponent>(e.parent)&&!reg.get<MeshComponent>(e.parent).visible)continue;
      const auto matrix=reg.get<TransformComponent>(e.parent).getMatrix();
      pos=glm::vec3(matrix*glm::vec4(e.localPosition,1));n=glm::normalize(glm::transpose(glm::inverse(glm::mat3(matrix)))*e.localNormal);
    }
    s.renderer->addInstance(mImpactMesh,glm::scale(orient(pos+n*.004f,n),glm::vec3(.035f)),false,nullptr,false);
  }
}

void VkRifleSystem::drawHud(VkAppState &s) {
  if(!mVisible||!s.scene.registry().has<RifleComponent>(mPlayer))return;
  const auto &w=s.scene.registry().get<RifleComponent>(mPlayer).state;
  auto *draw=ImGui::GetForegroundDrawList();const auto *v=ImGui::GetMainViewport();
  const ImVec2 c(v->Pos.x+v->Size.x*.5f,v->Pos.y+v->Size.y*.5f);
  if(mAim<.5f&&!w.reloading()) {
    const float gap=5+mRecoil*8;
    for(const auto d:{ImVec2(1,0),ImVec2(-1,0),ImVec2(0,1),ImVec2(0,-1)})
      draw->AddLine({c.x+d.x*gap,c.y+d.y*gap},{c.x+d.x*(gap+4),c.y+d.y*(gap+4)},IM_COL32(225,225,215,170),1);
  }
  if(mHitTime>0)for(int x:{-1,1})for(int y:{-1,1})
    draw->AddLine({c.x+x*5.f,c.y+y*5.f},{c.x+x*10.f,c.y+y*10.f},IM_COL32(245,215,155,220),2);
  char text[128];std::snprintf(text,sizeof(text),"AK-47   %s\n%02d / %03d%s",w.automatic?"AUTO":"SEMI",w.magazine,w.reserve,w.reloading()?"   RELOADING":"");
  const ImVec2 p(v->Pos.x+v->Size.x-270,v->Pos.y+v->Size.y-86);
  draw->AddRectFilled({p.x-14,p.y-10},{p.x+242,p.y+48},IM_COL32(10,13,12,145),4);
  draw->AddText(p,IM_COL32(230,233,225,235),text);
  draw->AddText({v->Pos.x+24,v->Pos.y+v->Size.y-30},IM_COL32(235,235,225,170),"LMB Fire   RMB Aim   R Reload   B Fire mode");
}
