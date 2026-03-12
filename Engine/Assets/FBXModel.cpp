#include "FBXModel.h"
#include "GLStateCache.h"
#include "Logger.h"
#include "Shader.h"
#include "Texture.h"
#include <stb/stb_image.h>

#include "tiny_gltf.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <map>
#include <vector>

// --- STATIC HELPERS & CACHE ---

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

static int getExtTextureIndex(const tinygltf::Value &extObj,
                              const char *textureInfoKey) {
  if (!extObj.IsObject() || !extObj.Has(textureInfoKey))
    return -1;
  const tinygltf::Value &texInfo = extObj.Get(textureInfoKey);
  if (!texInfo.IsObject() || !texInfo.Has("index"))
    return -1;
  return texInfo.Get("index").GetNumberAsInt();
}

static bool getExtFloat(const tinygltf::Value &extObj, const char *key,
                        float &outValue) {
  if (!extObj.IsObject() || !extObj.Has(key))
    return false;
  const tinygltf::Value &v = extObj.Get(key);
  if (!v.IsNumber())
    return false;
  outValue = (float)v.GetNumberAsDouble();
  return true;
}

static bool getExtVec3(const tinygltf::Value &extObj, const char *key,
                       glm::vec3 &outValue) {
  if (!extObj.IsObject() || !extObj.Has(key))
    return false;
  const tinygltf::Value &v = extObj.Get(key);
  if (!v.IsArray() || v.ArrayLen() < 3)
    return false;
  outValue = glm::vec3((float)v.Get(0).GetNumberAsDouble(),
                       (float)v.Get(1).GetNumberAsDouble(),
                       (float)v.Get(2).GetNumberAsDouble());
  return true;
}

static bool getExtVec4(const tinygltf::Value &extObj, const char *key,
                       glm::vec4 &outValue) {
  if (!extObj.IsObject() || !extObj.Has(key))
    return false;
  const tinygltf::Value &v = extObj.Get(key);
  if (!v.IsArray() || v.ArrayLen() < 4)
    return false;
  outValue = glm::vec4((float)v.Get(0).GetNumberAsDouble(),
                       (float)v.Get(1).GetNumberAsDouble(),
                       (float)v.Get(2).GetNumberAsDouble(),
                       (float)v.Get(3).GetNumberAsDouble());
  return true;
}

// --- CLASS IMPLEMENTATION ---

