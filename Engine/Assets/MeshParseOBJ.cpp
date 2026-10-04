#include "MeshNormals.h"
#include "Logger.h"
#include "MeshParse.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <unordered_map>

// tinyobjloader
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

namespace {

std::string getDir(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  return (slash == std::string::npos) ? std::string("./")
                                      : path.substr(0, slash + 1);
}

std::string joinPath(const std::string &a, const std::string &b) {
  if (a.empty())
    return b;
  char last = a.back();
  if (last == '/' || last == '\\')
    return a + b;
  return a + "/" + b;
}

bool isAbsolutePath(const std::string &p) {
  if (p.empty())
    return false;
  if (p[0] == '/' || p[0] == '\\')
    return true;

  if (p.size() >= 3 && std::isalpha((unsigned char)p[0]) && p[1] == ':' &&
      (p[2] == '\\' || p[2] == '/'))
    return true;

  return false;
}

bool fileExists(const std::string &path) {
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::path(path), ec);
}

std::string lastToken(const std::string &s) {
  size_t end = s.find_last_not_of(" \t\r\n");
  if (end == std::string::npos)
    return {};
  size_t start = s.find_last_of(" \t\r\n", end);
  if (start == std::string::npos)
    return s.substr(0, end + 1);
  return s.substr(start + 1, end - start);
}

std::string findUnknownTex(const tinyobj::material_t &m, const char *key) {
  auto it = m.unknown_parameter.find(key);
  if (it == m.unknown_parameter.end())
    return {};
  return lastToken(it->second);
}

bool findUnknownFloat(const tinyobj::material_t &m, const char *key,
                      float &outValue) {
  auto it = m.unknown_parameter.find(key);
  if (it == m.unknown_parameter.end())
    return false;
  const std::string token = lastToken(it->second);
  if (token.empty())
    return false;
  try {
    outValue = std::stof(token);
    return true;
  } catch (...) {
    return false;
  }
}

std::string pickDiffuseTextureName(const tinyobj::material_t &m) {
  if (!m.diffuse_texname.empty())
    return m.diffuse_texname;
  if (!m.specular_texname.empty())
    return m.specular_texname;
  return {};
}

std::string pickNormalTextureName(const tinyobj::material_t &m) {
  if (!m.normal_texname.empty())
    return m.normal_texname;
  if (!m.bump_texname.empty())
    return m.bump_texname;
  return {};
}

std::string pickRoughnessTextureName(const tinyobj::material_t &m) {
  if (!m.roughness_texname.empty())
    return m.roughness_texname;
  std::string unknown = findUnknownTex(m, "map_Pr");
  if (!unknown.empty())
    return unknown;
  unknown = findUnknownTex(m, "map_pr");
  if (!unknown.empty())
    return unknown;
  return {};
}

std::string pickGlossinessTextureName(const tinyobj::material_t &m) {
  if (!m.specular_highlight_texname.empty())
    return m.specular_highlight_texname;
  std::string unknown = findUnknownTex(m, "map_Ns");
  if (!unknown.empty())
    return unknown;
  unknown = findUnknownTex(m, "map_ns");
  if (!unknown.empty())
    return unknown;
  return {};
}

std::string pickMetallicTextureName(const tinyobj::material_t &m) {
  if (!m.metallic_texname.empty())
    return m.metallic_texname;
  std::string unknown = findUnknownTex(m, "map_Pm");
  if (!unknown.empty())
    return unknown;
  unknown = findUnknownTex(m, "map_pm");
  if (!unknown.empty())
    return unknown;
  return {};
}

std::string pickAOTextureName(const tinyobj::material_t &m) {
  if (!m.ambient_texname.empty())
    return m.ambient_texname;
  std::string unknown = findUnknownTex(m, "map_AO");
  if (!unknown.empty())
    return unknown;
  unknown = findUnknownTex(m, "map_ao");
  if (!unknown.empty())
    return unknown;
  unknown = findUnknownTex(m, "map_Ka");
  if (!unknown.empty())
    return unknown;
  unknown = findUnknownTex(m, "map_ka");
  if (!unknown.empty())
    return unknown;

  return {};
}

