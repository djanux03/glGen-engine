#pragma once

#include <vector>
#include <glm/glm.hpp>
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

    bool init(const char* vertPath, const char* fragPath);
    void shutdown();

    void add(const glm::vec3& pos,
             const glm::vec3& vel,
             float lifeSeconds,
             const glm::vec3& color);

    void addSmokeBurst(const glm::vec3& pos, const glm::vec3& forward);

    void update(float dt);
    void draw(const glm::mat4& view, const glm::mat4& projection, float size);

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

    std::unique_ptr<Shader> mShader;

    float mTime = 0.0f; // drives procedural smoke animation
};
