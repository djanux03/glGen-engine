#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <memory>
#include <string>

class Shader; // forward declare is fine

class HDRSky {
public:
  HDRSky();
  ~HDRSky(); // <-- declare, don't inline default

  void setYaw01(float yaw01);

  void setRotationDegrees(const glm::vec3 &eulerDeg);
  glm::vec3 mSkyRotDeg = glm::vec3(0.0f);

  bool init(const std::string &hdrPath, const std::string &vertPath,
            const std::string &fragPath);
  void shutdown();
  void draw(const glm::mat4 &view, const glm::mat4 &projection, float exposure,
            float gamma, const glm::vec3 &sunDir, const glm::vec3 &sunColor,
            float sunSize, float timeSec);

  void setSolidSky(bool on) { mUseSolidSky = on; }
  void setSkyColors(const glm::vec3 &horizon, const glm::vec3 &top) {
    mSkyHorizon = horizon;
    mSkyTop = top;
  }

  // Lightweight procedural sky clouds
  bool skyCloudsEnabled = true;
  float skyCloudScale = 2.4f;
  float skyCloudCoverage = 0.58f;
  float skyCloudDensity = 0.65f;
  float skyCloudSoftness = 0.18f;
  float skyCloudSpeed = 0.008f;
  glm::vec3 skyCloudColor = glm::vec3(0.97f, 0.98f, 1.0f);

  // Richer sun look (more realistic defaults)
  float sunDiscIntensity = 8.0f;
  float sunHaloIntensity = 0.6f;
  float sunRaysIntensity = 0.2f;

  // Night visuals
  float nightFactor = 0.0f;           // 0=day, 1=night
  float starIntensity = 0.4f;
  float milkyWayIntensity = 0.25f;
  glm::vec3 nightHorizonGlow = glm::vec3(0.08f, 0.12f, 0.20f);
  float nightDitherStrength = 0.004f;

private:
  void createFullscreenQuad_();

  std::unique_ptr<Shader> mShader;
  GLuint mHDRTex = 0;

  float mYaw01 = 0.0f;

  GLuint mVAO = 0;
  GLuint mVBO = 0;

  bool mUseSolidSky = false;
  glm::vec3 mSkyHorizon = glm::vec3(0.55f, 0.72f, 0.95f);
  glm::vec3 mSkyTop = glm::vec3(0.22f, 0.42f, 0.82f);
};
