#include "Logger.h"
#include "MeshParse.h"

#include "tiny_gltf.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace {

int getExtTextureIndex(const tinygltf::Value &extObj,
                       const char *textureInfoKey) {
  if (!extObj.IsObject() || !extObj.Has(textureInfoKey))
    return -1;
  const tinygltf::Value &texInfo = extObj.Get(textureInfoKey);
  if (!texInfo.IsObject() || !texInfo.Has("index"))
    return -1;
  return texInfo.Get("index").GetNumberAsInt();
}

bool getExtFloat(const tinygltf::Value &extObj, const char *key,
                 float &outValue) {
  if (!extObj.IsObject() || !extObj.Has(key))
    return false;
  const tinygltf::Value &v = extObj.Get(key);
  if (!v.IsNumber())
    return false;
  outValue = (float)v.GetNumberAsDouble();
  return true;
}

bool getExtVec3(const tinygltf::Value &extObj, const char *key,
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

bool getExtVec4(const tinygltf::Value &extObj, const char *key,
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

// Parsing context: walks the tinygltf model and fills a MeshData. Texture
// pixels are copied into MeshData::images (tinygltf has already decoded both
// external and embedded images), keyed by the path stored in the material.
struct GLTFParseContext {
  const tinygltf::Model &model;
  const std::string &directory;
  const std::string &sourcePath;
  MeshData &out;
  std::map<int, std::string> imageKeyBySource;

  // Registers the image behind a glTF texture index and returns its key
  // (empty when the texture is unusable).
  std::string textureKey(int textureIndex) {
    if (textureIndex < 0 || textureIndex >= (int)model.textures.size())
      return {};
    const tinygltf::Texture &tex = model.textures[textureIndex];
    if (tex.source < 0 || tex.source >= (int)model.images.size())
      return {};

    auto it = imageKeyBySource.find(tex.source);
    if (it != imageKeyBySource.end())
      return it->second;

    const tinygltf::Image &image = model.images[tex.source];
    if (image.width <= 0 || image.height <= 0 || image.image.empty())
      return {};

    const std::string key =
        image.uri.empty() ? ("embedded://" + std::to_string(tex.source))
                          : (directory + "/" + image.uri);

    MeshImage img;
    img.key = key;
    img.width = image.width;
    img.height = image.height;
    img.component = image.component;
    img.pixels = image.image;
    out.images.push_back(std::move(img));

    imageKeyBySource[tex.source] = key;
    return key;
  }

  void processNode(int nodeIndex) {
    if (nodeIndex < 0 || nodeIndex >= (int)model.nodes.size())
      return;

    const tinygltf::Node &node = model.nodes[nodeIndex];

    if (node.mesh >= 0)
      processMesh(model.meshes[node.mesh]);

    for (size_t i = 0; i < node.children.size(); i++)
      processNode(node.children[i]);
  }

  void processMesh(const tinygltf::Mesh &mesh) {
    for (size_t i = 0; i < mesh.primitives.size(); i++) {
      const tinygltf::Primitive &primitive = mesh.primitives[i];

      MeshSubmeshData submesh;

      // Get positions
      auto posAttrIt = primitive.attributes.find("POSITION");
      if (posAttrIt == primitive.attributes.end()) {
        LOG_WARN("Asset", "Skipping primitive without POSITION attribute");
        continue;
      }

      const tinygltf::Accessor &posAccessor =
          model.accessors[posAttrIt->second];
      const tinygltf::BufferView &posView =
          model.bufferViews[posAccessor.bufferView];
      const tinygltf::Buffer &posBuffer = model.buffers[posView.buffer];
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
            model.accessors[normalIt->second];
        const tinygltf::BufferView &normView =
            model.bufferViews[normAccessor.bufferView];
        const tinygltf::Buffer &normBuffer = model.buffers[normView.buffer];
        normals = normBuffer.data.data() + normView.byteOffset +
                  normAccessor.byteOffset;
        normStride = normAccessor.ByteStride(normView);
        if (normStride <= 0)
          normals = nullptr;
      }

      // Get UVs (if available)
      const unsigned char *uvs = nullptr;
      int uvStride = 0;
      auto uvIt = primitive.attributes.find("TEXCOORD_0");
      if (uvIt != primitive.attributes.end()) {
        const tinygltf::Accessor &uvAccessor = model.accessors[uvIt->second];
        const tinygltf::BufferView &uvView =
            model.bufferViews[uvAccessor.bufferView];
        const tinygltf::Buffer &uvBuffer = model.buffers[uvView.buffer];
        uvs = uvBuffer.data.data() + uvView.byteOffset + uvAccessor.byteOffset;
        uvStride = uvAccessor.ByteStride(uvView);
        if (uvStride <= 0)
          uvs = nullptr;
      }

      // Build vertices
      submesh.vertices.reserve(posAccessor.count);
      for (size_t v = 0; v < posAccessor.count; v++) {
        MeshVertex vertex;
        const float *pos =
            reinterpret_cast<const float *>(positions + v * posStride);
        vertex.pos = glm::vec3(pos[0], pos[1], pos[2]);

        submesh.aabbMin = glm::min(submesh.aabbMin, vertex.pos);
        submesh.aabbMax = glm::max(submesh.aabbMax, vertex.pos);
        submesh.hasBounds = true;

        if (normals) {
          const float *n =
              reinterpret_cast<const float *>(normals + v * normStride);
          vertex.normal = glm::vec3(n[0], n[1], n[2]);
        } else {
          vertex.normal = glm::vec3(0, 1, 0);
        }

        if (uvs) {
          const float *uv =
              reinterpret_cast<const float *>(uvs + v * uvStride);
          vertex.uv = glm::vec2(uv[0], uv[1]);
        } else {
          vertex.uv = glm::vec2(0, 0);
        }

        submesh.vertices.push_back(vertex);
      }

      // Get indices
      if (primitive.indices >= 0) {
        const tinygltf::Accessor &indexAccessor =
            model.accessors[primitive.indices];
        const tinygltf::BufferView &indexView =
            model.bufferViews[indexAccessor.bufferView];
        const tinygltf::Buffer &indexBuffer = model.buffers[indexView.buffer];
        const unsigned char *idxData = indexBuffer.data.data() +
                                       indexView.byteOffset +
                                       indexAccessor.byteOffset;

        if (indexAccessor.componentType ==
            TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
          const uint8_t *buf = reinterpret_cast<const uint8_t *>(idxData);
          for (size_t j = 0; j < indexAccessor.count; j++)
            submesh.indices.push_back((uint32_t)buf[j]);
        } else if (indexAccessor.componentType ==
                   TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
          const uint16_t *buf = reinterpret_cast<const uint16_t *>(idxData);
          for (size_t j = 0; j < indexAccessor.count; j++)
            submesh.indices.push_back((uint32_t)buf[j]);
        } else if (indexAccessor.componentType ==
                   TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
          const uint32_t *buf = reinterpret_cast<const uint32_t *>(idxData);
          for (size_t j = 0; j < indexAccessor.count; j++)
            submesh.indices.push_back(buf[j]);
        } else {
          LOG_WARN("Asset", "Unsupported glTF index component type: " +
                                std::to_string(indexAccessor.componentType));
        }
      } else {
        for (uint32_t j = 0; j < (uint32_t)submesh.vertices.size(); ++j)
          submesh.indices.push_back(j);
      }

      submesh.objectName = mesh.name.empty()
                               ? ("glTFPrimitive_" + std::to_string(i))
                               : (mesh.name + "_" + std::to_string(i));
      submesh.debugName = submesh.objectName;
      submesh.material.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
      submesh.material.sourceAssetPath = sourcePath;

      // Load material
      if (primitive.material >= 0) {
        const tinygltf::Material &mat = model.materials[primitive.material];
        submesh.materialName = mat.name;
        submesh.material.sourceMaterialName = mat.name;
        LOG_TRACE("Asset", "Processing material: " + mat.name);

        // Base color
        if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0) {
          submesh.material.texDiffusePath =
              textureKey(mat.pbrMetallicRoughness.baseColorTexture.index);
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
          submesh.material.emissiveColor =
              glm::vec3((float)mat.emissiveFactor[0],
                        (float)mat.emissiveFactor[1],
                        (float)mat.emissiveFactor[2]);
        }

        // Normal map
        if (mat.normalTexture.index >= 0) {
          submesh.material.texNormalPath =
              textureKey(mat.normalTexture.index);
        }

        // Metallic-roughness texture (packed: R=unused, G=roughness,
        // B=metallic)
        if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) {
          submesh.material.texRoughnessPath = textureKey(
              mat.pbrMetallicRoughness.metallicRoughnessTexture.index);
          submesh.material.texMetallicPath =
              submesh.material.texRoughnessPath; // Same texture, different
                                                 // channels
          submesh.material.roughnessChannel = 1; // G
          submesh.material.metallicChannel = 2;  // B
        }

        if (mat.occlusionTexture.index >= 0) {
          submesh.material.texAOPath = textureKey(mat.occlusionTexture.index);
          submesh.material.ao = (float)mat.occlusionTexture.strength;
        }

        if (mat.emissiveTexture.index >= 0) {
          submesh.material.texEmissivePath =
              textureKey(mat.emissiveTexture.index);
        }

        // glTF alpha masking/blending uses base-color alpha.
        if (mat.alphaMode == "MASK") {
          submesh.material.texOpacityPath = submesh.material.texDiffusePath;
          submesh.material.opacityChannel = 3;
          submesh.material.alphaCutoff = (float)mat.alphaCutoff;
        } else if (mat.alphaMode == "BLEND") {
          submesh.material.texOpacityPath = submesh.material.texDiffusePath;
          submesh.material.opacityChannel = 3;
          submesh.material.alphaCutoff = 0.001f;
        }

        // KHR_materials_pbrSpecularGlossiness compatibility fallback
        auto extIt = mat.extensions.find("KHR_materials_pbrSpecularGlossiness");
        if (extIt != mat.extensions.end() && extIt->second.IsObject()) {
          const tinygltf::Value &ext = extIt->second;

          const int diffuseTex = getExtTextureIndex(ext, "diffuseTexture");
          if (diffuseTex >= 0 && submesh.material.texDiffusePath.empty()) {
            submesh.material.texDiffusePath = textureKey(diffuseTex);
          }

          glm::vec4 diffuseFactor;
          if (getExtVec4(ext, "diffuseFactor", diffuseFactor)) {
            submesh.material.baseColor = diffuseFactor;
          }

          const int specGlossTex =
              getExtTextureIndex(ext, "specularGlossinessTexture");
          if (specGlossTex >= 0) {
            const std::string packedKey = textureKey(specGlossTex);
            if (!packedKey.empty()) {
              submesh.material.texRoughnessPath = packedKey;
              submesh.material.texMetallicPath = packedKey;
              submesh.material.roughnessChannel = 3; // A=glossiness
              submesh.material.metallicChannel =
                  2; // B≈specular intensity proxy
              submesh.material.roughnessMapIsGloss = true;
            }
          }

          float glossiness = 0.0f;
          if (getExtFloat(ext, "glossinessFactor", glossiness)) {
            submesh.material.roughness =
                std::clamp(1.0f - glossiness, 0.04f, 1.0f);
          }

          glm::vec3 specularFactor;
          if (getExtVec3(ext, "specularFactor", specularFactor)) {
            float maxSpec =
                std::max(specularFactor.x,
                         std::max(specularFactor.y, specularFactor.z));
            submesh.material.metallic =
                std::clamp((maxSpec - 0.04f) / 0.96f, 0.0f, 1.0f);
          }
        }
      }
      if (submesh.materialName.empty())
        submesh.materialName = "glTFMaterial_" + std::to_string(i);
      if (submesh.material.sourceMaterialName.empty())
        submesh.material.sourceMaterialName = submesh.materialName;
      submesh.material.id = submesh.materialName;

      out.submeshes.push_back(std::move(submesh));
    }
  }
};

} // namespace

