#include "HDRSky.h"
#include "Shader.h"
#include "Texture.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <glm/gtx/euler_angles.hpp>
#include <glm/gtc/type_ptr.hpp>


HDRSky::HDRSky() = default;
HDRSky::~HDRSky() = default;

bool HDRSky::init(const std::string& hdrPath)
{
    mShader = std::make_unique<Shader>(
        "/Users/edjan03/Downloads/glGen-main/shaders/glsl/hdr_sky.vert",
        "/Users/edjan03/Downloads/glGen-main/shaders/glsl/hdr_sky.frag"
    );

    createFullscreenQuad_();

    mHDRTex = LoadHDRTexture2D(hdrPath, true);
    return (mHDRTex != 0);
}

void HDRSky::shutdown()
{
    if (mHDRTex) glDeleteTextures(1, &mHDRTex);
    if (mVBO)    glDeleteBuffers(1, &mVBO);
    if (mVAO)    glDeleteVertexArrays(1, &mVAO);

    mHDRTex = 0;
    mVBO = 0;
    mVAO = 0;
    mShader.reset();
}

void HDRSky::createFullscreenQuad_()
{
    float quad[] = {
        // pos      // uv
        -1.f, -1.f,  0.f, 0.f,
         1.f, -1.f,  1.f, 0.f,
         1.f,  1.f,  1.f, 1.f,
        -1.f, -1.f,  0.f, 0.f,
         1.f,  1.f,  1.f, 1.f,
        -1.f,  1.f,  0.f, 1.f
    };

    glGenVertexArrays(1, &mVAO);
    glGenBuffers(1, &mVBO);

    glBindVertexArray(mVAO);
    glBindBuffer(GL_ARRAY_BUFFER, mVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}


void HDRSky::setYaw01(float yaw01)
{
    // Keep it in [0..1) so GLSL fract() wraps cleanly
    mYaw01 = yaw01 - floorf(yaw01);
    if (mYaw01 < 0.0f) mYaw01 += 1.0f;
}

void HDRSky::setRotationDegrees(const glm::vec3& eulerDeg)
{
    mSkyRotDeg = eulerDeg;
}

void HDRSky::draw(const glm::mat4& view,
                  const glm::mat4& projection,
                  float exposure,
                  float gamma)
{
if (!mShader) return;

    // Background render state
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);

    mShader->activate();
    mShader->setBool("uUseSolidSky", mUseSolidSky || (mHDRTex == 0));
    mShader->setVec3("uSkyHorizon", mSkyHorizon);
    mShader->setVec3("uSkyTop", mSkyTop);

    mShader->setInt("uHDR", 0);
    mShader->setFloat("uExposure", exposure);
    mShader->setFloat("uGamma", gamma);
    mShader->setFloat("uYaw", mYaw01);

    glm::vec3 r = glm::radians(mSkyRotDeg);

// GLM provides yawPitchRoll(Y, X, Z) (Y * X * Z) [web:385]
    glm::mat4 R4 = glm::yawPitchRoll(r.y, r.x, r.z);
    glm::mat3 R  = glm::mat3(R4);

    mShader->setMat3("uSkyRot", R);

    glm::mat4 invProj = glm::inverse(projection);
    glm::mat4 invView = glm::inverse(view);
    mShader->setMat4("uInvProj", invProj);
    mShader->setMat4("uInvView", invView);


    if (!(mUseSolidSky || (mHDRTex == 0)))
{
    mShader->setInt("uHDR", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mHDRTex);
}


    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mHDRTex);

    glBindVertexArray(mVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Restore state
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
}