std::string pickEmissiveTextureName(const tinyobj::material_t &m) {
  if (!m.emissive_texname.empty())
    return m.emissive_texname;
  std::string unknown = findUnknownTex(m, "map_Ke");
  if (!unknown.empty())
    return unknown;
  unknown = findUnknownTex(m, "map_ke");
  if (!unknown.empty())
    return unknown;
  return {};
}

std::string pickOpacityTextureName(const tinyobj::material_t &m) {
  if (!m.alpha_texname.empty())
    return m.alpha_texname;
  std::string unknown = findUnknownTex(m, "map_d");
  if (!unknown.empty())
    return unknown;
  unknown = findUnknownTex(m, "map_opacity");
  if (!unknown.empty())
    return unknown;
  return {};
}

// Resolves an MTL texture reference to a load path. Blender often exports
// textures into a sibling "textures/" dir while the MTL keeps only the
// filename, so common sibling folders are tried too. Returns the first
// candidate that exists, or the primary candidate (for diagnostics at upload
// time) when none do.
std::string resolveTexturePath(const std::string &name,
                               const std::string &baseDir) {
  if (name.empty())
    return {};

  std::string normalizedName = lastToken(name);
  if (normalizedName.empty())
    return {};

  std::vector<std::string> candidates;
  candidates.reserve(4);
  if (isAbsolutePath(normalizedName)) {
    candidates.push_back(normalizedName);
  } else {
    candidates.push_back(joinPath(baseDir, normalizedName));

    std::string fileOnly = normalizedName;
    size_t slash = fileOnly.find_last_of("/\\");
    if (slash != std::string::npos)
      fileOnly = fileOnly.substr(slash + 1);
    candidates.push_back(joinPath(baseDir, "textures/" + fileOnly));
    candidates.push_back(joinPath(baseDir, "Textures/" + fileOnly));
  }

  for (const auto &candidate : candidates) {
    if (fileExists(candidate))
      return candidate;
  }
  return candidates.front();
}

std::string makeSubmeshKey(const std::string &obj, const std::string &mtl) {
  return obj + "|" + mtl;
}

} // namespace

