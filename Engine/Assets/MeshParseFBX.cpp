#include "Logger.h"
#include "MeshParse.h"

#include "ufbx.h"
#include <stb/stb_image.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

bool fileExists(const std::string &path) {
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::path(path), ec);
}

// Parsing context: walks the ufbx scene and fills a MeshData. File textures
// are resolved to paths; embedded textures are decoded (stb) into RGBA
// payloads stored in MeshData::images.
struct FBXParseContext {
  const std::string &directory;
  const std::string &sourcePath;
  MeshData &out;
  std::map<const ufbx_texture *, std::string> keyByTexture;
  int embeddedCounter = 0;

  // Returns the load path (or embedded payload key) for a ufbx texture,
  // empty when unresolvable. Mirrors the old UFBXModel::loadTextureFromUFBX
  // candidate order: absolute filename, filename, relative filename, then
  // embedded content.
  std::string textureKey(const ufbx_texture *tex) {
    if (!tex)
      return {};

    auto it = keyByTexture.find(tex);
    if (it != keyByTexture.end())
      return it->second;

    auto resolveFilePath = [&](const std::string &candidate) -> std::string {
      if (candidate.empty())
        return {};
      std::string filePath = candidate;
      if (!(filePath[0] == '/' ||
            (filePath.size() > 1 && filePath[1] == ':'))) {
        filePath = directory + "/" + filePath;
      }
      if (fileExists(filePath))
        return filePath;
      return {};
    };

    std::string key;
    if (tex->absolute_filename.data && tex->absolute_filename.length > 0) {
      key = resolveFilePath(std::string(tex->absolute_filename.data,
                                        tex->absolute_filename.length));
    }
    if (key.empty() && tex->filename.data && tex->filename.length > 0) {
      key = resolveFilePath(
          std::string(tex->filename.data, tex->filename.length));
    }
    if (key.empty() && tex->relative_filename.data &&
        tex->relative_filename.length > 0) {
      key = resolveFilePath(std::string(tex->relative_filename.data,
                                        tex->relative_filename.length));
    }

    // Embedded image fallback.
    if (key.empty() && tex->content.data && tex->content.size > 0) {
      int w = 0, h = 0, channels = 0;
      stbi_uc *pixels =
          stbi_load_from_memory((const stbi_uc *)tex->content.data,
                                (int)tex->content.size, &w, &h, &channels, 4);
      if (pixels) {
        MeshImage img;
        img.key = "embedded://ufbx" + std::to_string(embeddedCounter++);
        img.width = w;
        img.height = h;
        img.component = 4;
        img.pixels.assign(pixels, pixels + (size_t)w * h * 4);
        stbi_image_free(pixels);

        key = img.key;
        out.images.push_back(std::move(img));
        LOG_TRACE("Asset", "ufbx decoded embedded texture bytes");
      }
    }

    keyByTexture[tex] = key;
    return key;
  }

  void processNode(ufbx_node *node) {
    if (!node)
      return;

    if (node->mesh) {
      processMesh(node->mesh, node);
    }

    for (size_t i = 0; i < node->children.count; i++) {
      processNode(node->children.data[i]);
    }
  }

