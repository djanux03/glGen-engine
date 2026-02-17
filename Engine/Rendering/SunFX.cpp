#include "SunFX.h"
#include "Shader.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cstdlib>

static float rand01() { return (float)rand() / (float)RAND_MAX; }

void SunFX::init()
{
    // Quad in XY plane (camera-facing via model matrix), z = 0
    // pos.xyz, uv.xy
    const float quad[] = {
        -0.5f, -0.5f, 0.0f,   0.0f, 0.0f,
         0.5f, -0.5f, 0.0f,   1.0f, 0.0f,
         0.5f,  0.5f, 0.0f,   1.0f, 1.0f,

        -0.5f, -0.5f, 0.0f,   0.0f, 0.0f,
         0.5f,  0.5f, 0.0f,   1.0f, 1.0f,
        -0.5f,  0.5f, 0.0f,   0.0f, 1.0f
    };

    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);

    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    particles.reserve(maxParticles);
}

void SunFX::shutdown()
{
    if (quadVBO) glDeleteBuffers(1, &quadVBO);
    if (quadVAO) glDeleteVertexArrays(1, &quadVAO);
    quadVBO = quadVAO = 0;
}

glm::mat4 SunFX::billboardModel(const glm::vec3& pos,
                                float size,
                                const glm::vec3& cameraFront,
                                const glm::vec3& cameraUp) const
{
    // Build billboard basis from camera vectors (right/up/look).
    // This is the standard approach: compute right with a cross product, then rebuild up. 
    glm::vec3 look  = glm::normalize(-cameraFront);
    glm::vec3 right = glm::normalize(glm::cross(cameraUp, look));
    glm::vec3 up    = glm::normalize(glm::cross(look, right));

    glm::mat4 rot(1.0f);
    rot[0] = glm::vec4(right, 0.0f);
    rot[1] = glm::vec4(up,    0.0f);
    rot[2] = glm::vec4(look,  0.0f);

    glm::mat4 model(1.0f);
    model = glm::translate(model, pos);
    model *= rot;
    model = glm::scale(model, glm::vec3(size));

    return model;
}

void SunFX::update(float dt, float /*timeSec*/)
{
    emitCarry += emitRate * dt;
    int spawnCount = (int)emitCarry;
    emitCarry -= (float)spawnCount;

    for (int i = 0; i < spawnCount && (int)particles.size() < maxParticles; i++)
    {
        glm::vec3 dir(rand01() * 2.0f - 1.0f,
                      rand01() * 0.8f,
                      rand01() * 2.0f - 1.0f);
        if (glm::length(dir) < 0.001f) dir = glm::vec3(1, 0, 0);
        dir = glm::normalize(dir);

        SunParticle p;
        p.pos = sunPos;
        p.vel = dir * particleSpeed;
        p.maxLife = particleLife;
        p.life = particleLife;
        p.size = particleSize;
        particles.push_back(p);
    }

    for (auto& p : particles)
    {
        p.life -= dt;
        p.pos  += p.vel * dt;
        p.vel  *= (1.0f - 0.6f * dt); // drag
    }

    particles.erase(
        std::remove_if(particles.begin(), particles.end(),
                       [](const SunParticle& p){ return p.life <= 0.0f; }),
        particles.end()
    );
}

void SunFX::draw(Shader& shader,
                 const glm::vec3& cameraFront,
                 const glm::vec3& cameraUp)
{
    glBindVertexArray(quadVAO);

    // Save state you change (optional, but good hygiene)
    GLboolean wasBlend = glIsEnabled(GL_BLEND);

    // Keep depth test ON (so sun can be occluded), but don't write depth for translucent passes
    glDepthMask(GL_FALSE);

    // ---------- 1) Core: alpha blend (no rectangle) ----------
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // standard transparency [web:605][web:882]

    shader.setBool("uUseColor", true);
    shader.setBool("uGlowPass", true);
    shader.setFloat("uGlowStrength", glowStrength);
    shader.setVec4("uColor", glm::vec4(1.0f));
    shader.setMat4("model", billboardModel(sunPos, sunSize, cameraFront, cameraUp));
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // ---------- 2) Halo + particles: additive ----------
    glBlendFunc(GL_SRC_ALPHA, GL_ONE); // additive glow [web:605]

    // Halo
    shader.setBool("uGlowPass", true);
    shader.setFloat("uGlowStrength", glowStrength);
    shader.setMat4("model", billboardModel(sunPos, sunSize * haloSizeMult, cameraFront, cameraUp));
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Particles (additive sprites)
    shader.setBool("uGlowPass", false);
    for (const auto& p : particles)
    {
        float a = p.life / p.maxLife;
        shader.setVec4("uColor", glm::vec4(1.0f, 0.45f, 0.10f, a * 0.8f));
        shader.setMat4("model", billboardModel(p.pos, p.size, cameraFront, cameraUp));
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    // Restore state
    glDepthMask(GL_TRUE);
    if (!wasBlend) glDisable(GL_BLEND);

    glBindVertexArray(0);
}

