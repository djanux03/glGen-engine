#include "OBJModel.h"
#include "GLStateCache.h"
#include "Logger.h"
#include "Shader.h"
#include "Texture.h"
#include <algorithm>
#include <cctype>
#include <glm/gtc/matrix_transform.hpp>
#include <unordered_map>

// tinyobjloader
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

struct ObjectTRSOverride {
  glm::vec3 posLocal{0.0f};
  glm::vec3 rotDegLocal{0.0f};
  glm::vec3 scaleLocal{1.0f};

  bool hasPivot = false;
  glm::vec3 pivotLocal{0.0f};

  bool enabled = false; // only apply if true
};

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

static std::string getDir(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  return (slash == std::string::npos) ? std::string("./")
                                      : path.substr(0, slash + 1);
}

static std::string joinPath(const std::string &a, const std::string &b) {
  if (a.empty())
    return b;
  char last = a.back();
  if (last == '/' || last == '\\')
    return a + b;
  return a + "/" + b;
}

static bool isAbsolutePath(const std::string &p) {
  if (p.empty())
    return false;
  if (p[0] == '/' || p[0] == '\\')
    return true;

  if (p.size() >= 3 && std::isalpha((unsigned char)p[0]) && p[1] == ':' &&
      (p[2] == '\\' || p[2] == '/'))
    return true;

  return false;
}

static std::string lastToken(const std::string &s) {
  size_t end = s.find_last_not_of(" \t\r\n");
  if (end == std::string::npos)
    return {};
  size_t start = s.find_last_of(" \t\r\n", end);
  if (start == std::string::npos)
    return s.substr(0, end + 1);
  return s.substr(start + 1, end - start);
}

static std::string pickSingleTextureName(const tinyobj::material_t &m) {
  if (!m.diffuse_texname.empty())
    return m.diffuse_texname;
  if (!m.specular_texname.empty())
    return m.specular_texname;
  if (!m.normal_texname.empty())
    return m.normal_texname;
  if (!m.bump_texname.empty())
    return m.bump_texname;

  auto it = m.unknown_parameter.find("map_refl");
  if (it != m.unknown_parameter.end())
    return lastToken(it->second);

  return {};
}

