#include "FBXModel.h"
#include "GLStateCache.h"
#include "Logger.h"
#include "Shader.h"
#include "Texture.h"
#include <stb/stb_image.h>


#include "tiny_gltf.h"

#include <map>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

// --- STATIC HELPERS & CACHE ---

static std::map<std::string, GLuint> sTextureCache;

static glm::mat4 buildTRS(const glm::vec3& pos, const glm::vec3& rotDeg, const glm::vec3& scale)
{
    glm::mat4 m(1.0f);
    m = glm::translate(m, pos);
    m = glm::rotate(m, glm::radians(rotDeg.y), glm::vec3(0,1,0));
    m = glm::rotate(m, glm::radians(rotDeg.x), glm::vec3(1,0,0));
    m = glm::rotate(m, glm::radians(rotDeg.z), glm::vec3(0,0,1));
    m = glm::scale(m, scale);
    return m;
}

// --- CLASS IMPLEMENTATION ---

bool FBXModel::loadFromFile(const std::string& path)
{
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
    const tinygltf::Scene& scene = mModel.scenes[mModel.defaultScene > -1 ? mModel.defaultScene : 0];
    
    for (size_t i = 0; i < scene.nodes.size(); i++) {
        processNode(scene.nodes[i]);
    }

    LOG_INFO("Asset", "Loaded glTF: " + path + " with " +
                          std::to_string(mSubmeshes.size()) + " submeshes.");
    return true;
}

void FBXModel::processNode(int nodeIndex)
{
    if (nodeIndex < 0 || nodeIndex >= mModel.nodes.size()) return;
    
    const tinygltf::Node& node = mModel.nodes[nodeIndex];

    // Process mesh if this node has one
    if (node.mesh >= 0) {
        processMesh(mModel.meshes[node.mesh]);
    }

    // Recursively process children
    for (size_t i = 0; i < node.children.size(); i++) {
        processNode(node.children[i]);
    }
}

void FBXModel::processMesh(const tinygltf::Mesh& mesh)
{
    // glTF meshes contain "primitives" (our submeshes)
    for (size_t i = 0; i < mesh.primitives.size(); i++) {
        const tinygltf::Primitive& primitive = mesh.primitives[i];

        std::vector<FBXVertex> vertices;
        std::vector<unsigned int> indices;

        // Get positions
        const tinygltf::Accessor& posAccessor = mModel.accessors[primitive.attributes.find("POSITION")->second];
        const tinygltf::BufferView& posView = mModel.bufferViews[posAccessor.bufferView];
        const tinygltf::Buffer& posBuffer = mModel.buffers[posView.buffer];
        const float* positions = reinterpret_cast<const float*>(&posBuffer.data[posView.byteOffset + posAccessor.byteOffset]);

        // Get normals (if available)
        const float* normals = nullptr;
        auto normalIt = primitive.attributes.find("NORMAL");
        if (normalIt != primitive.attributes.end()) {
            const tinygltf::Accessor& normAccessor = mModel.accessors[normalIt->second];
            const tinygltf::BufferView& normView = mModel.bufferViews[normAccessor.bufferView];
            const tinygltf::Buffer& normBuffer = mModel.buffers[normView.buffer];
            normals = reinterpret_cast<const float*>(&normBuffer.data[normView.byteOffset + normAccessor.byteOffset]);
        }

        // Get UVs (if available)
        const float* uvs = nullptr;
        auto uvIt = primitive.attributes.find("TEXCOORD_0");
        if (uvIt != primitive.attributes.end()) {
            const tinygltf::Accessor& uvAccessor = mModel.accessors[uvIt->second];
            const tinygltf::BufferView& uvView = mModel.bufferViews[uvAccessor.bufferView];
            const tinygltf::Buffer& uvBuffer = mModel.buffers[uvView.buffer];
            uvs = reinterpret_cast<const float*>(&uvBuffer.data[uvView.byteOffset + uvAccessor.byteOffset]);
        }

        // Build vertices
        for (size_t v = 0; v < posAccessor.count; v++) {
            FBXVertex vertex;
            vertex.pos = glm::vec3(positions[v * 3 + 0], positions[v * 3 + 1], positions[v * 3 + 2]);
            
            if (normals) {
                vertex.normal = glm::vec3(normals[v * 3 + 0], normals[v * 3 + 1], normals[v * 3 + 2]);
            } else {
                vertex.normal = glm::vec3(0, 1, 0);
            }
            
            if (uvs) {
                vertex.uv = glm::vec2(uvs[v * 2 + 0], uvs[v * 2 + 1]);
            } else {
                vertex.uv = glm::vec2(0, 0);
            }
            
            vertices.push_back(vertex);
        }

        // Get indices
        const tinygltf::Accessor& indexAccessor = mModel.accessors[primitive.indices];
        const tinygltf::BufferView& indexView = mModel.bufferViews[indexAccessor.bufferView];
        const tinygltf::Buffer& indexBuffer = mModel.buffers[indexView.buffer];

        // Handle different index types
        if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
            const uint16_t* buf = reinterpret_cast<const uint16_t*>(&indexBuffer.data[indexView.byteOffset + indexAccessor.byteOffset]);
            for (size_t j = 0; j < indexAccessor.count; j++) {
                indices.push_back(buf[j]);
            }
        } else if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
            const uint32_t* buf = reinterpret_cast<const uint32_t*>(&indexBuffer.data[indexView.byteOffset + indexAccessor.byteOffset]);
            for (size_t j = 0; j < indexAccessor.count; j++) {
                indices.push_back(buf[j]);
            }
        }

        // Create submesh
        FBXSubmesh submesh;
        submesh.indexCount = (GLsizei)indices.size();
        submesh.material.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
        submesh.material.texDiffuse = 0;
        submesh.material.texNormal = 0;
        submesh.material.texRoughness = 0;
        submesh.material.texMetallic = 0;

        // Load material
        if (primitive.material >= 0) {
            const tinygltf::Material& mat = mModel.materials[primitive.material];
            submesh.materialName = mat.name;
            LOG_TRACE("Asset", "Processing material: " + mat.name);

            // Base color
            if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0) {
                submesh.material.texDiffuse = LoadTextureFromGLTF(mat.pbrMetallicRoughness.baseColorTexture.index);
                if (submesh.material.texDiffuse != 0) {
                    LOG_TRACE("Asset", "Loaded diffuse texture");
                }
            }
            
            // Base color factor (fallback color)
            auto& colorFactor = mat.pbrMetallicRoughness.baseColorFactor;
            submesh.material.baseColor = glm::vec4((float)colorFactor[0], (float)colorFactor[1], (float)colorFactor[2], 1.0f);

            // Normal map
            if (mat.normalTexture.index >= 0) {
                submesh.material.texNormal = LoadTextureFromGLTF(mat.normalTexture.index);
                if (submesh.material.texNormal != 0) {
                    LOG_TRACE("Asset", "Loaded normal texture");
                }
            }

            // Metallic-roughness texture (packed: R=unused, G=roughness, B=metallic)
            if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) {
                submesh.material.texRoughness = LoadTextureFromGLTF(mat.pbrMetallicRoughness.metallicRoughnessTexture.index);
                submesh.material.texMetallic = submesh.material.texRoughness; // Same texture, different channels
                if (submesh.material.texRoughness != 0) {
                    LOG_TRACE("Asset", "Loaded metallic-roughness texture");
                }
            }
        }
        submesh.material.id = submesh.materialName;

        // Create GL buffers
        glGenVertexArrays(1, &submesh.vao);
        glGenBuffers(1, &submesh.vbo);
        glGenBuffers(1, &submesh.ebo);

        glBindVertexArray(submesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, submesh.vbo);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(FBXVertex), vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, submesh.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(FBXVertex), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(FBXVertex), (void*)offsetof(FBXVertex, uv));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(FBXVertex), (void*)offsetof(FBXVertex, normal));

        glBindVertexArray(0);
        mSubmeshes.push_back(submesh);
    }
}

