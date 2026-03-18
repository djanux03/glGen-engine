#include "FireflySystem.h"
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>

FireflySystem::~FireflySystem() { shutdown(); }

bool FireflySystem::init(const char *vertPath, const char *fragPath) {
  mShader = std::make_unique<Shader>(vertPath, fragPath);
  if (!mShader)
    return false;

  glGenVertexArrays(1, &mVAO);
  glGenBuffers(1, &mVBO);
  glBindVertexArray(mVAO);
  glBindBuffer(GL_ARRAY_BUFFER, mVBO);
  glBufferData(GL_ARRAY_BUFFER, 1, nullptr, GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3),
                        (void *)0);
  glBindVertexArray(0);
  mInitialized = true;
  return true;
}

void FireflySystem::shutdown() {
  if (mVBO) {
    glDeleteBuffers(1, &mVBO);
    mVBO = 0;
  }
  if (mVAO) {
    glDeleteVertexArrays(1, &mVAO);
    mVAO = 0;
  }
  mShader.reset();
  mParticles.clear();
  mCapacity = 0;
  mInitialized = false;
}

void FireflySystem::rebuildBuffer_(size_t count) {
  if (!mInitialized)
    return;
  mParticles.resize(count);
  glBindBuffer(GL_ARRAY_BUFFER, mVBO);
  glBufferData(GL_ARRAY_BUFFER, count * sizeof(glm::vec3), nullptr,
               GL_DYNAMIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  mCapacity = count;
}

void FireflySystem::respawn_(FireflyParticle &p,
                             const glm::vec3 &cameraPos, float radius,
                             float heightMin, float heightMax) {
  glm::vec3 dir(mUnit(mRng), 0.0f, mUnit(mRng));
  float len = glm::length(dir);
  if (len < 0.0001f)
    dir = glm::vec3(1.0f, 0.0f, 0.0f);
  else
    dir /= len;
  float r = std::abs(mUnit(mRng)) * radius;
  float y = heightMin + (heightMax - heightMin) * (0.5f + 0.5f * mUnit(mRng));
  p.pos = cameraPos + dir * r + glm::vec3(0.0f, y, 0.0f);
  p.vel =
      glm::vec3(mUnit(mRng), 0.2f * mUnit(mRng), mUnit(mRng)) * 0.6f;
  p.phase = 6.28318f * (0.5f + 0.5f * mUnit(mRng));
}

void FireflySystem::update(float dt, const glm::vec3 &cameraPos,
                           float nightFactor, int targetCount, float radius,
                           float heightMin, float heightMax) {
  if (!mInitialized)
    return;
  if (targetCount < 0)
    targetCount = 0;
  if (mCapacity != (size_t)targetCount) {
    rebuildBuffer_((size_t)targetCount);
    for (auto &p : mParticles)
      respawn_(p, cameraPos, radius, heightMin, heightMax);
  }
  if (mParticles.empty())
    return;

  const float maxDist = radius * 1.6f;
  for (auto &p : mParticles) {
    p.phase += dt * 0.8f;
    glm::vec3 drift = glm::vec3(std::sin(p.phase), 0.2f * std::cos(p.phase),
                                std::cos(p.phase * 0.7f));
    p.vel += drift * dt * 0.3f;
    p.vel = glm::clamp(p.vel, glm::vec3(-1.2f), glm::vec3(1.2f));
    p.pos += p.vel * dt * (0.3f + 0.7f * nightFactor);

    float dist = glm::length(p.pos - cameraPos);
    if (dist > maxDist || p.pos.y < cameraPos.y + heightMin ||
        p.pos.y > cameraPos.y + heightMax) {
      respawn_(p, cameraPos, radius, heightMin, heightMax);
    }
  }

  // Upload positions
  std::vector<glm::vec3> positions;
  positions.reserve(mParticles.size());
  for (const auto &p : mParticles)
    positions.push_back(p.pos);
  glBindBuffer(GL_ARRAY_BUFFER, mVBO);
  glBufferSubData(GL_ARRAY_BUFFER, 0,
                  positions.size() * sizeof(glm::vec3), positions.data());
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void FireflySystem::draw(const glm::mat4 &view, const glm::mat4 &projection,
                         float timeSec, float nightFactor, float size,
                         float intensity, const glm::vec3 &color) {
  if (!mInitialized || mParticles.empty() || nightFactor < 0.05f)
    return;

  mShader->activate();
  mShader->setMat4("view", view);
  mShader->setMat4("projection", projection);
  mShader->setFloat("uTime", timeSec);
  mShader->setFloat("uSize", size);
  mShader->setFloat("uIntensity", intensity * nightFactor);
  mShader->setVec3("uColor", color);

  glEnable(GL_PROGRAM_POINT_SIZE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE);
  glDepthMask(GL_FALSE);

  glBindVertexArray(mVAO);
  glDrawArrays(GL_POINTS, 0, (GLsizei)mParticles.size());
  glBindVertexArray(0);

  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
}
