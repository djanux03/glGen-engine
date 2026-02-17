#include "CloudFX.h"
#include "Shader.h"
#include <glm/gtc/matrix_transform.hpp>

void CloudFX::init()
{
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
}

void CloudFX::shutdown()
{
    if (vbo) glDeleteBuffers(1, &vbo);
    if (vao) glDeleteVertexArrays(1, &vao);
    vbo = vao = 0;
}

void CloudFX::draw(Shader& shader, const glm::vec3& cameraPos)
{
    glBindVertexArray(vao);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    shader.setBool("uUseColor", true);
    shader.setBool("uCloudPass", true);
    shader.setBool("uGlowPass", false);

    shader.setVec3("uCloudColor", color);
    shader.setFloat("uCloudScale", scale);
    shader.setFloat("uCloudSpeed", speed);
    shader.setFloat("uCloudCover", cover);
    shader.setFloat("uCloudSoftness", softness);
    shader.setFloat("uCloudAlpha", alpha);

    // NEW volumetric uniforms
    shader.setVec3("uCameraPos", cameraPos);
    shader.setFloat("uCloudHeight", height);
    shader.setFloat("uCloudThickness", thickness);
    shader.setFloat("uCloudDensity", density);
    shader.setFloat("uCloudLightAbsorption", lightAbsorption);
    shader.setFloat("uCloudPhaseG", phaseG);
    shader.setVec3("uCloudWind", glm::vec3(windDir.x, 0.0f, windDir.y));

    glm::mat4 model(1.0f);
    model = glm::translate(model, glm::vec3(0.0f, height, 0.0f)); // bottom of layer
    model = glm::scale(model, glm::vec3(size));
    shader.setMat4("model", model);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    shader.setBool("uCloudPass", false);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}
