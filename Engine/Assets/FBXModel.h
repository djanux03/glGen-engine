#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glad/glad.h>
#include <unordered_map>
#include "Texture.h"
#include "tiny_gltf.h"

struct FBXVertex {
    glm::vec3 pos;
    glm::vec2 uv;
    glm::vec3 normal;
};

struct FBXSubmesh {
    std::string name;
    std::string materialName;
    GLuint vao = 0, vbo = 0, ebo = 0;
    GLsizei indexCount = 0;
    
    // Textures/Material
    glm::vec3 kd{1.0f};
    GLuint texDiffuse = 0;    // BaseColor
    GLuint texNormal = 0;     // Normal map
    GLuint texRoughness = 0;  // Roughness map
    GLuint texMetallic = 0;   // Metallic map
};

class FBXModel {
public:
    bool loadFromFile(const std::string& path);
    void draw(class Shader& shader, const glm::vec3& pos, const glm::vec3& rot, const glm::vec3& scale);
    void shutdown();

private:
    std::vector<FBXSubmesh> mSubmeshes;
    std::string mDirectory;
    tinygltf::Model mModel;
    
    void processNode(int nodeIndex);
    void processMesh(const tinygltf::Mesh& mesh);
    GLuint LoadTextureFromGLTF(int textureIndex);
    GLuint CreateTextureFromImage(const tinygltf::Image& image);
};
