#include "CloudFX.h"
#include "Shader.h"
#include <glm/gtc/matrix_transform.hpp>

bool CloudFX::init(const std::string &vertPath, const std::string &fragPath)
{
    mShader = std::make_unique<Shader>(vertPath.c_str(), fragPath.c_str());

    // Quad in XZ plane (y=0), pos.xyz uv.xy
    const float quad[] = {
        -0.5f, 0.0f, -0.5f,   0.0f, 0.0f,
         0.5f, 0.0f, -0.5f,   1.0f, 0.0f,
         0.5f, 0.0f,  0.5f,   1.0f, 1.0f,

        -0.5f, 0.0f, -0.5f,   0.0f, 0.0f,
         0.5f, 0.0f,  0.5f,   1.0f, 1.0f,
        -0.5f, 0.0f,  0.5f,   0.0f, 1.0f
    };

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
    return true;
}

void CloudFX::shutdown()
{
    if (vbo) glDeleteBuffers(1, &vbo);
    if (vao) glDeleteVertexArrays(1, &vao);
    vbo = vao = 0;
    mShader.reset();
}

void CloudFX::draw(const glm::mat4 &view, const glm::mat4 &projection,
                   const glm::vec3 &cameraPos, float timeSec,
                   const glm::vec3 &sunColor, float sunIntensity)
{
    if (!mShader)
        return;
    glBindVertexArray(vao);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    mShader->activate();
    mShader->setMat4("view", view);
    mShader->setMat4("projection", projection);
    mShader->setFloat("uTime", timeSec);
    mShader->setVec3("uSunColor", sunColor);
    mShader->setFloat("uSunIntensity", sunIntensity);

    mShader->setVec3("uCloudColor", color);
    mShader->setFloat("uCloudScale", scale);
    mShader->setFloat("uCloudSpeed", speed);
    mShader->setFloat("uCloudCover", cover);
    mShader->setFloat("uCloudSoftness", softness);
    mShader->setFloat("uCloudAlpha", alpha);

    // NEW volumetric uniforms
    mShader->setVec3("uCameraPos", cameraPos);
    mShader->setFloat("uCloudHeight", height);
    mShader->setFloat("uCloudThickness", thickness);
    mShader->setFloat("uCloudDensity", density);
    mShader->setFloat("uCloudLightAbsorption", lightAbsorption);
    mShader->setFloat("uCloudPhaseG", phaseG);
    mShader->setVec3("uCloudWind", glm::vec3(windDir.x, 0.0f, windDir.y));

    glm::mat4 model(1.0f);
    model = glm::translate(model, glm::vec3(0.0f, height, 0.0f)); // bottom of layer
    model = glm::scale(model, glm::vec3(size));
    mShader->setMat4("model", model);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}
