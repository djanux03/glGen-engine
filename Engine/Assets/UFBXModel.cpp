#include "UFBXModel.h"
#include "GLStateCache.h"
#include "Logger.h"
#include "Shader.h"
#include "Texture.h"
#include <algorithm>
#include <cstdint>
#include <stb/stb_image.h>
#include <glm/gtc/matrix_transform.hpp>
#include <map>
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

bool UFBXModel::loadFromFile(const std::string &path) {
  shutdown();

  size_t slash = path.find_last_of("/\\");
  mDirectory = (slash == std::string::npos) ? "." : path.substr(0, slash);

  ufbx_load_opts opts = {0};
  opts.target_axes = ufbx_axes_right_handed_y_up;
  opts.target_unit_meters = 1.0f;
  opts.generate_missing_normals = true;

  ufbx_error error;
  mScene = ufbx_load_file(path.c_str(), &opts, &error);

  if (!mScene) {
    LOG_ERROR("Asset",
              "ufbx load failed: " + std::string(error.description.data,
                                                 error.description.length));
    return false;
  }

  mHasBounds = false;
  mAabbMin = glm::vec3(1e30f);
  mAabbMax = glm::vec3(-1e30f);

  processNode(mScene->root_node);

  LOG_INFO("Asset", "Loaded true FBX: " + path + " with " +
                        std::to_string(mSubmeshes.size()) + " submeshes.");
  return true;
}

void UFBXModel::processNode(ufbx_node *node) {
  if (!node)
    return;

  if (node->mesh) {
    processMesh(node->mesh, node);
  }

  for (size_t i = 0; i < node->children.count; i++) {
    processNode(node->children.data[i]);
  }
}

