#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>

class Shader;

class CloudFX
{
public:
    void init();
    void shutdown();
void draw(Shader& shader, const glm::vec3& cameraPos);

    // tweakables (additions)
    float thickness = 8.0f;          // vertical size of cloud layer
    float density = 1.2f;            // overall density multiplier
    float lightAbsorption = 1.4f;    // how quickly light dies inside cloud
    float phaseG = 0.75f;            // HG anisotropy (forward scattering)
    glm::vec2 windDir = glm::normalize(glm::vec2(1.0f, 0.3f));

    float height = 16.0f;
    float size = 160.0f;

    float scale = 15.0f;
    float speed = 0.015f;
    float cover = 0.55f;
    float softness = 0.10f;
    float alpha = 0.45f;

    glm::vec3 color = glm::vec3(1.0f);

private:
    GLuint vao = 0, vbo = 0;
};
