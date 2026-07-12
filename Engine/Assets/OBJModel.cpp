#include "OBJModel.h"
#include "GLStateCache.h"
#include "Logger.h"
#include "MeshData.h"
#include "MeshParse.h"
#include "Shader.h"
#include "Texture.h"
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <unordered_map>

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
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4),
                        (void *)0);
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4),
                        (void *)(sizeof(glm::vec4)));
  glEnableVertexAttribArray(5);
  glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4),
                        (void *)(2 * sizeof(glm::vec4)));
  glEnableVertexAttribArray(6);
  glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4),
                        (void *)(3 * sizeof(glm::vec4)));

  glVertexAttribDivisor(3, 1);
  glVertexAttribDivisor(4, 1);
  glVertexAttribDivisor(5, 1);
  glVertexAttribDivisor(6, 1);
}

static void uploadShadowOnlyMesh(GLuint &vao, GLuint &vbo, GLsizei &vertexCount,
                                 const std::vector<glm::vec3> &positions) {
  if (positions.empty())
    return;

  if (vbo != 0)
    glDeleteBuffers(1, &vbo);
  if (vao != 0)
    glDeleteVertexArrays(1, &vao);

  glGenVertexArrays(1, &vao);
  glGenBuffers(1, &vbo);

  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, positions.size() * sizeof(glm::vec3),
               positions.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3),
                        (void *)0);
  glBindVertexArray(0);

  vertexCount = static_cast<GLsizei>(positions.size());
}

// Creates GL textures for every texture path recorded at parse time.
// OBJ/MTL textures go through the shared global cache (they are shared
// between models and never deleted per-model).
static void resolveMaterialTextures(MaterialAsset &mat) {
  auto load = [](uint32_t &id, const std::string &path, TextureUsage usage) {
    if (id == 0 && !path.empty())
      id = LoadTexture2DCached(path, true, usage);
  };
  load(mat.texDiffuse, mat.texDiffusePath, TextureUsage::Color);
  load(mat.texNormal, mat.texNormalPath, TextureUsage::Data);
  load(mat.texRoughness, mat.texRoughnessPath, TextureUsage::Data);
  load(mat.texMetallic, mat.texMetallicPath, TextureUsage::Data);
  load(mat.texAO, mat.texAOPath, TextureUsage::Data);
  load(mat.texEmissive, mat.texEmissivePath, TextureUsage::Color);
  load(mat.texOpacity, mat.texOpacityPath, TextureUsage::Data);
}

bool OBJModel::loadFromFile(const std::string &objPath) {
  auto data = parseMeshOBJ(objPath);
  if (!data)
    return false;
  return loadFromData(*data);
}

bool OBJModel::loadFromData(const MeshData &data) {
  shutdown();

  std::vector<glm::vec3> shadowVertices;
  std::vector<Vertex> expanded;

  for (const auto &sd : data.submeshes) {
    Submesh sm;
    sm.objectName = sd.objectName;
    sm.materialName = sd.materialName;
    sm.debugName = sd.debugName;
    sm.material = sd.material;
    sm.aabbMin = sd.aabbMin;
    sm.aabbMax = sd.aabbMax;
    sm.hasBounds = sd.hasBounds;
    resolveMaterialTextures(sm.material);

    // Flatten indexed data — OBJModel draws non-indexed triangle lists.
    expanded.clear();
    if (!sd.indices.empty()) {
      expanded.reserve(sd.indices.size());
      for (uint32_t idx : sd.indices) {
        const MeshVertex &v = sd.vertices[idx];
        expanded.push_back({v.pos, v.uv, v.normal});
      }
    } else {
      expanded.reserve(sd.vertices.size());
      for (const auto &v : sd.vertices)
        expanded.push_back({v.pos, v.uv, v.normal});
    }

    sm.vertexCount = (GLsizei)expanded.size();
    if (sm.vertexCount > 0) {
      glGenVertexArrays(1, &sm.vao);
      glGenBuffers(1, &sm.vbo);

      glBindVertexArray(sm.vao);
      glBindBuffer(GL_ARRAY_BUFFER, sm.vbo);
      glBufferData(GL_ARRAY_BUFFER, expanded.size() * sizeof(Vertex),
                   expanded.data(), GL_STATIC_DRAW);

      glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                            (void *)offsetof(Vertex, pos));
      glEnableVertexAttribArray(0);

      glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                            (void *)offsetof(Vertex, uv));
      glEnableVertexAttribArray(1);

      glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                            (void *)offsetof(Vertex, normal));
      glEnableVertexAttribArray(2);

      glBindVertexArray(0);

      for (const auto &v : expanded)
        shadowVertices.push_back(v.pos);

      LOG_TRACE("Asset",
                "OBJ submesh '" + sm.debugName +
                    "' verts=" + std::to_string(sm.vertexCount) + " tex=" +
                    std::to_string((unsigned long long)sm.material.texDiffuse));
    }

    mSubmeshes.push_back(sm);
  }

  for (const auto &entry : data.objectBounds) {
    ObjectBounds ob;
    ob.aabbMin = entry.second.aabbMin;
    ob.aabbMax = entry.second.aabbMax;
    ob.hasBounds = entry.second.hasBounds;
    mObjectBounds[entry.first] = ob;
  }

  uploadShadowOnlyMesh(mShadowMesh.vao, mShadowMesh.vbo,
                       mShadowMesh.vertexCount, shadowVertices);
  mShadowMesh.instancingReady = false;
  mShadowMesh.instancedVBO = 0;

  return true;
}

