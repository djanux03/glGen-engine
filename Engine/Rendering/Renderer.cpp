#include "Renderer.h"
#include "Shader.h"
#include <GLFW/glfw3.h> // Needed for glfwExtensionSupported check if you add Anisotropy
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::init(const char* vertexPath,
                    const char* fragmentPath,
                    const char* sidePath,
                    const char* topPath,
                    const char* bottomPath)
{
    (void)sidePath; (void)topPath; (void)bottomPath;

    // Suggestion: Increase shadow res to 2048 for sharper shadows if performance allows
    return initWithShadows(vertexPath, fragmentPath,
                           sidePath, topPath, bottomPath,
                           "/Users/edjan03/Downloads/glGen-main/shaders/glsl/point_shadow_depth.vert",
                           "/Users/edjan03/Downloads/glGen-main/shaders/glsl/point_shadow_depth.frag",
                           2048); 
}

bool Renderer::initWithShadows(const char* vertexPath,
                              const char* fragmentPath,
                              const char* sidePath,
                              const char* topPath,
                              const char* bottomPath,
                              const char* shadowVertPath,
                              const char* shadowFragPath,
                              int shadowMapRes)
{
    (void)sidePath; (void)topPath; (void)bottomPath;

    mShader = std::make_unique<Shader>(vertexPath, fragmentPath);
    mShader->activate();

    mShader->setFloat("uGamma", 2.2f);
    mShader->setFloat("uSpecStrength", 0.5f); // Bumped up slightly
    mShader->setFloat("uShininess", 32.0f);   // Lower shininess = larger highlights

    mShader->setInt("texture1", 0);
    mShader->setInt("shadowCube", 1);
    mShader->setFloat("uSunIntensity", 1.0f);
    mShader->setFloat("uShadowStrength", 1.5f);

    if (!initShadowResources_(shadowVertPath, shadowFragPath, shadowMapRes))
        std::cout << "Shadow resources init failed (continuing without shadows)\n";

    return true;
}

bool Renderer::initShadowResources_(const char* shadowVertPath,
                                   const char* shadowFragPath,
                                   int shadowMapRes)
{
    mShadowRes = shadowMapRes;

    glGenTextures(1, &mShadowCubeTex);
    glBindTexture(GL_TEXTURE_CUBE_MAP, mShadowCubeTex);

    for (unsigned int i = 0; i < 6; ++i)
    {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0,
                     GL_DEPTH_COMPONENT,
                     mShadowRes, mShadowRes, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    }

    // VISUAL UPGRADE: Use GL_LINEAR for PCF (soft shadows) in shader
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR); 
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

    glGenFramebuffers(1, &mShadowFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, mShadowFBO);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, mShadowCubeTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        std::cout << "Shadow FBO incomplete: " << status << "\n";
        shutdownShadowResources_();
        return false;
    }

    mShadowShader = std::make_unique<Shader>(shadowVertPath, shadowFragPath);
    return true;
}

void Renderer::shutdownShadowResources_()
{
    if (mShadowCubeTex) glDeleteTextures(1, &mShadowCubeTex);
    if (mShadowFBO)     glDeleteFramebuffers(1, &mShadowFBO);

    mShadowCubeTex = 0;
    mShadowFBO = 0;
    mShadowShader.reset();
}

void Renderer::shutdown()
{
    shutdownShadowResources_();
    mShader.reset();
}

void Renderer::beginFrame(float r, float g, float b, float a)
{
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::setFrameUniforms(const glm::mat4& view,
                                const glm::mat4& projection,
                                float mixVal,
                                float timeSec,
                                const glm::vec3& sunColor,
                                float ambientStrength,
                                const glm::vec3& cameraPos,
                                float sunIntensity,
                                const glm::vec3& lightPos,
                                float farPlane,
                                float shadowStrength)
{
    mShader->activate();

    mShader->setMat4("view", view);
    mShader->setMat4("projection", projection);
    mShader->setFloat("mixVal", mixVal);
    mShader->setFloat("uTime", timeSec);

    mShader->setVec3("uSunColor", sunColor);
    mShader->setFloat("uAmbient", ambientStrength);
    mShader->setVec3("uCameraPos", cameraPos);
    mShader->setFloat("uSunIntensity", sunIntensity);

    mShader->setVec3("uLightPos", lightPos);
    mShader->setFloat("uFarPlane", farPlane);
    mShader->setFloat("uShadowStrength", shadowStrength);

    // VISUAL UPGRADE: Fog
    // (Ensure you add these uniforms to your Fragment Shader)
    mShader->setVec3("uFogColor", glm::vec3(0.5f, 0.6f, 0.7f)); // Sky blue-ish
    mShader->setFloat("uFogDensity", 0.005f); // Atmosphere thickness

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, mShadowCubeTex);
    glActiveTexture(GL_TEXTURE0);
}

void Renderer::beginShadowPass()
{
    if (!mShadowFBO || !mShadowCubeTex || !mShadowShader) return;

    glGetIntegerv(GL_VIEWPORT, mPrevViewport);
    glViewport(0, 0, mShadowRes, mShadowRes);

    glBindFramebuffer(GL_FRAMEBUFFER, mShadowFBO);
    glClear(GL_DEPTH_BUFFER_BIT);

    // VISUAL UPGRADE: Cull Front Faces
    // This solves "peter panning" (floating shadows) better than PolygonOffset usually does.
    // It renders the BACK of the objects into the shadow map.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
}

void Renderer::endShadowPass()
{
    if (!mShadowFBO) return;

    // Restore standard rendering state
    glCullFace(GL_BACK);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(mPrevViewport[0], mPrevViewport[1], mPrevViewport[2], mPrevViewport[3]);
}

Shader& Renderer::shader() { return *mShader; }
Shader& Renderer::shadowShader() { return *mShadowShader; }