bool OBJModel::loadFromFile(const std::string &objPath) {
  shutdown();

  std::string baseDir = getDir(objPath);

  tinyobj::attrib_t attrib;
  std::vector<tinyobj::shape_t> shapes;
  std::vector<tinyobj::material_t> materials;
  std::string warn, err;

  bool ok = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err,
                             objPath.c_str(), baseDir.c_str(), true);

  if (!warn.empty())
    LOG_WARN("Asset", "OBJ warn: " + warn);
  if (!err.empty())
    LOG_ERROR("Asset", "OBJ error: " + err);
  if (!ok)
    return false;

  // Pre-load material GPU data once
  struct MatGPU {
    std::string name;
    glm::vec3 kd;
    GLuint tex;
  };
  std::vector<MatGPU> matGpu;
  if (!materials.empty()) {
    matGpu.resize(materials.size());
    for (size_t i = 0; i < materials.size(); ++i) {
      const auto &m = materials[i];
      matGpu[i].name = m.name;
      matGpu[i].kd = glm::vec3((float)m.diffuse[0], (float)m.diffuse[1],
                               (float)m.diffuse[2]);
      matGpu[i].tex = 0;

      std::string texName = pickSingleTextureName(m);
      if (!texName.empty()) {
        std::string texPath =
            isAbsolutePath(texName) ? texName : joinPath(baseDir, texName);
        matGpu[i].tex = LoadTexture2D(texPath.c_str());
        if (matGpu[i].tex == 0)
          LOG_WARN("Asset", "Failed to load texture: " + texPath);
      }
    }
  }

  std::unordered_map<std::string, size_t> submeshIndexByKey;
  std::vector<std::vector<Vertex>> vertsPerSubmesh;

  auto ensureSubmesh = [&](const std::string &objectName, int matId) -> size_t {
    std::string materialName = "Default";
    glm::vec3 kd(1.0f);
    GLuint tex = 0;

    if (!materials.empty() && matId >= 0 && matId < (int)materials.size()) {
      materialName = matGpu[(size_t)matId].name;
      kd = matGpu[(size_t)matId].kd;
      tex = matGpu[(size_t)matId].tex;
    }

    std::string key = makeKey(objectName, materialName);
    auto it = submeshIndexByKey.find(key);
    if (it != submeshIndexByKey.end())
      return it->second;

    Submesh sm;
    sm.objectName = objectName;
    sm.materialName = materialName;
    sm.debugName = objectName + " / " + materialName;
    sm.material.baseColor = glm::vec4(kd, 1.0f);
    sm.material.texDiffuse = tex;
    sm.material.id = sm.debugName;

    size_t idx = mSubmeshes.size();
    mSubmeshes.push_back(sm);
    vertsPerSubmesh.emplace_back();
    submeshIndexByKey[key] = idx;
    return idx;
  };

  auto expandBounds = [](glm::vec3 &mn, glm::vec3 &mx, bool &has,
                         const glm::vec3 &p) {
    mn = glm::min(mn, p);
    mx = glm::max(mx, p);
    has = true;
  };

  for (const auto &shape : shapes) {
    // tinyobj shape.name typically corresponds to OBJ object/group naming
    const std::string objName =
        shape.name.empty() ? "DefaultObject" : shape.name;

    size_t index_offset = 0;
    for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++) {
      int fv = (int)shape.mesh.num_face_vertices[f];

      int matId = -1;
      if (f < shape.mesh.material_ids.size())
        matId = shape.mesh.material_ids[f];

      size_t subIdx = ensureSubmesh(objName, matId);
      auto &sm = mSubmeshes[subIdx];

      // triangles expected (triangulate=true)
      if (fv == 3) {
        Vertex face[3]{};
        bool hasAllNormals = true;

        for (int v = 0; v < 3; v++) {
          tinyobj::index_t idx = shape.mesh.indices[index_offset + v];
          Vertex vert{};

          vert.pos.x = attrib.vertices[3 * idx.vertex_index + 0];
          vert.pos.y = attrib.vertices[3 * idx.vertex_index + 1];
          vert.pos.z = attrib.vertices[3 * idx.vertex_index + 2];

          if (idx.texcoord_index >= 0 && !attrib.texcoords.empty()) {
            vert.uv.x = attrib.texcoords[2 * idx.texcoord_index + 0];
            vert.uv.y = attrib.texcoords[2 * idx.texcoord_index + 1];
          } else
            vert.uv = glm::vec2(0.0f);

          if (idx.normal_index >= 0 && !attrib.normals.empty()) {
            vert.normal.x = attrib.normals[3 * idx.normal_index + 0];
            vert.normal.y = attrib.normals[3 * idx.normal_index + 1];
            vert.normal.z = attrib.normals[3 * idx.normal_index + 2];
          } else {
            vert.normal = glm::vec3(0.0f);
            hasAllNormals = false;
          }

          face[v] = vert;
        }

        // bounds: submesh + object
        expandBounds(sm.aabbMin, sm.aabbMax, sm.hasBounds, face[0].pos);
        expandBounds(sm.aabbMin, sm.aabbMax, sm.hasBounds, face[1].pos);
        expandBounds(sm.aabbMin, sm.aabbMax, sm.hasBounds, face[2].pos);

        auto &ob = mObjectBounds[objName];
        expandBounds(ob.aabbMin, ob.aabbMax, ob.hasBounds, face[0].pos);
        expandBounds(ob.aabbMin, ob.aabbMax, ob.hasBounds, face[1].pos);
        expandBounds(ob.aabbMin, ob.aabbMax, ob.hasBounds, face[2].pos);

        if (!hasAllNormals) {
          glm::vec3 e1 = face[1].pos - face[0].pos;
          glm::vec3 e2 = face[2].pos - face[0].pos;
          glm::vec3 n = glm::normalize(glm::cross(e1, e2));
          if (!glm::any(glm::isnan(n)))
            face[0].normal = face[1].normal = face[2].normal = n;
          else
            face[0].normal = face[1].normal = face[2].normal =
                glm::vec3(0, 1, 0);
        }

        vertsPerSubmesh[subIdx].push_back(face[0]);
        vertsPerSubmesh[subIdx].push_back(face[1]);
        vertsPerSubmesh[subIdx].push_back(face[2]);
      }

      index_offset += fv;
    }
  }

  // Upload each (object/material) submesh
  for (size_t i = 0; i < mSubmeshes.size(); ++i) {
    auto &sm = mSubmeshes[i];
    auto &verts = vertsPerSubmesh[i];

    sm.vertexCount = (GLsizei)verts.size();
    if (sm.vertexCount == 0)
      continue;

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

    LOG_TRACE("Asset",
              "OBJ submesh '" + sm.debugName +
                  "' verts=" + std::to_string(sm.vertexCount) + " tex=" +
                  std::to_string((unsigned long long)sm.material.texDiffuse));
  }

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

  GLStateCache::instance().bindVertexArray(0);
}

void OBJModel::draw(Shader &shader, const glm::vec3 &position,
                    const glm::vec3 &rotDeg, const glm::vec3 &scale) {
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

    sm.material.apply(shader);
    GLStateCache::instance().bindVertexArray(sm.vao);
    glDrawArrays(GL_TRIANGLES, 0, sm.vertexCount);
  }

  GLStateCache::instance().bindVertexArray(0);
}