bool OBJModel::loadFromVertices(const std::vector<VertexData> &vertices,
                                const std::string &name) {
  if (vertices.empty())
    return false;

  // Convert VertexData → internal Vertex (same layout)
  std::vector<Vertex> verts(vertices.size());
  std::vector<glm::vec3> shadowVertices;
  glm::vec3 bMin(1e30f), bMax(-1e30f);
  for (size_t i = 0; i < vertices.size(); ++i) {
    verts[i].pos = vertices[i].pos;
    verts[i].uv = vertices[i].uv;
    verts[i].normal = vertices[i].normal;
    shadowVertices.push_back(vertices[i].pos);
    bMin = glm::min(bMin, vertices[i].pos);
    bMax = glm::max(bMax, vertices[i].pos);
  }

  // Create single submesh
  Submesh sm;
  sm.objectName = name;
  sm.materialName = "default";
  sm.debugName = name;
  sm.vertexCount = (GLsizei)verts.size();
  sm.aabbMin = bMin;
  sm.aabbMax = bMax;
  sm.hasBounds = true;

  // White material (no texture)
  sm.material.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
  sm.material.texDiffuse = 0;
  sm.material.texNormal = 0;

  // Upload to GPU
  glGenVertexArrays(1, &sm.vao);
  glGenBuffers(1, &sm.vbo);

  glBindVertexArray(sm.vao);
  glBindBuffer(GL_ARRAY_BUFFER, sm.vbo);
  glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(Vertex), verts.data(),
               GL_STATIC_DRAW);

  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        (void *)offsetof(Vertex, pos));
  glEnableVertexAttribArray(0);

  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        (void *)offsetof(Vertex, uv));
  glEnableVertexAttribArray(1);

  glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        (void *)offsetof(Vertex, normal));
  glEnableVertexAttribArray(2);

  glBindVertexArray(0);

  mSubmeshes.push_back(sm);

  // Object-level bounds
  ObjectBounds ob;
  ob.aabbMin = bMin;
  ob.aabbMax = bMax;
  ob.hasBounds = true;
  mObjectBounds[name] = ob;

  uploadShadowOnlyMesh(mShadowMesh.vao, mShadowMesh.vbo,
                       mShadowMesh.vertexCount, shadowVertices);
  mShadowMesh.instancingReady = false;
  mShadowMesh.instancedVBO = 0;

  return true;
}
void OBJModel::setObjectYawDeg(const std::string &objectName, float yawDeg) {
  auto &o = mYawOverride[objectName];
  o.yawDeg = yawDeg;
  o.hasPivot = false; // use default pivot (object center)
}

void OBJModel::setObjectYawDegPivot(const std::string &objectName, float yawDeg,
                                    const glm::vec3 &pivotLocal) {
  auto &o = mYawOverride[objectName];
  o.yawDeg = yawDeg;
  o.pivotLocal = pivotLocal;
  o.hasPivot = true; // force this pivot
}

void OBJModel::clearObjectOverrides() { mYawOverride.clear(); }

bool OBJModel::getObjectCenterLocal(const std::string &objectName,
                                    glm::vec3 &outCenter) const {
  auto it = mObjectBounds.find(objectName);
  if (it == mObjectBounds.end())
    return false;
  const auto &b = it->second;
  if (!b.hasBounds)
    return false;
  outCenter = (b.aabbMin + b.aabbMax) * 0.5f;
  return true;
}
std::vector<std::string> OBJModel::objectNames() const {
  std::vector<std::string> out;
  out.reserve(mObjectBounds.size());
  for (const auto &kv : mObjectBounds)
    out.push_back(kv.first);
  std::sort(out.begin(), out.end());
  return out;
}

