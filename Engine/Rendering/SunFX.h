#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>

class Shader;

struct SunParticle {
  glm::vec3 pos;
  glm::vec3 vel;
  float life = 0.0f;
  float maxLife = 1.0f;
  float size = 0.1f;
};

class SunFX {
public:
  glm::vec3 sunPos = glm::vec3(5.0f, 16.0f, 5.0f);
  // Sun FX defaults (from your UI ranges)
  float sunSize = 5.017f;
  float haloSizeMult = 1.570f;
  float glowStrength = 0.542f;

  // Sun Particles defaults (from your UI ranges)
  float emitRate = 1290.678f;
  int maxParticles = 4136;
  float particleSpeed = 2.199f;
  float particleLife = 0.294f;
  float particleSize = 0.355f;

  // Lighting (directional sun)
  glm::vec3 sunDir =
      glm::normalize(glm::vec3(-0.2f, -1.0f, -0.3f)); // rays travel this way
  glm::vec3 sunColor = glm::vec3(1.0f);
  float lightIntensity = 0.542f;
  float ambientStrength = 0.25f;

  float sunAzimuth = 180.0f;
  float sunElevation = 45.0f;

private:
  float emitCarry = 0.0f;
};
