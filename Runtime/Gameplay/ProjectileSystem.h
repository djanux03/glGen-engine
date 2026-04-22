#pragma once

#include <vector>
#include <glm/glm.hpp>
#include <cstdint>
#include <memory>

class Shader;

struct Projectile
{
    glm::vec3 pos{0.0f};
    glm::vec3 vel{0.0f};
    glm::vec3 color{1.0f};
    float life = 0.0f; // seconds remaining
};

struct SmokeParticle
{
    glm::vec3 pos{0.0f};
    glm::vec3 vel{0.0f};
    float life = 0.0f;
    float startLife = 0.0f; // for fading
};

class ProjectileSystem
{
public:
    ProjectileSystem() = default;
    ~ProjectileSystem();

    bool init(const char* vertPath, const char* fragPath,
              const char* laserVertPath = nullptr,
              const char* laserFragPath = nullptr);
    void shutdown();

    void add(const glm::vec3& pos,
             const glm::vec3& vel,
             float lifeSeconds,
             const glm::vec3& color);

    void addSmokeBurst(const glm::vec3& pos, const glm::vec3& forward);

    void update(float dt);
    void draw(const glm::mat4& view, const glm::mat4& projection,
              const glm::vec3& cameraPos, float size);

    void setLaserBeam(const glm::vec3& start,
                      const glm::vec3& end,
                      bool hit,
                      uint32_t hitEntity,
                      const glm::vec3& hitNormal,
                      float durationSeconds = 0.16f);
    void clearLaserBeam();
    bool laserActive() const { return mLaserActive; }
    uint32_t laserHitEntity() const { return mLaserHitEntity; }
    glm::vec3 laserHitPosition() const { return mLaserEnd; }

    std::size_t count() const { return mProj.size(); }

private:
    struct InstanceGPU
    {
        glm::vec3 center;
        glm::vec3 color;
        float alpha;
    };

    std::vector<Projectile> mProj;
    std::vector<SmokeParticle> mSmoke;

    unsigned int mVAO = 0;
    unsigned int mQuadVBO = 0;
    unsigned int mInstanceVBO = 0;
    unsigned int mLaserVAO = 0;
    unsigned int mLaserVBO = 0;

    std::unique_ptr<Shader> mShader;
    std::unique_ptr<Shader> mLaserShader;

    float mTime = 0.0f; // drives procedural smoke animation

    bool mLaserActive = false;
    bool mLaserHit = false;
    uint32_t mLaserHitEntity = 0;
    glm::vec3 mLaserStart{0.0f};
    glm::vec3 mLaserEnd{0.0f};
    glm::vec3 mLaserHitNormal{0.0f, 1.0f, 0.0f};
    glm::vec3 mLaserColor{0.25f, 0.82f, 1.0f};
      float mLaserRadius = 0.075f;
      float mLaserIntensity = 4.2f;
    float mLaserLife = 0.0f;
    float mLaserStartLife = 0.0f;
};