bool OBJModel::getObjectBounds(const std::string &objectName, glm::vec3 &outMin,
                               glm::vec3 &outMax) const {
  auto it = mObjectBounds.find(objectName);
  if (it == mObjectBounds.end())
    return false;
  const auto &b = it->second;
  if (!b.hasBounds)
    return false;
  outMin = b.aabbMin;
  outMax = b.aabbMax;
  return true;
}

bool OBJModel::getGlobalBounds(glm::vec3 &outMin, glm::vec3 &outMax) const {
  if (mSubmeshes.empty())
    return false;

  glm::vec3 globalMin(1e30f);
  glm::vec3 globalMax(-1e30f);
  bool hasAnyBounds = false;

  for (const auto &sm : mSubmeshes) {
    if (sm.hasBounds) {
      globalMin = glm::min(globalMin, sm.aabbMin);
      globalMax = glm::max(globalMax, sm.aabbMax);
      hasAnyBounds = true;
    }
  }

  if (!hasAnyBounds)
    return false;

  outMin = globalMin;
  outMax = globalMax;
  return true;
}

void OBJModel::centerAtOrigin(UpAxis upAxis) {
  // Compute global AABB across all submeshes
  glm::vec3 globalMin(1e30f), globalMax(-1e30f);
  bool hasAny = false;
  for (const auto &sm : mSubmeshes) {
    if (sm.hasBounds) {
      globalMin = glm::min(globalMin, sm.aabbMin);
      globalMax = glm::max(globalMax, sm.aabbMax);
      hasAny = true;
    }
  }
  if (!hasAny)
    return;

  glm::vec3 center = (globalMin + globalMax) * 0.5f;
  glm::vec3 offset(0.0f);
  switch (upAxis) {
  case UpAxis::Y:
    // Base at Y=0, center XZ
    offset = glm::vec3(-center.x, -globalMin.y, -center.z);
    break;
  case UpAxis::Z:
    // Base at Z=0, center XY
    offset = glm::vec3(-center.x, -center.y, -globalMin.z);
    break;
  case UpAxis::X:
    // Base at X=0, center YZ
    offset = glm::vec3(-globalMin.x, -center.y, -center.z);
    break;
  }

  if (glm::length(offset) < 1e-4f)
    return; // already at origin, nothing to do

  // Re-map each submesh VBO and shift positions
  for (auto &sm : mSubmeshes) {
    if (sm.vbo == 0 || sm.vertexCount == 0)
      continue;

    glBindBuffer(GL_ARRAY_BUFFER, sm.vbo);
    GLsizeiptr size = sm.vertexCount * (GLsizeiptr)sizeof(Vertex);
    Vertex *verts = reinterpret_cast<Vertex *>(glMapBufferRange(
        GL_ARRAY_BUFFER, 0, size, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT));
    if (!verts) {
      glBindBuffer(GL_ARRAY_BUFFER, 0);
      continue;
    }

    for (GLsizei i = 0; i < sm.vertexCount; ++i) {
      verts[i].pos += offset;
    }

    glUnmapBuffer(GL_ARRAY_BUFFER);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Update CPU-side AABB for this submesh
    sm.aabbMin += offset;
    sm.aabbMax += offset;
  }

  // Update object-level bounds too
  for (auto &[name, ob] : mObjectBounds) {
    if (ob.hasBounds) {
      ob.aabbMin += offset;
      ob.aabbMax += offset;
    }
  }

  if (mShadowMesh.vbo != 0 && mShadowMesh.vertexCount > 0) {
    glBindBuffer(GL_ARRAY_BUFFER, mShadowMesh.vbo);
    GLsizeiptr size =
        mShadowMesh.vertexCount * (GLsizeiptr)sizeof(glm::vec3);
    glm::vec3 *positions = reinterpret_cast<glm::vec3 *>(glMapBufferRange(
        GL_ARRAY_BUFFER, 0, size, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT));
    if (positions) {
      for (GLsizei i = 0; i < mShadowMesh.vertexCount; ++i) {
        positions[i] += offset;
      }
      glUnmapBuffer(GL_ARRAY_BUFFER);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  }
}

bool OBJModel::getObjectLocalTRS(const std::string &objectName,
                                 glm::vec3 &outPos, glm::vec3 &outRotDeg,
                                 glm::vec3 &outScale) const {
  auto it = mObjectTRS.find(objectName);
  if (it == mObjectTRS.end())
    return false;
  const auto &o = it->second;
  if (!o.enabled)
    return false;

  outPos = o.posLocal;
  outRotDeg = o.rotDegLocal;
  outScale = o.scaleLocal;
  return true;
}

void OBJModel::setObjectLocalTRS(const std::string &objectName,
                                 const glm::vec3 &pos, const glm::vec3 &rotDeg,
                                 const glm::vec3 &scale) {
  auto &o = mObjectTRS[objectName];
  o.posLocal = pos;
  o.rotDegLocal = rotDeg;
  o.scaleLocal = scale;
  o.enabled = true;
}

void OBJModel::clearObjectLocalTRS(const std::string &objectName) {
  auto it = mObjectTRS.find(objectName);
  if (it != mObjectTRS.end())
    mObjectTRS.erase(it);
}

void OBJModel::clearAllObjectLocalTRS() { mObjectTRS.clear(); }

// Backwards-compat: material-based center (same as before)
bool OBJModel::getSubmeshCenterLocal(const std::string &materialName,
                                     glm::vec3 &outCenter) const {
  for (const auto &sm : mSubmeshes) {
    if (sm.materialName == materialName && sm.hasBounds) {
      outCenter = (sm.aabbMin + sm.aabbMax) * 0.5f;
      return true;
    }
  }
  return false;
}

void OBJModel::shutdown() {
  if (mShadowMesh.vbo)
    glDeleteBuffers(1, &mShadowMesh.vbo);
  if (mShadowMesh.vao)
    glDeleteVertexArrays(1, &mShadowMesh.vao);
  mShadowMesh = {};

  for (auto &sm : mSubmeshes) {
    if (sm.vbo)
      glDeleteBuffers(1, &sm.vbo);
    if (sm.vao)
      glDeleteVertexArrays(1, &sm.vao);
    // IMPORTANT: textures are shared between multiple submeshes now (same
    // material) so DO NOT delete sm.tex here (or you’ll double-delete).
    sm = {};
  }
  mSubmeshes.clear();
  mObjectBounds.clear();
  mYawOverride.clear();
  mObjectTRS.clear();
}

static glm::mat4 buildTR(const glm::vec3 &position, const glm::vec3 &rotDeg) {
  glm::mat4 m(1.0f);
  m = glm::translate(m, position);
  m = glm::rotate(m, glm::radians(rotDeg.y), glm::vec3(0, 1, 0));
  m = glm::rotate(m, glm::radians(rotDeg.x), glm::vec3(1, 0, 0));
  m = glm::rotate(m, glm::radians(rotDeg.z), glm::vec3(0, 0, 1));
  return m;
}
glm::mat4 OBJModel::buildObjectExtra(const std::string &objectName) const {
  glm::mat4 extra(1.0f);

  // --- 1) Existing yaw override (keep your behavior) ---
  auto itYaw = mYawOverride.find(objectName);
  if (itYaw != mYawOverride.end() && itYaw->second.yawDeg != 0.0f) {
    glm::vec3 pivot(0.0f);
    bool hasPivot = false;

    if (itYaw->second.hasPivot) {
      pivot = itYaw->second.pivotLocal;
      hasPivot = true;
    } else {
      hasPivot = getObjectCenterLocal(objectName, pivot);
    }

    if (hasPivot) {
      extra = glm::translate(glm::mat4(1.0f), pivot) *
              glm::rotate(glm::mat4(1.0f), glm::radians(itYaw->second.yawDeg),
                          glm::vec3(0, 1, 0)) *
              glm::translate(glm::mat4(1.0f), -pivot);
    }
  }

  // --- 2) New full local TRS override (editor gizmo) ---
  auto it = mObjectTRS.find(objectName);
  if (it != mObjectTRS.end() && it->second.enabled) {
    glm::vec3 pivot = it->second.pivotLocal;
    if (!it->second.hasPivot)
      (void)getObjectCenterLocal(objectName, pivot);

    glm::mat4 localTRS = buildTRS(it->second.posLocal, it->second.rotDegLocal,
                                  it->second.scaleLocal);

    // Apply TRS about pivot (so rotate/scale feel correct)
    glm::mat4 aboutPivot = glm::translate(glm::mat4(1.0f), pivot) * localTRS *
                           glm::translate(glm::mat4(1.0f), -pivot);

    extra = extra * aboutPivot;
  }

  return extra;
}

void OBJModel::drawDepth(Shader &shadowShader, const glm::vec3 &position,
                         const glm::vec3 &rotDeg, const glm::vec3 &scale) {
  // IMPORTANT: uniforms require the program to be active; ensure the caller
  // activated it, or uncomment the next line if your Shader class has
  // activate(). [web:2157] shadowShader.activate();

  glm::mat4 TR = buildTR(position, rotDeg);
  glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);

  for (auto &sm : mSubmeshes) {
    if (sm.vertexCount == 0 || sm.vao == 0)
      continue;

    glm::mat4 extra = buildObjectExtra(sm.objectName); // yaw + editor TRS
    glm::mat4 model = TR * extra * S; // TRS order matters [web:2214]

    shadowShader.setMat4("model", model);

    GLStateCache::instance().bindVertexArray(sm.vao);
    glDrawArrays(GL_TRIANGLES, 0, sm.vertexCount);
  }
}

