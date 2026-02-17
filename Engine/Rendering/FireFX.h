#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <memory>

class Shader;


struct FireFXParams {
    bool enabled = true;

    glm::vec3 position = glm::vec3(0.0f);  // world-space override (optional)
    glm::vec3 offset   = glm::vec3(0.0f);  // local tweak

    float size = 1.0f;

    // Fire look
    float intensity = 1.0f;     // drives uIntensity
    float smokeOpacity = 0.8f;  // drives uOpacity

    // Smoke shaping
    float smokeScaleXY = 1.7f;  // width multiplier
    float smokeScaleY  = 2.3f;  // height multiplier
    float smokeLift    = 0.6f;  // lift in "size units"
};


class FireFX
{
public:
    FireFX();
    ~FireFX();              // declare only (no =default here)

    bool init(const char* fireTexPath,
              const char* billboardVertPath,
              const char* fireFragPath,
              const char* smokeFragPath);
    void shutdown();

    void draw(const glm::mat4& view,
              const glm::mat4& projection,
              const glm::vec3& cameraPos,
              const glm::vec3& firePos,
              float timeSec);

    void setSize(float s) { mSize = s; }
    FireFXParams& params() { return mParams; }
    const FireFXParams& params() const { return mParams; }
private:
    void createQuad_();

    std::unique_ptr<Shader> mFireShader;
    std::unique_ptr<Shader> mSmokeShader;
    GLuint mTex = 0;
    GLuint mVAO = 0, mVBO = 0;
    float mSize = 0.8f;
    
    FireFXParams mParams;

};