bool FBXModel::loadFromFile(const std::string &path) {
  shutdown();

  tinygltf::TinyGLTF loader;
  std::string err, warn;
  bool ret = false;

  // Determine base directory
  size_t slash = path.find_last_of("/\\");
  mDirectory = (slash == std::string::npos) ? "." : path.substr(0, slash);

  // Check if binary (.glb) or ASCII (.gltf)
  if (path.find(".glb") != std::string::npos) {
    ret = loader.LoadBinaryFromFile(&mModel, &err, &warn, path);
  } else {
    ret = loader.LoadASCIIFromFile(&mModel, &err, &warn, path);
  }

  if (!warn.empty()) {
    LOG_WARN("Asset", "glTF warning: " + warn);
  }

  if (!err.empty()) {
    LOG_ERROR("Asset", "glTF error: " + err);
  }

  if (!ret) {
    LOG_ERROR("Asset", "Failed to load glTF: " + path);
    return false;
  }

  // Process all scenes (usually just one)
  const tinygltf::Scene &scene =
      mModel.scenes[mModel.defaultScene > -1 ? mModel.defaultScene : 0];

  for (size_t i = 0; i < scene.nodes.size(); i++) {
    processNode(scene.nodes[i]);
  }

  LOG_INFO("Asset", "Loaded glTF: " + path + " with " +
                        std::to_string(mSubmeshes.size()) + " submeshes.");

  // Calculate global bounds across all geometry
  mHasBounds = false;
  mAabbMin = glm::vec3(1e30f);
  mAabbMax = glm::vec3(-1e30f);

  // We do a second fast pass over all generated submeshes rather than tinygltf
  // buffers
  for (const auto &sm : mSubmeshes) {
    // Technically, our vertices are stuck in VBOs right now; we should have
    // computed AABB during processMesh. Let's rely on standard practice: I will
    // update processMesh to accumulate these!
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

void FBXModel::processNode(int nodeIndex) {
  if (nodeIndex < 0 || nodeIndex >= mModel.nodes.size())
    return;

  const tinygltf::Node &node = mModel.nodes[nodeIndex];

  // Process mesh if this node has one
  if (node.mesh >= 0) {
    processMesh(mModel.meshes[node.mesh]);
  }

  // Recursively process children
  for (size_t i = 0; i < node.children.size(); i++) {
    processNode(node.children[i]);
  }
}

void FBXModel::processMesh(const tinygltf::Mesh &mesh) {
  // glTF meshes contain "primitives" (our submeshes)
  for (size_t i = 0; i < mesh.primitives.size(); i++) {
    const tinygltf::Primitive &primitive = mesh.primitives[i];

    std::vector<FBXVertex> vertices;
    std::vector<unsigned int> indices;

    // Get positions
    auto posAttrIt = primitive.attributes.find("POSITION");
    if (posAttrIt == primitive.attributes.end()) {
      LOG_WARN("Asset", "Skipping primitive without POSITION attribute");
      continue;
    }

    const tinygltf::Accessor &posAccessor = mModel.accessors[posAttrIt->second];
    const tinygltf::BufferView &posView =
        mModel.bufferViews[posAccessor.bufferView];
    const tinygltf::Buffer &posBuffer = mModel.buffers[posView.buffer];
    const unsigned char *positions =
        posBuffer.data.data() + posView.byteOffset + posAccessor.byteOffset;
    const int posStride = posAccessor.ByteStride(posView);
    if (posStride <= 0) {
      LOG_WARN("Asset", "Invalid POSITION stride in glTF primitive");
      continue;
    }

    // Get normals (if available)
    const unsigned char *normals = nullptr;
    int normStride = 0;
    auto normalIt = primitive.attributes.find("NORMAL");
    if (normalIt != primitive.attributes.end()) {
      const tinygltf::Accessor &normAccessor =
          mModel.accessors[normalIt->second];
      const tinygltf::BufferView &normView =
          mModel.bufferViews[normAccessor.bufferView];
      const tinygltf::Buffer &normBuffer = mModel.buffers[normView.buffer];
      normals =
          normBuffer.data.data() + normView.byteOffset + normAccessor.byteOffset;
      normStride = normAccessor.ByteStride(normView);
      if (normStride <= 0)
        normals = nullptr;
    }

    // Get UVs (if available)
    const unsigned char *uvs = nullptr;
    int uvStride = 0;
    auto uvIt = primitive.attributes.find("TEXCOORD_0");
    if (uvIt != primitive.attributes.end()) {
      const tinygltf::Accessor &uvAccessor = mModel.accessors[uvIt->second];
      const tinygltf::BufferView &uvView =
          mModel.bufferViews[uvAccessor.bufferView];
      const tinygltf::Buffer &uvBuffer = mModel.buffers[uvView.buffer];
      uvs = uvBuffer.data.data() + uvView.byteOffset + uvAccessor.byteOffset;
      uvStride = uvAccessor.ByteStride(uvView);
      if (uvStride <= 0)
        uvs = nullptr;
    }

    // Build vertices
    for (size_t v = 0; v < posAccessor.count; v++) {
      FBXVertex vertex;
      const float *pos = reinterpret_cast<const float *>(positions + v * posStride);
      vertex.pos = glm::vec3(pos[0], pos[1], pos[2]);

      // Accumulate global AABB bounds
      mAabbMin = glm::min(mAabbMin, vertex.pos);
      mAabbMax = glm::max(mAabbMax, vertex.pos);
      mHasBounds = true;

      if (normals) {
        const float *n = reinterpret_cast<const float *>(normals + v * normStride);
        vertex.normal = glm::vec3(n[0], n[1], n[2]);
      } else {
        vertex.normal = glm::vec3(0, 1, 0);
      }

      if (uvs) {
        const float *uv = reinterpret_cast<const float *>(uvs + v * uvStride);
        vertex.uv = glm::vec2(uv[0], uv[1]);
      } else {
        vertex.uv = glm::vec2(0, 0);
      }

      vertices.push_back(vertex);
    }

    // Get indices
    if (primitive.indices >= 0) {
      const tinygltf::Accessor &indexAccessor = mModel.accessors[primitive.indices];
      const tinygltf::BufferView &indexView =
          mModel.bufferViews[indexAccessor.bufferView];
      const tinygltf::Buffer &indexBuffer = mModel.buffers[indexView.buffer];
      const unsigned char *idxData =
          indexBuffer.data.data() + indexView.byteOffset + indexAccessor.byteOffset;

      if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
        const uint8_t *buf = reinterpret_cast<const uint8_t *>(idxData);
        for (size_t j = 0; j < indexAccessor.count; j++)
          indices.push_back((unsigned int)buf[j]);
      } else if (indexAccessor.componentType ==
                 TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
        const uint16_t *buf = reinterpret_cast<const uint16_t *>(idxData);
        for (size_t j = 0; j < indexAccessor.count; j++)
          indices.push_back((unsigned int)buf[j]);
      } else if (indexAccessor.componentType ==
                 TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
        const uint32_t *buf = reinterpret_cast<const uint32_t *>(idxData);
        for (size_t j = 0; j < indexAccessor.count; j++)
          indices.push_back((unsigned int)buf[j]);
      } else {
        LOG_WARN("Asset", "Unsupported glTF index component type: " +
                              std::to_string(indexAccessor.componentType));
      }
    } else {
      for (unsigned int j = 0; j < (unsigned int)vertices.size(); ++j)
        indices.push_back(j);
    }

    // Create submesh
    FBXSubmesh submesh;
    submesh.indexCount = (GLsizei)indices.size();
    submesh.material.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
    submesh.material.texDiffuse = 0;
    submesh.material.texNormal = 0;
    submesh.material.texRoughness = 0;
    submesh.material.texMetallic = 0;
    submesh.material.texAO = 0;
    submesh.material.texEmissive = 0;
    submesh.material.texOpacity = 0;
    submesh.material.alphaCutoff = 0.0f;
    submesh.material.roughnessMapIsGloss = false;

    // Load material
    if (primitive.material >= 0) {
      const tinygltf::Material &mat = mModel.materials[primitive.material];
      submesh.materialName = mat.name;
      LOG_TRACE("Asset", "Processing material: " + mat.name);

      // Base color
      if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0) {
        submesh.material.texDiffuse = LoadTextureFromGLTF(
            mat.pbrMetallicRoughness.baseColorTexture.index);
        if (submesh.material.texDiffuse != 0) {
          LOG_TRACE("Asset", "Loaded diffuse texture");
        }
      }

      // Base color factor (fallback color)
      auto &colorFactor = mat.pbrMetallicRoughness.baseColorFactor;
      if (colorFactor.size() >= 4) {
        submesh.material.baseColor =
            glm::vec4((float)colorFactor[0], (float)colorFactor[1],
                      (float)colorFactor[2], (float)colorFactor[3]);
      }
      submesh.material.roughness =
          (float)mat.pbrMetallicRoughness.roughnessFactor;
      submesh.material.metallic =
          (float)mat.pbrMetallicRoughness.metallicFactor;
      if (mat.emissiveFactor.size() >= 3) {
        submesh.material.emissiveColor = glm::vec3((float)mat.emissiveFactor[0],
                                                   (float)mat.emissiveFactor[1],
                                                   (float)mat.emissiveFactor[2]);
      }

      // Normal map
      if (mat.normalTexture.index >= 0) {
        submesh.material.texNormal =
            LoadTextureFromGLTF(mat.normalTexture.index);
        if (submesh.material.texNormal != 0) {
          LOG_TRACE("Asset", "Loaded normal texture");
        }
      }

      // Metallic-roughness texture (packed: R=unused, G=roughness, B=metallic)
      if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) {
        submesh.material.texRoughness = LoadTextureFromGLTF(
            mat.pbrMetallicRoughness.metallicRoughnessTexture.index);
        submesh.material.texMetallic =
            submesh.material.texRoughness; // Same texture, different channels
        submesh.material.roughnessChannel = 1; // G
        submesh.material.metallicChannel = 2;  // B
        if (submesh.material.texRoughness != 0) {
          LOG_TRACE("Asset", "Loaded metallic-roughness texture");
        }
      }

      if (mat.occlusionTexture.index >= 0) {
        submesh.material.texAO = LoadTextureFromGLTF(mat.occlusionTexture.index);
        submesh.material.ao = (float)mat.occlusionTexture.strength;
      }

      if (mat.emissiveTexture.index >= 0) {
        submesh.material.texEmissive = LoadTextureFromGLTF(mat.emissiveTexture.index);
      }

      // glTF alpha masking/blending uses base-color alpha.
      if (mat.alphaMode == "MASK") {
        submesh.material.texOpacity = submesh.material.texDiffuse;
        submesh.material.opacityChannel = 3;
        submesh.material.alphaCutoff = (float)mat.alphaCutoff;
      } else if (mat.alphaMode == "BLEND") {
        submesh.material.texOpacity = submesh.material.texDiffuse;
        submesh.material.opacityChannel = 3;
        submesh.material.alphaCutoff = 0.001f;
      }

      // KHR_materials_pbrSpecularGlossiness compatibility fallback
      auto extIt = mat.extensions.find("KHR_materials_pbrSpecularGlossiness");
      if (extIt != mat.extensions.end() && extIt->second.IsObject()) {
        const tinygltf::Value &ext = extIt->second;

        const int diffuseTex = getExtTextureIndex(ext, "diffuseTexture");
        if (diffuseTex >= 0 && submesh.material.texDiffuse == 0) {
          submesh.material.texDiffuse = LoadTextureFromGLTF(diffuseTex);
        }

        glm::vec4 diffuseFactor;
        if (getExtVec4(ext, "diffuseFactor", diffuseFactor)) {
          submesh.material.baseColor = diffuseFactor;
        }

        const int specGlossTex = getExtTextureIndex(ext, "specularGlossinessTexture");
        if (specGlossTex >= 0) {
          GLuint packedTex = LoadTextureFromGLTF(specGlossTex);
          if (packedTex != 0) {
            submesh.material.texRoughness = packedTex;
            submesh.material.texMetallic = packedTex;
            submesh.material.roughnessChannel = 3; // A=glossiness
            submesh.material.metallicChannel = 2;  // B≈specular intensity proxy
            submesh.material.roughnessMapIsGloss = true;
          }
        }

        float glossiness = 0.0f;
        if (getExtFloat(ext, "glossinessFactor", glossiness)) {
          submesh.material.roughness = std::clamp(1.0f - glossiness, 0.04f, 1.0f);
        }

        glm::vec3 specularFactor;
        if (getExtVec3(ext, "specularFactor", specularFactor)) {
          float maxSpec = std::max(specularFactor.x,
                                   std::max(specularFactor.y, specularFactor.z));
          submesh.material.metallic =
              std::clamp((maxSpec - 0.04f) / 0.96f, 0.0f, 1.0f);
        }
      }
    }
    if (submesh.materialName.empty())
      submesh.materialName = "glTFMaterial_" + std::to_string(i);
    submesh.material.id = submesh.materialName;

    // Create GL buffers
    glGenVertexArrays(1, &submesh.vao);
    glGenBuffers(1, &submesh.vbo);
    glGenBuffers(1, &submesh.ebo);

    glBindVertexArray(submesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, submesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(FBXVertex),
                 vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, submesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int),
                 indices.data(), GL_STATIC_DRAW);

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
}

