#include "ProjectileSystem.h"
#include "Shader.h"

#include <glad/glad.h>
#include <algorithm>
#include <cstddef>
#include <cmath>

ProjectileSystem::~ProjectileSystem()
{
    shutdown();
}

bool ProjectileSystem::init(const char* vertPath, const char* fragPath,
                            const char* laserVertPath,
                            const char* laserFragPath)
{
    shutdown();

    mShader = std::make_unique<Shader>(vertPath, fragPath);
    if (!mShader || !mShader->isValid()) return false;

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

    if (laserVertPath && laserFragPath)
    {
        mLaserShader = std::make_unique<Shader>(laserVertPath, laserFragPath);
        if (!mLaserShader || !mLaserShader->isValid())
            return false;

        const float beamVerts[] = {
            0.0f, -1.0f,
            0.0f,  1.0f,
            1.0f, -1.0f,
            1.0f,  1.0f,
        };

        glGenVertexArrays(1, &mLaserVAO);
        glBindVertexArray(mLaserVAO);

        glGenBuffers(1, &mLaserVBO);
        glBindBuffer(GL_ARRAY_BUFFER, mLaserVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(beamVerts), beamVerts,
                     GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                              (void*)0);

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
    }

    mTime = 0.0f;
    return true;
}

void ProjectileSystem::shutdown()
{
    if (mInstanceVBO) glDeleteBuffers(1, &mInstanceVBO);
    if (mQuadVBO)     glDeleteBuffers(1, &mQuadVBO);
    if (mVAO)         glDeleteVertexArrays(1, &mVAO);
    if (mLaserVBO)    glDeleteBuffers(1, &mLaserVBO);
    if (mLaserVAO)    glDeleteVertexArrays(1, &mLaserVAO);

    mInstanceVBO = 0;
    mQuadVBO = 0;
    mVAO = 0;
    mLaserVBO = 0;
    mLaserVAO = 0;

    mShader.reset();
    mLaserShader.reset();

    mProj.clear();
    mSmoke.clear();
    mTime = 0.0f;
    clearLaserBeam();
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

    if (mLaserActive)
    {
        mLaserLife -= dt;
        if (mLaserLife <= 0.0f)
            clearLaserBeam();
    }

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

void ProjectileSystem::setLaserBeam(const glm::vec3& start,
                                    const glm::vec3& end,
                                    bool hit,
                                    uint32_t hitEntity,
                                    const glm::vec3& hitNormal,
                                    float durationSeconds)
{
    mLaserStart = start;
    mLaserEnd = end;
    mLaserHit = hit;
    mLaserHitEntity = hitEntity;
    mLaserHitNormal = hitNormal;
    mLaserActive = glm::length(end - start) > 0.01f;
    mLaserStartLife = std::max(0.01f, durationSeconds);
    mLaserLife = mLaserStartLife;
}

void ProjectileSystem::clearLaserBeam()
{
    mLaserActive = false;
    mLaserHit = false;
    mLaserHitEntity = 0;
    mLaserLife = 0.0f;
    mLaserStartLife = 0.0f;
}

void ProjectileSystem::draw(const glm::mat4& view,
                            const glm::mat4& projection,
                            const glm::vec3& cameraPos,
                            float size)
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

    if (mLaserActive && mLaserShader && mLaserVAO)
    {
        GLint prevBlendSrcRgb = GL_ONE;
        GLint prevBlendDstRgb = GL_ZERO;
        GLint prevBlendSrcAlpha = GL_ONE;
        GLint prevBlendDstAlpha = GL_ZERO;
        GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
        GLboolean cullWasEnabled = glIsEnabled(GL_CULL_FACE);
        GLboolean depthMask = GL_TRUE;

        glGetIntegerv(GL_BLEND_SRC_RGB, &prevBlendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &prevBlendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlendDstAlpha);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);

        mLaserShader->activate();
        mLaserShader->setMat4("view", view);
        mLaserShader->setMat4("projection", projection);
        mLaserShader->setVec3("uStart", mLaserStart);
        mLaserShader->setVec3("uEnd", mLaserEnd);
        mLaserShader->setVec3("uCameraPos", cameraPos);
        mLaserShader->setVec3("uColor", mLaserColor);
        mLaserShader->setFloat("uRadius", mLaserRadius);
        mLaserShader->setFloat("uIntensity", mLaserIntensity);
        mLaserShader->setFloat("uTime", mTime);
        const float pulse =
            mLaserStartLife > 0.0f ? (mLaserLife / mLaserStartLife) : 0.0f;
        mLaserShader->setFloat("uPulse", std::clamp(pulse, 0.0f, 1.0f));
        mLaserShader->setBool("uHit", mLaserHit);

        glBindVertexArray(mLaserVAO);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDepthMask(depthMask);

        if (cullWasEnabled)
            glEnable(GL_CULL_FACE);
        else
            glDisable(GL_CULL_FACE);
        glBlendFuncSeparate(prevBlendSrcRgb, prevBlendDstRgb,
                            prevBlendSrcAlpha, prevBlendDstAlpha);
        if (!blendWasEnabled)
            glDisable(GL_BLEND);

        glBindVertexArray(0);
    }
}
