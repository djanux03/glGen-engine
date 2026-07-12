#include "UFBXModel.h"
#include "GLStateCache.h"
#include "Logger.h"
#include "MeshData.h"
#include "MeshParse.h"
#include "Shader.h"
#include "Texture.h"
#include <cstdint>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>

static glm::mat4 buildTRS(const glm::vec3 &pos, const glm::vec3 &rotDeg,
                          const glm::vec3 &scale) {
  glm::mat4 m(1.0f);
  m = glm::translate(m, pos);
  m = glm::rotate(m, glm::radians(rotDeg.y), glm::vec3(0, 1, 0));
  m = glm::rotate(m, glm::radians(rotDeg.x), glm::vec3(1, 0, 0));
  m = glm::rotate(m, glm::radians(rotDeg.z), glm::vec3(0, 0, 1));
  m = glm::scale(m, scale);
  return m;
}

static void bindInstanceMatrixAttributes(GLuint instanceVBO) {
  glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
  const std::size_t vec4Size = sizeof(glm::vec4);
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 4 * vec4Size, (void *)0);
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, 4 * vec4Size,
                        (void *)(1 * vec4Size));
  glEnableVertexAttribArray(5);
  glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, 4 * vec4Size,
                        (void *)(2 * vec4Size));
  glEnableVertexAttribArray(6);
  glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, 4 * vec4Size,
                        (void *)(3 * vec4Size));

  glVertexAttribDivisor(3, 1);
  glVertexAttribDivisor(4, 1);
  glVertexAttribDivisor(5, 1);
  glVertexAttribDivisor(6, 1);
}

static void uploadShadowOnlyMesh(GLuint &vao, GLuint &vbo, GLuint &ebo,
                                 GLsizei &indexCount,
                                 const std::vector<glm::vec3> &positions,
                                 const std::vector<unsigned int> &indices) {
  if (positions.empty() || indices.empty())
    return;

  if (ebo != 0)
    glDeleteBuffers(1, &ebo);
  if (vbo != 0)
    glDeleteBuffers(1, &vbo);
  if (vao != 0)
    glDeleteVertexArrays(1, &vao);

  glGenVertexArrays(1, &vao);
  glGenBuffers(1, &vbo);
  glGenBuffers(1, &ebo);

  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, positions.size() * sizeof(glm::vec3),
               positions.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int),
               indices.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3),
                        (void *)0);
  glBindVertexArray(0);

  indexCount = static_cast<GLsizei>(indices.size());
}

bool UFBXModel::loadFromFile(const std::string &path) {
  auto data = parseMeshFBX(path);
  if (!data)
    return false;
  return loadFromData(*data);
}

GLuint UFBXModel::textureFor_(const MeshData &data, const std::string &path,
                              TextureUsage usage) {
  if (path.empty())
    return 0;

  const std::string key =
      path + (usage == TextureUsage::Color ? "|color" : "|data");
  auto cached = mTextureCache.find(key);
  if (cached != mTextureCache.end())
    return cached->second;

  GLuint texID = 0;
  if (const MeshImage *img = data.findImage(path)) {
    texID = CreateTexture2DFromPixels(img->pixels.data(), img->width,
                                      img->height, img->component, usage);
    if (texID != 0)
      LOG_TRACE("Asset", "ufbx loaded embedded texture bytes");
  } else {
    texID = LoadTexture2D(path, true, usage);
    if (texID != 0)
      LOG_TRACE("Asset", "ufbx loaded texture file: " + path);
  }

  if (texID != 0)
    mTextureCache[key] = texID;
  return texID;
}

