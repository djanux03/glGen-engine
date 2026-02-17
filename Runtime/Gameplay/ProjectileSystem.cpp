#include "ProjectileSystem.h"
#include "Shader.h"

#include <glad/glad.h>
#include <algorithm>
#include <cstddef>

ProjectileSystem::~ProjectileSystem()
{
    shutdown();
}

bool ProjectileSystem::init(const char* vertPath, const char* fragPath)
{
    shutdown();

    mShader = std::make_unique<Shader>(vertPath, fragPath);
    if (!mShader) return false;

    // 2D quad centered at origin (XY plane), triangle strip (4 verts).
    const float quad[] = {
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
        -0.5f,  0.5f, 0.0f,
         0.5f,  0.5f, 0.0f
    };

    glGenVertexArrays(1, &mVAO);
    glBindVertexArray(mVAO);

    glGenBuffers(1, &mQuadVBO);
    glBindBuffer(GL_ARRAY_BUFFER, mQuadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    // location 0: quad vertex (vec3)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

    // instance buffer
    glGenBuffers(1, &mInstanceVBO);
    glBindBuffer(GL_ARRAY_BUFFER, mInstanceVBO);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STREAM_DRAW);

    // location 1: instance center (vec3)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(InstanceGPU), (void*)offsetof(InstanceGPU, center));
    glVertexAttribDivisor(1, 1);

    // location 2: instance color (vec3)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(InstanceGPU), (void*)offsetof(InstanceGPU, color));
    glVertexAttribDivisor(2, 1);

    // location 3: instance alpha (float)
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(InstanceGPU), (void*)offsetof(InstanceGPU, alpha));
    glVertexAttribDivisor(3, 1);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    mTime = 0.0f;
    return true;
}

void ProjectileSystem::shutdown()
{
    if (mInstanceVBO) glDeleteBuffers(1, &mInstanceVBO);
    if (mQuadVBO)     glDeleteBuffers(1, &mQuadVBO);
    if (mVAO)         glDeleteVertexArrays(1, &mVAO);

    mInstanceVBO = 0;
    mQuadVBO = 0;
    mVAO = 0;

    mShader.reset();

    mProj.clear();
    mSmoke.clear();
    mTime = 0.0f;
}

void ProjectileSystem::addSmokeBurst(const glm::vec3& pos, const glm::vec3& forward)
{
    const int count = 12;

    for (int i = 0; i < count; ++i)
    {
        SmokeParticle s;
        s.pos = pos;

        float t = (i - count * 0.5f) / (float)count; // -0.5..0.5
        glm::vec3 sideways = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
        glm::vec3 dir = glm::normalize(forward * 0.6f + glm::vec3(0, 1, 0) * 0.8f + sideways * t * 0.6f);

        s.vel = dir * 3.0f;
        s.life = 0.8f;
        s.startLife = s.life;

        mSmoke.push_back(s);
    }
}

void ProjectileSystem::add(const glm::vec3& pos,
                           const glm::vec3& vel,
                           float lifeSeconds,
                           const glm::vec3& color)
{
    Projectile p;
    p.pos = pos;
    p.vel = vel;
    p.life = lifeSeconds;
    p.color = color;
    mProj.push_back(p);
}

void ProjectileSystem::update(float dt)
{
    mTime += dt;

    for (auto& p : mProj)
    {
        p.pos += p.vel * dt;
        p.life -= dt;
    }

    mProj.erase(std::remove_if(mProj.begin(), mProj.end(),
                              [](const Projectile& p) { return p.life <= 0.0f; }),
                mProj.end());

    for (auto& s : mSmoke)
    {
        s.pos += s.vel * dt;
        s.life -= dt;
        s.vel *= 0.92f; // drag
    }

    mSmoke.erase(std::remove_if(mSmoke.begin(), mSmoke.end(),
                                [](const SmokeParticle& s) { return s.life <= 0.0f; }),
                 mSmoke.end());
}

void ProjectileSystem::draw(const glm::mat4& view, const glm::mat4& projection, float size)
{
    if (!mShader) return;

    mShader->activate();
    mShader->setMat4("view", view);
    mShader->setMat4("projection", projection);
    mShader->setFloat("uTime", mTime);

    glBindVertexArray(mVAO);
    glEnable(GL_BLEND);

    // 1) Bullets (additive, no smoke noise)
    if (!mProj.empty())
    {
        std::vector<InstanceGPU> inst;
        inst.reserve(mProj.size());
        for (const auto& p : mProj)
            inst.push_back({ p.pos, p.color, 1.0f });

        mShader->setInt("uSmokePass", 0);
        mShader->setFloat("uSize", size);

        glBindBuffer(GL_ARRAY_BUFFER, mInstanceVBO);
        glBufferData(GL_ARRAY_BUFFER, inst.size() * sizeof(InstanceGPU), inst.data(), GL_STREAM_DRAW);

        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)inst.size());
    }

    // 2) Smoke (alpha blend, procedural noise)
    if (!mSmoke.empty())
    {
        std::vector<InstanceGPU> inst;
        inst.reserve(mSmoke.size());
        for (const auto& s : mSmoke)
        {
            float a = (s.startLife > 0.0f) ? (s.life / s.startLife) : 0.0f;
            inst.push_back({ s.pos, glm::vec3(0.5f), a });
        }

        mShader->setInt("uSmokePass", 1);
        mShader->setFloat("uSize", size * 3.5f);

        glBindBuffer(GL_ARRAY_BUFFER, mInstanceVBO);
        glBufferData(GL_ARRAY_BUFFER, inst.size() * sizeof(InstanceGPU), inst.data(), GL_STREAM_DRAW);

        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Optional (usually improves blended particles):
        glDepthMask(GL_FALSE);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)inst.size());
        glDepthMask(GL_TRUE);
    }

    glDisable(GL_BLEND);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}
