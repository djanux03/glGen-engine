#include "FireFX.h"
#include "Shader.h"
#include "Texture.h"
#include <glm/gtc/matrix_transform.hpp>

FireFX::FireFX() = default;
FireFX::~FireFX() = default;
bool FireFX::init(const char* fireTexPath,
                  const char* billboardVertPath,
                  const char* fireFragPath,
                  const char* smokeFragPath)
{
mFireShader = std::make_unique<Shader>(
    billboardVertPath,
    fireFragPath
);

mSmokeShader = std::make_unique<Shader>(
    billboardVertPath,
    smokeFragPath
);


    createQuad_();
    mTex = LoadTexture2D(fireTexPath);   // needs alpha in the texture

    return (mTex != 0);
}

void FireFX::shutdown()
{
    if (mTex) glDeleteTextures(1, &mTex);
    if (mVBO) glDeleteBuffers(1, &mVBO);
    if (mVAO) glDeleteVertexArrays(1, &mVAO);
    mTex = 0; mVBO = 0; mVAO = 0;
    mFireShader.reset();
    mSmokeShader.reset();

}

void FireFX::createQuad_()
{
    // quad in local XY plane, centered at origin
    float quad[] = {
        // pos          // uv
        -0.5f,-0.5f,0,   0,0,
         0.5f,-0.5f,0,   1,0,
         0.5f, 0.5f,0,   1,1,
        -0.5f,-0.5f,0,   0,0,
         0.5f, 0.5f,0,   1,1,
        -0.5f, 0.5f,0,   0,1
    };

    glGenVertexArrays(1, &mVAO);
    glGenBuffers(1, &mVBO);

    glBindVertexArray(mVAO);
    glBindBuffer(GL_ARRAY_BUFFER, mVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void FireFX::draw(const glm::mat4& view,
                  const glm::mat4& projection,
                  const glm::vec3& cameraPos,
                  const glm::vec3& firePos,
                  float timeSec)
{
    if (!mFireShader || !mSmokeShader) return;

    // If you added params(), respect enabled
    if (!mParams.enabled) return;

    // Final world position = auto firePos + user offset
    glm::vec3 pos = firePos + mParams.offset;

    float size = mParams.size;

    // Billboard: use camera right/up vectors from view matrix
    glm::vec3 camRight = glm::vec3(view[0][0], view[1][0], view[2][0]);
    glm::vec3 camUp    = glm::vec3(view[0][1], view[1][1], view[2][1]);

    // Base billboard model (fire)
    glm::mat4 model(1.0f);
    model[0] = glm::vec4(camRight * size, 0.0f);
    model[1] = glm::vec4(camUp    * size, 0.0f);
    model[2] = glm::vec4(glm::normalize(glm::cross(camRight, camUp)) * size, 0.0f);
    model[3] = glm::vec4(pos, 1.0f);

    // Smoke model: larger + lifted above the flame
    glm::mat4 smokeModel = model;
    smokeModel[0] *= mParams.smokeScaleXY;      // wider
    smokeModel[1] *= mParams.smokeScaleY;       // taller
    smokeModel[3].y += mParams.smokeLift * size;

    // Optional mask texture
    bool useMask = (mTex != 0);
    if (useMask)
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mTex);
    }

    glBindVertexArray(mVAO);

    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE);

// More slices around the Y axis -> less 2D


    // ---------------- FIRE (additive) ----------------
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    mFireShader->activate();
    mFireShader->setMat4("view", view);
    mFireShader->setMat4("projection", projection);
    mFireShader->setMat4("model", model);
    mFireShader->setFloat("uTime", timeSec);
    mFireShader->setFloat("uIntensity", mParams.intensity);
    mFireShader->setInt("uTex", 0);
    mFireShader->setBool("uUseMaskTex", useMask);

    // If you implemented rim lighting version:
    // mFireShader->setVec3("uCameraPos", cameraPos);

    // More slices around the Y axis -> less 2D
static const float yaws[] = {
    0.0f,
    0.78539816f,
    1.5707963f,
    2.35619449f,
    3.14159265f,
    -2.35619449f,
    -1.5707963f,
    -0.78539816f
};

// Build a small "stack" of fire layers (bottom/mid/top)
struct FireLayer { float yOff; float scale; float intensityMul; float jitter; };
static const FireLayer layers[] = {
    { 0.00f, 1.00f, 1.00f, 0.03f },
    { 0.22f, 0.85f, 0.85f, 0.04f },
    { 0.45f, 0.65f, 0.70f, 0.05f }
};

// Small, stable per-slice offsets to break symmetry (in billboard space)
static const glm::vec2 sliceJitter[] = {
    { 0.00f,  0.00f },
    { 0.30f,  0.10f },
    {-0.25f,  0.15f },
    { 0.15f, -0.20f },
    {-0.10f, -0.30f },
    { 0.22f, -0.05f },
    {-0.18f,  0.06f },
    { 0.08f,  0.26f }
};

for (const auto& layer : layers)
{
    // Start from the base billboard model you already built
    glm::mat4 layerModel = model;

    // Move the layer upward (in world Y)
    layerModel[3].y += layer.yOff * size;

    // Scale layer in X/Y by multiplying the billboard basis vectors
    layerModel[0] *= layer.scale;
    layerModel[1] *= layer.scale;

    // Slightly reduce intensity as it goes up
    mFireShader->setFloat("uIntensity", mParams.intensity * layer.intensityMul);

    for (int i = 0; i < 8; ++i)
    {
        // Jitter in billboard right/up so it stays "around" the flame regardless of camera angle
        glm::vec3 jitterWorld =
            camRight * (sliceJitter[i].x * layer.jitter * size) +
            camUp    * (sliceJitter[i].y * layer.jitter * size);

        glm::mat4 sliceModel = layerModel;
        sliceModel[3].x += jitterWorld.x;
        sliceModel[3].y += jitterWorld.y;
        sliceModel[3].z += jitterWorld.z;

        mFireShader->setMat4("model", sliceModel);
        mFireShader->setFloat("uYaw", yaws[i]);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
}


    // ---------------- SMOKE (alpha blend) ----------------
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    mSmokeShader->activate();
    mSmokeShader->setMat4("view", view);
    mSmokeShader->setMat4("projection", projection);
    mSmokeShader->setMat4("model", smokeModel);
    mSmokeShader->setFloat("uTime", timeSec);
    mSmokeShader->setFloat("uOpacity", mParams.smokeOpacity);
    mSmokeShader->setInt("uTex", 0);
    mSmokeShader->setBool("uUseMaskTex", useMask);

    // Smoke can use fewer slices if you want (2 is often enough)
    mSmokeShader->setFloat("uYaw", 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    mSmokeShader->setFloat("uYaw", 1.5707963f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}