void OBJModel::draw(Shader &shader, const glm::vec3 &position,
                    const glm::vec3 &rotDeg, const glm::vec3 &scale,
                    const MaterialAsset *materialOverride) {
  // Pass-level uniforms (uGlowPass, uCloudPass, texture1) are set once
  // per frame by RenderSystem — no need to repeat here.

  glm::mat4 TR = buildTR(position, rotDeg);
  glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);

  for (auto &sm : mSubmeshes) {
    if (sm.vertexCount == 0 || sm.vao == 0)
      continue;

    glm::mat4 extra = buildObjectExtra(sm.objectName); // yaw + editor TRS
    glm::mat4 model = TR * extra * S; // TRS order matters [web:2214]
    shader.setMat4("model", model);

    if (materialOverride) {
      applyMaterial(*materialOverride, shader);
    } else {
      applyMaterial(sm.material, shader);
    }
    GLStateCache::instance().bindVertexArray(sm.vao);
    glDrawArrays(GL_TRIANGLES, 0, sm.vertexCount);
  }
}

void OBJModel::drawInstanced(Shader &shader, unsigned int instanceVBO,
                             int instanceCount) {
  if (instanceCount == 0)
    return;

  for (auto &sm : mSubmeshes) {
    if (sm.vertexCount == 0 || sm.vao == 0)
      continue;

    applyMaterial(sm.material, shader);
    GLStateCache::instance().bindVertexArray(sm.vao);

    if (!sm.instancingReady || sm.instancedVBO != instanceVBO) {
      // Setup instanced attributes once per VAO/VBO pair
      bindInstanceMatrixAttributes(instanceVBO);
      sm.instancedVBO = instanceVBO;
      sm.instancingReady = true;
    }

    glDrawArraysInstanced(GL_TRIANGLES, 0, sm.vertexCount, instanceCount);
  }
}