GLuint FBXModel::LoadTextureFromGLTF(int textureIndex) {
  if (textureIndex < 0 || textureIndex >= mModel.textures.size())
    return 0;

  const tinygltf::Texture &tex = mModel.textures[textureIndex];
  if (tex.source < 0 || tex.source >= mModel.images.size())
    return 0;

  const tinygltf::Image &image = mModel.images[tex.source];

  // Check cache
  std::string key = image.uri.empty()
                        ? ("embedded_" + std::to_string(tex.source))
                        : image.uri;
  if (mTextureCache.find(key) != mTextureCache.end()) {
    return mTextureCache[key];
  }

  GLuint texID = CreateTextureFromImage(image);

  if (texID != 0) {
    mTextureCache[key] = texID;
    if (image.uri.empty()) {
      LOG_TRACE("Asset", "Loaded embedded texture from glTF");
    } else {
      LOG_TRACE("Asset", "Loaded texture file: " + image.uri);
    }
  }

  return texID;
}

GLuint FBXModel::CreateTextureFromImage(const tinygltf::Image &image) {
  if (image.width <= 0 || image.height <= 0 || image.image.empty())
    return 0;

  GLuint texID = 0;
  glGenTextures(1, &texID);
  glBindTexture(GL_TEXTURE_2D, texID);

  GLenum format = GL_RGBA;
  if (image.component == 3) {
    format = GL_RGB;
  } else if (image.component == 1) {
    format = GL_RED;
  }

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  glTexImage2D(GL_TEXTURE_2D, 0, format, image.width, image.height, 0, format,
               GL_UNSIGNED_BYTE, image.image.data());
  glGenerateMipmap(GL_TEXTURE_2D);

  return texID;
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
      materialOverride->apply(shader);
    } else {
      sm.material.apply(shader);
    }

    GLStateCache::instance().bindVertexArray(sm.vao);
    glDrawElements(GL_TRIANGLES, sm.indexCount, GL_UNSIGNED_INT, 0);
  }
  GLStateCache::instance().bindVertexArray(0);
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

  GLStateCache::instance().bindVertexArray(0);
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

  // tinygltf::Model cleans up automatically
}