void UFBXModel::processMesh(ufbx_mesh *mesh, ufbx_node *node) {
  for (size_t p = 0; p < mesh->material_parts.count; p++) {
    ufbx_mesh_part part = mesh->material_parts.data[p];
    if (part.num_triangles == 0)
      continue;

    std::vector<UFBXVertex> vertices;
    std::vector<unsigned int> indices;

    // Material Loading
    UFBXSubmesh submesh;
    submesh.material.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);

    ufbx_material *fb_mat = nullptr;
    if (node && p < node->materials.count) {
      fb_mat = node->materials.data[p];
    } else if (p < mesh->materials.count) {
      fb_mat = mesh->materials.data[p];
    }

    if (fb_mat) {
      submesh.materialName = fb_mat->name.data;
      if (fb_mat->pbr.base_color.has_value) {
        ufbx_vec3 c = fb_mat->pbr.base_color.value_vec3;
        submesh.material.baseColor = glm::vec4(c.x, c.y, c.z, 1.0f);
      }
      if (fb_mat->pbr.base_color.texture) {
        submesh.material.texDiffuse =
            loadTextureFromUFBX(fb_mat->pbr.base_color.texture);
      }
      if (fb_mat->pbr.normal_map.texture) {
        submesh.material.texNormal =
            loadTextureFromUFBX(fb_mat->pbr.normal_map.texture);
      }
      if (fb_mat->pbr.roughness.has_value) {
        submesh.material.roughness =
            std::clamp((float)fb_mat->pbr.roughness.value_real, 0.0f, 1.0f);
      }
      if (fb_mat->pbr.roughness.texture) {
        submesh.material.texRoughness =
            loadTextureFromUFBX(fb_mat->pbr.roughness.texture);
      }
      if (fb_mat->pbr.metalness.has_value) {
        submesh.material.metallic =
            std::clamp((float)fb_mat->pbr.metalness.value_real, 0.0f, 1.0f);
      }
      if (fb_mat->pbr.metalness.texture) {
        submesh.material.texMetallic =
            loadTextureFromUFBX(fb_mat->pbr.metalness.texture);
      }
      if (fb_mat->pbr.ambient_occlusion.has_value) {
        submesh.material.ao = std::clamp(
            (float)fb_mat->pbr.ambient_occlusion.value_real, 0.0f, 1.0f);
      }
      if (fb_mat->pbr.ambient_occlusion.texture) {
        submesh.material.texAO =
            loadTextureFromUFBX(fb_mat->pbr.ambient_occlusion.texture);
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
        submesh.material.texEmissive =
            loadTextureFromUFBX(fb_mat->pbr.emission_color.texture);
      } else if (fb_mat->pbr.emission_factor.texture) {
        submesh.material.texEmissive =
            loadTextureFromUFBX(fb_mat->pbr.emission_factor.texture);
      }

      if (fb_mat->pbr.opacity.has_value) {
        submesh.material.baseColor.a =
            std::clamp((float)fb_mat->pbr.opacity.value_real, 0.0f, 1.0f);
      }
      if (fb_mat->pbr.opacity.texture) {
        submesh.material.texOpacity = loadTextureFromUFBX(fb_mat->pbr.opacity.texture);
        submesh.material.opacityChannel = 0; // FBX opacity maps are typically grayscale.
        submesh.material.alphaCutoff = 0.333f;
      }

      if (fb_mat->pbr.glossiness.texture && submesh.material.texRoughness == 0) {
        submesh.material.texRoughness =
            loadTextureFromUFBX(fb_mat->pbr.glossiness.texture);
        submesh.material.roughnessMapIsGloss = (submesh.material.texRoughness != 0);
      }
      if (fb_mat->pbr.glossiness.has_value && fb_mat->pbr.roughness.has_value == false) {
        float gloss = std::clamp((float)fb_mat->pbr.glossiness.value_real, 0.0f, 1.0f);
        submesh.material.roughness = 1.0f - gloss;
      }
    } else {
      submesh.materialName = "DefaultFBX";
    }
    submesh.material.id = submesh.materialName;

    // Triangulate
    size_t num_tri_indices = part.num_triangles * 3;
    std::vector<uint32_t> tri_indices(num_tri_indices);
    size_t index_offset = 0;
    for (size_t f = 0; f < part.num_faces; f++) {
      uint32_t face_idx = part.face_indices.data[f];
      ufbx_face face = mesh->faces.data[face_idx];
      uint32_t num_tris =
          ufbx_triangulate_face(tri_indices.data() + index_offset,
                                tri_indices.size() - index_offset, mesh, face);
      index_offset += num_tris * 3;
    }

    for (size_t i = 0; i < num_tri_indices; i++) {
      uint32_t index = tri_indices[i];

      UFBXVertex vertex;
      ufbx_vec3 v = ufbx_get_vertex_vec3(&mesh->vertex_position, index);
      v = ufbx_transform_position(&node->geometry_to_world, v);
      vertex.pos = glm::vec3(v.x, v.y, v.z);

      // accumulate true geometric bounds
      mAabbMin = glm::min(mAabbMin, vertex.pos);
      mAabbMax = glm::max(mAabbMax, vertex.pos);
      mHasBounds = true;

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

      vertices.push_back(vertex);
      indices.push_back((unsigned int)i); // directly indexed since we unpacked
    }

    submesh.indexCount = (GLsizei)indices.size();

    glGenVertexArrays(1, &submesh.vao);
    glGenBuffers(1, &submesh.vbo);
    glGenBuffers(1, &submesh.ebo);

    glBindVertexArray(submesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, submesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(UFBXVertex),
                 vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, submesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int),
                 indices.data(), GL_STATIC_DRAW);

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
}

