#pragma once
#include "Rendering/Shader.h"
#include <glm/glm.hpp>
#include <memory>
#include <random>
#include <vector>

struct FireflyParticle {
  glm::vec3 pos{};
  glm::vec3 vel{};
  float phase = 0.0f;
};

class FireflySystem {
public:
  FireflySystem() = default;
  ~FireflySystem();

  bool init(const char *vertPath, const char *fragPath);
  void shutdown();

  void update(float dt, const glm::vec3 &cameraPos, float nightFactor,
              int targetCount, float radius, float heightMin,
              float heightMax);
  void draw(const glm::mat4 &view, const glm::mat4 &projection, float timeSec,
            float nightFactor, float size, float intensity,
            const glm::vec3 &color);

private:
  void rebuildBuffer_(size_t count);
  void respawn_(FireflyParticle &p, const glm::vec3 &cameraPos, float radius,
                float heightMin, float heightMax);

  std::unique_ptr<Shader> mShader;
  GLuint mVAO = 0;
  GLuint mVBO = 0;
  size_t mCapacity = 0;
  std::vector<FireflyParticle> mParticles;
  std::mt19937 mRng{1337u};
  std::uniform_real_distribution<float> mUnit{-1.0f, 1.0f};
  bool mInitialized = false;
};