bool UFBXModel::loadFromData(const MeshData &data) {
  shutdown();
  mSourcePath = data.sourcePath;

  mHasBounds = false;
  mAabbMin = glm::vec3(1e30f);
  mAabbMax = glm::vec3(-1e30f);

  std::vector<glm::vec3> shadowPositions;
  std::vector<unsigned int> shadowIndices;

  for (const auto &sd : data.submeshes) {
    UFBXSubmesh submesh;
    submesh.name = sd.objectName;
    submesh.materialName = sd.materialName;
    submesh.material = sd.material;

    submesh.material.texDiffuse =
        textureFor_(data, submesh.material.texDiffusePath, TextureUsage::Color);
    submesh.material.texNormal =
        textureFor_(data, submesh.material.texNormalPath, TextureUsage::Data);
    submesh.material.texRoughness = textureFor_(
        data, submesh.material.texRoughnessPath, TextureUsage::Data);
    submesh.material.texMetallic = textureFor_(
        data, submesh.material.texMetallicPath, TextureUsage::Data);
    submesh.material.texAO =
        textureFor_(data, submesh.material.texAOPath, TextureUsage::Data);
    submesh.material.texEmissive = textureFor_(
        data, submesh.material.texEmissivePath, TextureUsage::Color);
    submesh.material.texOpacity = textureFor_(
        data, submesh.material.texOpacityPath, TextureUsage::Data);

    if (sd.hasBounds) {
      mAabbMin = glm::min(mAabbMin, sd.aabbMin);
      mAabbMax = glm::max(mAabbMax, sd.aabbMax);
      mHasBounds = true;
    }

    // Combined shadow mesh across all submeshes
    const unsigned int shadowBaseIndex =
        static_cast<unsigned int>(shadowPositions.size());
    shadowPositions.reserve(shadowPositions.size() + sd.vertices.size());
    for (const auto &vertex : sd.vertices) {
      shadowPositions.push_back(vertex.pos);
    }
    shadowIndices.reserve(shadowIndices.size() + sd.indices.size());
    for (uint32_t index : sd.indices) {
      shadowIndices.push_back(shadowBaseIndex + index);
    }

    static_assert(sizeof(UFBXVertex) == sizeof(MeshVertex),
                  "UFBXVertex must match MeshVertex layout");
    submesh.indexCount = (GLsizei)sd.indices.size();

    glGenVertexArrays(1, &submesh.vao);
    glGenBuffers(1, &submesh.vbo);
    glGenBuffers(1, &submesh.ebo);

    glBindVertexArray(submesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, submesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, sd.vertices.size() * sizeof(MeshVertex),
                 sd.vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, submesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 sd.indices.size() * sizeof(uint32_t), sd.indices.data(),
                 GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(UFBXVertex),
                          (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(UFBXVertex),
                          (void *)offsetof(UFBXVertex, uv));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(UFBXVertex),
                          (void *)offsetof(UFBXVertex, normal));

    glBindVertexArray(0);
    mSubmeshes.push_back(submesh);
  }

  uploadShadowOnlyMesh(mShadowMesh.vao, mShadowMesh.vbo, mShadowMesh.ebo,
                       mShadowMesh.indexCount, shadowPositions, shadowIndices);
  mShadowMesh.instancedVBO = 0;
  mShadowMesh.instancingReady = false;

  return true;
}

void UFBXModel::draw(Shader &shader, const glm::vec3 &pos, const glm::vec3 &rot,
                     const glm::vec3 &scale,
                     const MaterialAsset *materialOverride) {
  glm::mat4 modelMatrix = buildTRS(pos, rot, scale);
  shader.setMat4("model", modelMatrix);

  for (auto &sm : mSubmeshes) {
    if (sm.vao == 0)
      continue;
    if (materialOverride) {
      applyMaterial(*materialOverride, shader);
    } else {
      applyMaterial(sm.material, shader);
    }
    GLStateCache::instance().bindVertexArray(sm.vao);
    glDrawElements(GL_TRIANGLES, sm.indexCount, GL_UNSIGNED_INT, 0);
  }
}