GLuint FBXModel::LoadTextureFromGLTF(int textureIndex)
{
    if (textureIndex < 0 || textureIndex >= mModel.textures.size()) return 0;

    const tinygltf::Texture& tex = mModel.textures[textureIndex];
    if (tex.source < 0 || tex.source >= mModel.images.size()) return 0;

    const tinygltf::Image& image = mModel.images[tex.source];
    
    // Check cache
    std::string key = image.uri.empty() ? ("embedded_" + std::to_string(tex.source)) : image.uri;
    if (sTextureCache.find(key) != sTextureCache.end()) {
        return sTextureCache[key];
    }

    GLuint texID = CreateTextureFromImage(image);
    
    if (texID != 0) {
        sTextureCache[key] = texID;
        if (image.uri.empty()) {
            LOG_TRACE("Asset", "Loaded embedded texture from glTF");
        } else {
            LOG_TRACE("Asset", "Loaded texture file: " + image.uri);
        }
    }

    return texID;
}

GLuint FBXModel::CreateTextureFromImage(const tinygltf::Image& image)
{
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, format, image.width, image.height, 0, format, GL_UNSIGNED_BYTE, &image.image[0]);
    glGenerateMipmap(GL_TEXTURE_2D);

    return texID;
}

void FBXModel::draw(Shader& shader, const glm::vec3& pos, const glm::vec3& rot, const glm::vec3& scale)
{
    glm::mat4 modelMatrix = buildTRS(pos, rot, scale);
    shader.setMat4("model", modelMatrix);

    LOG_TRACE("Render", "Drawing FBX/glTF model submeshes=" +
                            std::to_string(mSubmeshes.size()));


    for(const auto& sm : mSubmeshes)
    {
        if (sm.vao == 0) continue;

        LOG_TRACE("Render", "Submesh textures diffuse=" +
                                std::to_string((unsigned long long)sm.material.texDiffuse) +
                                " normal=" +
                                std::to_string((unsigned long long)sm.material.texNormal) +
                                " roughness=" +
                                std::to_string((unsigned long long)sm.material.texRoughness));
        sm.material.apply(shader);

        GLStateCache::instance().bindVertexArray(sm.vao);
        glDrawElements(GL_TRIANGLES, sm.indexCount, GL_UNSIGNED_INT, 0);
    }
    GLStateCache::instance().bindVertexArray(0);
}

void FBXModel::drawDepth(Shader& shadowShader, const glm::vec3& pos, const glm::vec3& rot, const glm::vec3& scale)
{
    glm::mat4 modelMatrix = buildTRS(pos, rot, scale);
    shadowShader.setMat4("model", modelMatrix);

    for (const auto& sm : mSubmeshes)
    {
        if (sm.vao == 0 || sm.indexCount <= 0) continue;
        GLStateCache::instance().bindVertexArray(sm.vao);
        glDrawElements(GL_TRIANGLES, sm.indexCount, GL_UNSIGNED_INT, 0);
    }

    GLStateCache::instance().bindVertexArray(0);
}

void FBXModel::shutdown()
{
    for(auto& sm : mSubmeshes) {
        if (sm.vao) glDeleteVertexArrays(1, &sm.vao);
        if (sm.vbo) glDeleteBuffers(1, &sm.vbo);
        if (sm.ebo) glDeleteBuffers(1, &sm.ebo);
    }
    mSubmeshes.clear();
    // tinygltf::Model cleans up automatically
}
