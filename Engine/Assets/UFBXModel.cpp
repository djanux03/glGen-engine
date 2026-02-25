#include "UFBXModel.h"
#include "GLStateCache.h"
#include "Logger.h"
#include "Shader.h"
#include "Texture.h"
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
      if (fb_mat->pbr.roughness.texture) {
        submesh.material.texRoughness =
            loadTextureFromUFBX(fb_mat->pbr.roughness.texture);
        submesh.material.texMetallic = submesh.material.texRoughness;
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
      vertex.pos = glm::vec3(v.x, v.y, v.z);

      // accumulate true geometric bounds
      mAabbMin = glm::min(mAabbMin, vertex.pos);
      mAabbMax = glm::max(mAabbMax, vertex.pos);
      mHasBounds = true;

      if (mesh->vertex_normal.exists) {
        ufbx_vec3 n = ufbx_get_vertex_vec3(&mesh->vertex_normal, index);
        vertex.normal = glm::vec3(n.x, n.y, n.z);
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
  if (!tex || !tex->relative_filename.data)
    return 0;
  std::string relPath = tex->relative_filename.data;
  if (relPath.empty())
    return 0;

  // Check if absolute, else attach to directory
  std::string fPath = relPath;
  if (fPath[0] != '/' && (fPath.size() < 2 || fPath[1] != ':')) {
    fPath = mDirectory + "/" + relPath;
  }

  if (mTextureCache.find(fPath) != mTextureCache.end()) {
    return mTextureCache[fPath];
  }

  GLuint glid = LoadTexture2D(fPath.c_str());
  if (glid != 0) {
    mTextureCache[fPath] = glid;
    LOG_TRACE("Asset", "ufbx loaded texture file: " + fPath);
  }
  return glid;
}

void UFBXModel::draw(Shader &shader, const glm::vec3 &pos, const glm::vec3 &rot,
                     const glm::vec3 &scale) {
  glm::mat4 modelMatrix = buildTRS(pos, rot, scale);
  shader.setMat4("model", modelMatrix);

  for (const auto &sm : mSubmeshes) {
    if (sm.vao == 0)
      continue;
    sm.material.apply(shader);
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