GLuint UFBXModel::loadTextureFromUFBX(ufbx_texture *tex) {
  if (!tex)
    return 0;

  auto tryLoadFilePath = [&](const std::string &candidate) -> GLuint {
    if (candidate.empty())
      return 0;

    std::string filePath = candidate;
    if (!(filePath[0] == '/' || (filePath.size() > 1 && filePath[1] == ':'))) {
      filePath = mDirectory + "/" + filePath;
    }

    auto cached = mTextureCache.find(filePath);
    if (cached != mTextureCache.end())
      return cached->second;

    GLuint glid = LoadTexture2D(filePath.c_str());
    if (glid != 0) {
      mTextureCache[filePath] = glid;
      LOG_TRACE("Asset", "ufbx loaded texture file: " + filePath);
    }
    return glid;
  };

  if (tex->absolute_filename.data && tex->absolute_filename.length > 0) {
    if (GLuint glid = tryLoadFilePath(
            std::string(tex->absolute_filename.data, tex->absolute_filename.length)))
      return glid;
  }
  if (tex->filename.data && tex->filename.length > 0) {
    if (GLuint glid = tryLoadFilePath(
            std::string(tex->filename.data, tex->filename.length)))
      return glid;
  }
  if (tex->relative_filename.data && tex->relative_filename.length > 0) {
    if (GLuint glid = tryLoadFilePath(
            std::string(tex->relative_filename.data, tex->relative_filename.length)))
      return glid;
  }

  // Embedded image fallback.
  if (tex->content.data && tex->content.size > 0) {
    const std::string key = "embedded_" + std::to_string((uintptr_t)tex);
    auto cached = mTextureCache.find(key);
    if (cached != mTextureCache.end())
      return cached->second;

    int w = 0, h = 0, channels = 0;
    stbi_uc *pixels =
        stbi_load_from_memory((const stbi_uc *)tex->content.data,
                              (int)tex->content.size, &w, &h, &channels, 4);
    if (!pixels)
      return 0;

    GLuint texID = 0;
    glGenTextures(1, &texID);
    glBindTexture(GL_TEXTURE_2D, texID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    stbi_image_free(pixels);

    mTextureCache[key] = texID;
    LOG_TRACE("Asset", "ufbx loaded embedded texture bytes");
    return texID;
  }

  return 0;
}

void UFBXModel::draw(Shader &shader, const glm::vec3 &pos, const glm::vec3 &rot,
                     const glm::vec3 &scale,
                     const MaterialAsset *materialOverride) {
  glm::mat4 modelMatrix = buildTRS(pos, rot, scale);
  shader.setMat4("model", modelMatrix);

  for (const auto &sm : mSubmeshes) {
    if (sm.vao == 0)
      continue;
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

void UFBXModel::drawDepth(Shader &shadowShader, const glm::vec3 &pos,
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

void UFBXModel::drawInstanced(Shader &shader, unsigned int instanceVBO,
                              int instanceCount) {
  for (const auto &sm : mSubmeshes) {
    if (sm.vao == 0)
      continue;

    sm.material.apply(shader);
    GLStateCache::instance().bindVertexArray(sm.vao);

    glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
    std::size_t vec4Size = sizeof(glm::vec4);
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

    glDrawElementsInstanced(GL_TRIANGLES, sm.indexCount, GL_UNSIGNED_INT, 0,
                            instanceCount);

    glVertexAttribDivisor(3, 0);
    glVertexAttribDivisor(4, 0);
    glVertexAttribDivisor(5, 0);
    glVertexAttribDivisor(6, 0);

    glDisableVertexAttribArray(3);
    glDisableVertexAttribArray(4);
    glDisableVertexAttribArray(5);
    glDisableVertexAttribArray(6);
  }
  GLStateCache::instance().bindVertexArray(0);
}

void UFBXModel::drawDepthInstanced(Shader &shadowShader,
                                   unsigned int instanceVBO,
                                   int instanceCount) {
  for (const auto &sm : mSubmeshes) {
    if (sm.vao == 0 || sm.indexCount <= 0)
      continue;

    GLStateCache::instance().bindVertexArray(sm.vao);

    glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
    std::size_t vec4Size = sizeof(glm::vec4);
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

    glDrawElementsInstanced(GL_TRIANGLES, sm.indexCount, GL_UNSIGNED_INT, 0,
                            instanceCount);

    glVertexAttribDivisor(3, 0);
    glVertexAttribDivisor(4, 0);
    glVertexAttribDivisor(5, 0);
    glVertexAttribDivisor(6, 0);

    glDisableVertexAttribArray(3);
    glDisableVertexAttribArray(4);
    glDisableVertexAttribArray(5);
    glDisableVertexAttribArray(6);
  }
  GLStateCache::instance().bindVertexArray(0);
}

bool UFBXModel::getGlobalBounds(glm::vec3 &outMin, glm::vec3 &outMax) const {
  if (!mHasBounds)
    return false;
  outMin = mAabbMin;
  outMax = mAabbMax;
  return true;
}

void UFBXModel::shutdown() {
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

  if (mScene) {
    ufbx_free_scene(mScene);
    mScene = nullptr;
  }
}