void UFBXModel::drawDepth(Shader &shadowShader, const glm::vec3 &pos,
                          const glm::vec3 &rot, const glm::vec3 &scale) {
  glm::mat4 modelMatrix = buildTRS(pos, rot, scale);
  shadowShader.setMat4("model", modelMatrix);

  if (mShadowMesh.vao != 0 && mShadowMesh.indexCount > 0) {
    GLStateCache::instance().bindVertexArray(mShadowMesh.vao);
    glDrawElements(GL_TRIANGLES, mShadowMesh.indexCount, GL_UNSIGNED_INT, 0);
    return;
  }

  for (auto &sm : mSubmeshes) {
    if (sm.vao == 0 || sm.indexCount <= 0)
      continue;
    GLStateCache::instance().bindVertexArray(sm.vao);
    glDrawElements(GL_TRIANGLES, sm.indexCount, GL_UNSIGNED_INT, 0);
  }
}

void UFBXModel::drawInstanced(Shader &shader, unsigned int instanceVBO,
                              int instanceCount) {
  if (instanceCount == 0)
    return;

  for (auto &sm : mSubmeshes) {
    if (sm.vao == 0)
      continue;

    applyMaterial(sm.material, shader);
    GLStateCache::instance().bindVertexArray(sm.vao);

    if (!sm.instancingReady || sm.instancedVBO != instanceVBO) {
      bindInstanceMatrixAttributes(instanceVBO);
      sm.instancedVBO = instanceVBO;
      sm.instancingReady = true;
    }

    glDrawElementsInstanced(GL_TRIANGLES, sm.indexCount, GL_UNSIGNED_INT, 0,
                            instanceCount);
  }
}

void UFBXModel::drawDepthInstanced(Shader &shadowShader,
                                   unsigned int instanceVBO,
                                   int instanceCount) {
  if (instanceCount == 0)
    return;

  if (mShadowMesh.vao != 0 && mShadowMesh.indexCount > 0) {
    GLStateCache::instance().bindVertexArray(mShadowMesh.vao);

    if (!mShadowMesh.instancingReady || mShadowMesh.instancedVBO != instanceVBO) {
      bindInstanceMatrixAttributes(instanceVBO);
      mShadowMesh.instancedVBO = instanceVBO;
      mShadowMesh.instancingReady = true;
    }

    glDrawElementsInstanced(GL_TRIANGLES, mShadowMesh.indexCount,
                            GL_UNSIGNED_INT, 0, instanceCount);
    return;
  }

  for (auto &sm : mSubmeshes) {
    if (sm.vao == 0 || sm.indexCount <= 0)
      continue;

    GLStateCache::instance().bindVertexArray(sm.vao);

    if (!sm.instancingReady || sm.instancedVBO != instanceVBO) {
      bindInstanceMatrixAttributes(instanceVBO);
      sm.instancedVBO = instanceVBO;
      sm.instancingReady = true;
    }

    glDrawElementsInstanced(GL_TRIANGLES, sm.indexCount, GL_UNSIGNED_INT, 0,
                            instanceCount);
  }
}

bool UFBXModel::getGlobalBounds(glm::vec3 &outMin, glm::vec3 &outMax) const {
  if (!mHasBounds)
    return false;
  outMin = mAabbMin;
  outMax = mAabbMax;
  return true;
}

void UFBXModel::shutdown() {
  if (mShadowMesh.vao)
    glDeleteVertexArrays(1, &mShadowMesh.vao);
  if (mShadowMesh.vbo)
    glDeleteBuffers(1, &mShadowMesh.vbo);
  if (mShadowMesh.ebo)
    glDeleteBuffers(1, &mShadowMesh.ebo);
  mShadowMesh = {};

  for (auto &sm : mSubmeshes) {
    if (sm.vao)
      glDeleteVertexArrays(1, &sm.vao);
    if (sm.vbo)
      glDeleteBuffers(1, &sm.vbo);
    if (sm.ebo)
      glDeleteBuffers(1, &sm.ebo);
  }
  mSubmeshes.clear();

  // Free cached textures
  for (auto &[path, texId] : mTextureCache) {
    if (texId != 0)
      glDeleteTextures(1, &texId);
  }
  mTextureCache.clear();

  mHasBounds = false;
  mAabbMin = glm::vec3(1e30f);
  mAabbMax = glm::vec3(-1e30f);
  mSourcePath.clear();
}