void OBJModel::drawDepthInstanced(Shader &shadowShader,
                                  unsigned int instanceVBO, int instanceCount) {
  if (instanceCount == 0)
    return;

  const bool canUseCombinedShadowMesh =
      mShadowMesh.vao != 0 && mShadowMesh.vertexCount > 0 &&
      mYawOverride.empty() && mObjectTRS.empty();
  if (canUseCombinedShadowMesh) {
    GLStateCache::instance().bindVertexArray(mShadowMesh.vao);

    if (!mShadowMesh.instancingReady || mShadowMesh.instancedVBO != instanceVBO) {
      bindInstanceMatrixAttributes(instanceVBO);
      mShadowMesh.instancedVBO = instanceVBO;
      mShadowMesh.instancingReady = true;
    }

    glDrawArraysInstanced(GL_TRIANGLES, 0, mShadowMesh.vertexCount,
                          instanceCount);
    return;
  }

  for (auto &sm : mSubmeshes) {
    if (sm.vertexCount == 0 || sm.vao == 0)
      continue;

    GLStateCache::instance().bindVertexArray(sm.vao);

    if (!sm.instancingReady || sm.instancedVBO != instanceVBO) {
      // Setup instanced attributes once per VAO/VBO pair
      bindInstanceMatrixAttributes(instanceVBO);
      sm.instancedVBO = instanceVBO;
      sm.instancingReady = true;
    }

    glDrawArraysInstanced(GL_TRIANGLES, 0, sm.vertexCount, instanceCount);
  }
}