std::unique_ptr<MeshData> parseMeshOBJ(const std::string &objPath) {
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
    return nullptr;

  auto data = std::make_unique<MeshData>();
  data->sourcePath = objPath;

  // Convert MTL materials to backend-neutral MaterialAssets with resolved
  // texture paths.
  std::vector<MaterialAsset> parsedMaterials(materials.size());
  for (size_t i = 0; i < materials.size(); ++i) {
    const auto &m = materials[i];
    MaterialAsset &mat = parsedMaterials[i];
    mat.sourceAssetPath = objPath;
    mat.sourceMaterialName = m.name;
    mat.baseColor = glm::vec4((float)m.diffuse[0], (float)m.diffuse[1],
                              (float)m.diffuse[2],
                              std::clamp((float)m.dissolve, 0.0f, 1.0f));
    mat.emissiveColor = glm::vec3((float)m.emission[0], (float)m.emission[1],
                                  (float)m.emission[2]);

    float tmpValue = 0.0f;
    if (findUnknownFloat(m, "Pr", tmpValue) ||
        findUnknownFloat(m, "pr", tmpValue) || m.roughness > 0.0f) {
      mat.roughness = std::clamp((float)m.roughness, 0.0f, 1.0f);
      if (findUnknownFloat(m, "Pr", tmpValue) ||
          findUnknownFloat(m, "pr", tmpValue)) {
        mat.roughness = std::clamp(tmpValue, 0.0f, 1.0f);
      }
    }
    if (findUnknownFloat(m, "Pm", tmpValue) ||
        findUnknownFloat(m, "pm", tmpValue) || m.metallic > 0.0f) {
      mat.metallic = std::clamp((float)m.metallic, 0.0f, 1.0f);
      if (findUnknownFloat(m, "Pm", tmpValue) ||
          findUnknownFloat(m, "pm", tmpValue)) {
        mat.metallic = std::clamp(tmpValue, 0.0f, 1.0f);
      }
    }
    if (findUnknownFloat(m, "Ao", tmpValue) ||
        findUnknownFloat(m, "AO", tmpValue) ||
        findUnknownFloat(m, "ao", tmpValue)) {
      mat.ao = std::clamp(tmpValue, 0.0f, 1.0f);
    }
    if (m.shininess > 0.0f && mat.roughness >= 0.8f) {
      // Legacy MTL `Ns` is glossiness-like. Convert to roughness fallback.
      float ns = std::max((float)m.shininess, 1.0f);
      mat.roughness = std::clamp(std::sqrt(2.0f / (ns + 2.0f)), 0.04f, 1.0f);
    }

    mat.texDiffusePath = resolveTexturePath(pickDiffuseTextureName(m), baseDir);
    mat.texNormalPath = resolveTexturePath(pickNormalTextureName(m), baseDir);
    mat.texRoughnessPath =
        resolveTexturePath(pickRoughnessTextureName(m), baseDir);
    if (mat.texRoughnessPath.empty()) {
      mat.texRoughnessPath =
          resolveTexturePath(pickGlossinessTextureName(m), baseDir);
      mat.roughnessMapIsGloss = !mat.texRoughnessPath.empty();
    }
    mat.texMetallicPath =
        resolveTexturePath(pickMetallicTextureName(m), baseDir);
    mat.texAOPath = resolveTexturePath(pickAOTextureName(m), baseDir);
    mat.texEmissivePath =
        resolveTexturePath(pickEmissiveTextureName(m), baseDir);
    mat.texOpacityPath = resolveTexturePath(pickOpacityTextureName(m), baseDir);
    if (!mat.texOpacityPath.empty()) {
      mat.opacityChannel = 0;
      mat.alphaCutoff = 0.333f;
    }
  }

  std::unordered_map<std::string, size_t> submeshIndexByKey;
  std::unordered_map<std::string, size_t> objectBoundsIndexByName;

  auto ensureSubmesh = [&](const std::string &objectName,
                           int matId) -> size_t {
    std::string materialName = "Default";
    const MaterialAsset *parsedMat = nullptr;

    if (!materials.empty() && matId >= 0 && matId < (int)materials.size()) {
      parsedMat = &parsedMaterials[(size_t)matId];
      materialName = materials[(size_t)matId].name;
    }

    std::string key = makeSubmeshKey(objectName, materialName);
    auto it = submeshIndexByKey.find(key);
    if (it != submeshIndexByKey.end())
      return it->second;

    MeshSubmeshData sm;
    sm.objectName = objectName;
    sm.materialName = materialName;
    sm.debugName = objectName + " / " + materialName;
    if (parsedMat) {
      sm.material = *parsedMat;
      sm.material.baseColor.a = parsedMat->baseColor.a;
    } else {
      sm.material.baseColor = glm::vec4(1.0f);
      sm.material.sourceAssetPath = objPath;
    }
    if (sm.material.sourceMaterialName.empty())
      sm.material.sourceMaterialName = sm.materialName;
    sm.material.id = sm.debugName;

    size_t idx = data->submeshes.size();
    data->submeshes.push_back(std::move(sm));
    submeshIndexByKey[key] = idx;
    return idx;
  };

  auto objectBoundsFor = [&](const std::string &objName) -> MeshObjectBounds & {
    auto it = objectBoundsIndexByName.find(objName);
    if (it != objectBoundsIndexByName.end())
      return data->objectBounds[it->second].second;
    objectBoundsIndexByName[objName] = data->objectBounds.size();
    data->objectBounds.emplace_back(objName, MeshObjectBounds{});
    return data->objectBounds.back().second;
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
      auto &sm = data->submeshes[subIdx];

      // triangles expected (triangulate=true)
      if (fv == 3) {
        MeshVertex face[3]{};
        bool hasAllNormals = true;

        for (int v = 0; v < 3; v++) {
          tinyobj::index_t idx = shape.mesh.indices[index_offset + v];
          MeshVertex vert{};

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

        auto &ob = objectBoundsFor(objName);
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

        sm.vertices.push_back(face[0]);
        sm.vertices.push_back(face[1]);
        sm.vertices.push_back(face[2]);
      }

      index_offset += fv;
    }
  }

  repairMissingNormals(*data);
  return data;
}

std::unique_ptr<MeshData> parseMeshFile(const std::string &path) {
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });

  if (ext == ".obj")
    return parseMeshOBJ(path);
  if (ext == ".gltf" || ext == ".glb")
    return parseMeshGLTF(path);
  if (ext == ".fbx")
    return parseMeshFBX(path);
  return nullptr;
}