std::unique_ptr<MeshData> parseMeshGLTF(const std::string &path) {
  tinygltf::Model model;
  tinygltf::TinyGLTF loader;
  std::string err, warn;
  bool ret = false;

  // Determine base directory
  size_t slash = path.find_last_of("/\\");
  const std::string directory =
      (slash == std::string::npos) ? "." : path.substr(0, slash);

  // Check if binary (.glb) or ASCII (.gltf)
  if (path.find(".glb") != std::string::npos) {
    ret = loader.LoadBinaryFromFile(&model, &err, &warn, path);
  } else {
    ret = loader.LoadASCIIFromFile(&model, &err, &warn, path);
  }

  if (!warn.empty()) {
    LOG_WARN("Asset", "glTF warning: " + warn);
  }

  if (!err.empty()) {
    LOG_ERROR("Asset", "glTF error: " + err);
  }

  if (!ret) {
    LOG_ERROR("Asset", "Failed to load glTF: " + path);
    return nullptr;
  }

  auto data = std::make_unique<MeshData>();
  data->sourcePath = path;

  GLTFParseContext ctx{model, directory, path, *data, {}};

  // Process all scenes (usually just one)
  const tinygltf::Scene &scene =
      model.scenes[model.defaultScene > -1 ? model.defaultScene : 0];

  for (size_t i = 0; i < scene.nodes.size(); i++) {
    ctx.processNode(scene.nodes[i]);
  }

  LOG_INFO("Asset", "Loaded glTF: " + path + " with " +
                        std::to_string(data->submeshes.size()) +
                        " submeshes.");

  return data;
}
