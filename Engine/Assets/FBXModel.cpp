#include "FBXModel.h"
#include "GLStateCache.h"
#include "Logger.h"
#include "MeshData.h"
#include "MeshParse.h"
#include "Shader.h"
#include "Texture.h"

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

bool FBXModel::loadFromFile(const std::string &path) {
  auto data = parseMeshGLTF(path);
  if (!data)
    return false;
  return loadFromData(*data);
}

GLuint FBXModel::textureFor_(const MeshData &data, const std::string &path,
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
  } else {
    // glTF parses all images into payloads, so this only triggers for
    // paths pointing outside the parsed image set.
    texID = LoadTexture2D(path, true, usage);
  }

  if (texID != 0) {
    mTextureCache[key] = texID;
    LOG_TRACE("Asset", "Loaded glTF texture: " + path);
  }
  return texID;
}

bool FBXModel::loadFromData(const MeshData &data) {
  shutdown();
  mSourcePath = data.sourcePath;

  mHasBounds = false;
  mAabbMin = glm::vec3(1e30f);
  mAabbMax = glm::vec3(-1e30f);

  for (const auto &sd : data.submeshes) {
    FBXSubmesh submesh;
    submesh.name = sd.objectName;
    submesh.materialName = sd.materialName;
    submesh.material = sd.material;

    // Resolve texture paths/payloads to GL textures. Opacity uses Color
    // usage because glTF alpha rides in the base-color texture — this makes
    // it share the diffuse cache entry (same GL id, like the old loader).
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
        data, submesh.material.texOpacityPath, TextureUsage::Color);

    if (sd.hasBounds) {
      mAabbMin = glm::min(mAabbMin, sd.aabbMin);
      mAabbMax = glm::max(mAabbMax, sd.aabbMax);
      mHasBounds = true;
    }

    // Create GL buffers
    static_assert(sizeof(FBXVertex) == sizeof(MeshVertex),
                  "FBXVertex must match MeshVertex layout");
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
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(FBXVertex),
                          (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(FBXVertex),
                          (void *)offsetof(FBXVertex, uv));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(FBXVertex),
                          (void *)offsetof(FBXVertex, normal));

    glBindVertexArray(0);
    mSubmeshes.push_back(submesh);
  }

  return true;
}

bool FBXModel::getGlobalBounds(glm::vec3 &outMin, glm::vec3 &outMax) const {
  if (!mHasBounds)
    return false;
  outMin = mAabbMin;
  outMax = mAabbMax;
  return true;
}

void FBXModel::draw(Shader &shader, const glm::vec3 &pos, const glm::vec3 &rot,
                    const glm::vec3 &scale,
                    const MaterialAsset *materialOverride) {
  glm::mat4 modelMatrix = buildTRS(pos, rot, scale);
  shader.setMat4("model", modelMatrix);

  LOG_TRACE("Render", "Drawing FBX/glTF model submeshes=" +
                          std::to_string(mSubmeshes.size()));

  for (const auto &sm : mSubmeshes) {
    if (sm.vao == 0)
      continue;

    LOG_TRACE("Render",
              "Submesh textures diffuse=" +
                  std::to_string((unsigned long long)sm.material.texDiffuse) +
                  " normal=" +
                  std::to_string((unsigned long long)sm.material.texNormal) +
                  " roughness=" +
                  std::to_string((unsigned long long)sm.material.texRoughness));
    if (materialOverride) {
      applyMaterial(*materialOverride, shader);
    } else {
      applyMaterial(sm.material, shader);
    }

    GLStateCache::instance().bindVertexArray(sm.vao);
    glDrawElements(GL_TRIANGLES, sm.indexCount, GL_UNSIGNED_INT, 0);
  }
}

void FBXModel::drawDepth(Shader &shadowShader, const glm::vec3 &pos,
                         const glm::vec3 &rot, const glm::vec3 &scale) {
  glm::mat4 modelMatrix = buildTRS(pos, rot, scale);
  shadowShader.setMat4("model", modelMatrix);

  for (const auto &sm : mSubmeshes) {
    if (sm.vao == 0 || sm.indexCount <= 0)
      continue;
    GLStateCache::instance().bindVertexArray(sm.vao);
    glDrawElements(GL_TRIANGLES, sm.indexCount, GL_UNSIGNED_INT, 0);
  }

}

void FBXModel::shutdown() {
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
  for (auto &[key, texId] : mTextureCache) {
    if (texId != 0)
      glDeleteTextures(1, &texId);
  }
  mTextureCache.clear();

  mHasBounds = false;
  mAabbMin = glm::vec3(1e30f);
  mAabbMax = glm::vec3(-1e30f);
  mSourcePath.clear();
}
