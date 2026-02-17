#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <memory>
#include <string>

class Shader; // forward declare is fine

class HDRSky
{
public:
    HDRSky();
    ~HDRSky(); // <-- declare, don't inline default

    void setYaw01(float yaw01);

    void setRotationDegrees(const glm::vec3& eulerDeg);
    glm::vec3 mSkyRotDeg = glm::vec3(0.0f);

    bool init(const std::string& hdrPath,
              const std::string& vertPath,
              const std::string& fragPath);
    void shutdown();
    void draw(const glm::mat4& view, const glm::mat4& projection,
              float exposure = 1.0f, float gamma = 2.2f);

    void setSolidSky(bool on) { mUseSolidSky = on; }
    void setSkyColors(const glm::vec3& horizon, const glm::vec3& top)
    {
        mSkyHorizon = horizon;
        mSkyTop = top;
    }


private:
    void createFullscreenQuad_();

    std::unique_ptr<Shader> mShader;
    GLuint mHDRTex = 0;

    float mYaw01 = 0.0f;

    GLuint mVAO = 0;
    GLuint mVBO = 0;

    bool mUseSolidSky = false;
    glm::vec3 mSkyHorizon = glm::vec3(0.65f, 0.75f, 0.90f);
    glm::vec3 mSkyTop     = glm::vec3(0.15f, 0.25f, 0.55f);

};
