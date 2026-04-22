#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <memory>
#include <string>

class Shader;

class CloudFX
{
public:
    bool init(const std::string &vertPath, const std::string &fragPath);
    void shutdown();
    void draw(const glm::mat4 &view, const glm::mat4 &projection,
              const glm::vec3 &cameraPos, float timeSec,
              const glm::vec3 &sunColor, float sunIntensity,
              const glm::vec3 &sunDir);
    Shader *shader() const { return mShader.get(); }

    bool enabled = false;
    float thickness = 14.0f;         // vertical size of cloud layer
    float density = 0.72f;           // overall density multiplier
    float lightAbsorption = 1.15f;   // how quickly light dies inside cloud
    float phaseG = 0.58f;            // HG anisotropy (forward scattering)
    glm::vec2 windDir = glm::normalize(glm::vec2(1.0f, 0.3f));

    float height = 42.0f;
    float size = 420.0f;

    float scale = 7.5f;
    float speed = 0.010f;
    float cover = 0.52f;
    float softness = 0.22f;
    float alpha = 0.28f;

    glm::vec3 color = glm::vec3(1.0f);

private:
    GLuint vao = 0, vbo = 0;
    std::unique_ptr<Shader> mShader;
};