  void processMesh(ufbx_mesh *mesh, ufbx_node *node) {
    for (size_t p = 0; p < mesh->material_parts.count; p++) {
      ufbx_mesh_part part = mesh->material_parts.data[p];
      if (part.num_triangles == 0)
        continue;

      MeshSubmeshData submesh;
      submesh.material.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
      submesh.material.sourceAssetPath = sourcePath;
      submesh.objectName =
          node && node->name.data
              ? std::string(node->name.data, node->name.length)
              : std::string();
      submesh.debugName = submesh.objectName;

      ufbx_material *fb_mat = nullptr;
      if (node && p < node->materials.count) {
        fb_mat = node->materials.data[p];
      } else if (p < mesh->materials.count) {
        fb_mat = mesh->materials.data[p];
      }

      if (fb_mat) {
        submesh.materialName = fb_mat->name.data;
        submesh.material.sourceMaterialName = submesh.materialName;
        if (fb_mat->pbr.base_color.has_value) {
          ufbx_vec3 c = fb_mat->pbr.base_color.value_vec3;
          submesh.material.baseColor = glm::vec4(c.x, c.y, c.z, 1.0f);
        }
        if (fb_mat->pbr.base_color.texture) {
          submesh.material.texDiffusePath =
              textureKey(fb_mat->pbr.base_color.texture);
        }
        if (fb_mat->pbr.normal_map.texture) {
          submesh.material.texNormalPath =
              textureKey(fb_mat->pbr.normal_map.texture);
        }
        if (fb_mat->pbr.roughness.has_value) {
          submesh.material.roughness =
              std::clamp((float)fb_mat->pbr.roughness.value_real, 0.0f, 1.0f);
        }
        if (fb_mat->pbr.roughness.texture) {
          submesh.material.texRoughnessPath =
              textureKey(fb_mat->pbr.roughness.texture);
        }
        if (fb_mat->pbr.metalness.has_value) {
          submesh.material.metallic =
              std::clamp((float)fb_mat->pbr.metalness.value_real, 0.0f, 1.0f);
        }
        if (fb_mat->pbr.metalness.texture) {
          submesh.material.texMetallicPath =
              textureKey(fb_mat->pbr.metalness.texture);
        }
        if (fb_mat->pbr.ambient_occlusion.has_value) {
          submesh.material.ao = std::clamp(
              (float)fb_mat->pbr.ambient_occlusion.value_real, 0.0f, 1.0f);
        }
        if (fb_mat->pbr.ambient_occlusion.texture) {
          submesh.material.texAOPath =
              textureKey(fb_mat->pbr.ambient_occlusion.texture);
        }

        if (fb_mat->pbr.emission_color.has_value) {
          ufbx_vec3 ec = fb_mat->pbr.emission_color.value_vec3;
          submesh.material.emissiveColor = glm::vec3(ec.x, ec.y, ec.z);
        }
        if (fb_mat->pbr.emission_factor.has_value) {
          submesh.material.emissiveStrength =
              std::max(0.0f, (float)fb_mat->pbr.emission_factor.value_real);
        }
        if (fb_mat->pbr.emission_color.texture) {
          submesh.material.texEmissivePath =
              textureKey(fb_mat->pbr.emission_color.texture);
        } else if (fb_mat->pbr.emission_factor.texture) {
          submesh.material.texEmissivePath =
              textureKey(fb_mat->pbr.emission_factor.texture);
        }

        if (fb_mat->pbr.opacity.has_value) {
          submesh.material.baseColor.a =
              std::clamp((float)fb_mat->pbr.opacity.value_real, 0.0f, 1.0f);
        }
        if (fb_mat->pbr.opacity.texture) {
          submesh.material.texOpacityPath =
              textureKey(fb_mat->pbr.opacity.texture);
          submesh.material.opacityChannel =
              0; // FBX opacity maps are typically grayscale.
          submesh.material.alphaCutoff = 0.333f;
        }

        if (fb_mat->pbr.glossiness.texture &&
            submesh.material.texRoughnessPath.empty()) {
          submesh.material.texRoughnessPath =
              textureKey(fb_mat->pbr.glossiness.texture);
          submesh.material.roughnessMapIsGloss =
              !submesh.material.texRoughnessPath.empty();
        }
        if (fb_mat->pbr.glossiness.has_value &&
            fb_mat->pbr.roughness.has_value == false) {
          float gloss = std::clamp((float)fb_mat->pbr.glossiness.value_real,
                                   0.0f, 1.0f);
          submesh.material.roughness = 1.0f - gloss;
        }
      } else {
        submesh.materialName = "DefaultFBX";
      }
      if (submesh.material.sourceMaterialName.empty())
        submesh.material.sourceMaterialName = submesh.materialName;
      submesh.material.id = submesh.materialName;

      // Triangulate
      size_t num_tri_indices = part.num_triangles * 3;
      std::vector<uint32_t> tri_indices(num_tri_indices);
      size_t index_offset = 0;
      for (size_t f = 0; f < part.num_faces; f++) {
        uint32_t face_idx = part.face_indices.data[f];
        ufbx_face face = mesh->faces.data[face_idx];
        uint32_t num_tris = ufbx_triangulate_face(
            tri_indices.data() + index_offset,
            tri_indices.size() - index_offset, mesh, face);
        index_offset += num_tris * 3;
      }

      submesh.vertices.reserve(num_tri_indices);
      submesh.indices.reserve(num_tri_indices);
      for (size_t i = 0; i < num_tri_indices; i++) {
        uint32_t index = tri_indices[i];

        MeshVertex vertex;
        ufbx_vec3 v = ufbx_get_vertex_vec3(&mesh->vertex_position, index);
        v = ufbx_transform_position(&node->geometry_to_world, v);
        vertex.pos = glm::vec3(v.x, v.y, v.z);

        submesh.aabbMin = glm::min(submesh.aabbMin, vertex.pos);
        submesh.aabbMax = glm::max(submesh.aabbMax, vertex.pos);
        submesh.hasBounds = true;

        if (mesh->vertex_normal.exists) {
          ufbx_vec3 n = ufbx_get_vertex_vec3(&mesh->vertex_normal, index);
          n = ufbx_transform_direction(&node->geometry_to_world, n);
          vertex.normal = glm::normalize(glm::vec3(n.x, n.y, n.z));
        } else {
          vertex.normal = glm::vec3(0, 1, 0);
        }

        if (mesh->vertex_uv.exists) {
          ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, index);
          vertex.uv = glm::vec2(uv.x, uv.y);
        } else {
          vertex.uv = glm::vec2(0, 0);
        }

        submesh.vertices.push_back(vertex);
        submesh.indices.push_back(
            (uint32_t)i); // directly indexed since we unpacked
      }

      out.submeshes.push_back(std::move(submesh));
    }
  }
};

} // namespace

std::unique_ptr<MeshData> parseMeshFBX(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  const std::string directory =
      (slash == std::string::npos) ? "." : path.substr(0, slash);

  ufbx_load_opts opts = {0};
  opts.target_axes = ufbx_axes_right_handed_y_up;
  opts.target_unit_meters = 1.0f;
  opts.generate_missing_normals = true;

  ufbx_error error;
  ufbx_scene *scene = ufbx_load_file(path.c_str(), &opts, &error);

  if (!scene) {
    LOG_ERROR("Asset",
              "ufbx load failed: " + std::string(error.description.data,
                                                 error.description.length));
    return nullptr;
  }

  auto data = std::make_unique<MeshData>();
  data->sourcePath = path;

  FBXParseContext ctx{directory, path, *data, {}, 0};
  ctx.processNode(scene->root_node);

  ufbx_free_scene(scene);

  LOG_INFO("Asset", "Loaded true FBX: " + path + " with " +
                        std::to_string(data->submeshes.size()) +
                        " submeshes.");
  return data;
}
