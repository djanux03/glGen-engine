#pragma once
#include "Material.h"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

class Shader;

class OBJModel {
public:
  bool loadFromFile(const std::string &objPath);
  void shutdown();

  // Load from raw vertex data (for procedural meshes)
  struct VertexData {
    glm::vec3 pos;
    glm::vec2 uv;
    glm::vec3 normal;
  };
  bool loadFromVertices(const std::vector<VertexData> &vertices,
                        const std::string &name = "Primitive");

  // Same signature your engine already uses:
  void draw(Shader &shader, const glm::vec3 &position, const glm::vec3 &rotDeg,
            const glm::vec3 &scale);

  void drawDepth(Shader &shadowShader, const glm::vec3 &position,
                 const glm::vec3 &rotDeg, const glm::vec3 &scale);

  // Existing anchor query (material-based):
  bool getSubmeshCenterLocal(const std::string &materialName,
                             glm::vec3 &outCenter) const;

  // NEW: object-based query (o <name> in OBJ):
  bool getObjectCenterLocal(const std::string &objectName,
                            glm::vec3 &outCenter) const;

  // NEW: rotate an OBJ object (e.g. "Turret_02") around its local pivot (AABB
  // center).
  void setObjectYawDeg(const std::string &objectName, float yawDeg);
  void clearObjectOverrides();
  void setObjectYawDegPivot(const std::string &objectName, float yawDeg,
                            const glm::vec3 &pivotLocal);
  bool getObjectLocalTRS(const std::string &objectName, glm::vec3 &outPos,
                         glm::vec3 &outRotDeg, glm::vec3 &outScale) const;

  void setObjectLocalTRS(const std::string &objectName, const glm::vec3 &pos,
                         const glm::vec3 &rotDeg, const glm::vec3 &scale);

  void clearObjectLocalTRS(const std::string &objectName);
  void clearAllObjectLocalTRS();

  glm::mat4 buildObjectExtra(const std::string &objectName) const;
  std::vector<std::string> objectNames() const;
  std::size_t submeshCount() const { return mSubmeshes.size(); }

  // Object-level AABB bounds (for mouse picking per submesh)
  bool getObjectBounds(const std::string &objectName, glm::vec3 &outMin,
                       glm::vec3 &outMax) const;

  // Global AABB bounds for the entire loaded model
  bool getGlobalBounds(glm::vec3 &outMin, glm::vec3 &outMax) const;

private:
  struct Vertex {
    glm::vec3 pos;
    glm::vec2 uv;
    glm::vec3 normal;
  };

  struct ObjectBounds {
    glm::vec3 aabbMin = glm::vec3(1e30f);
    glm::vec3 aabbMax = glm::vec3(-1e30f);
    bool hasBounds = false;
  };

  struct Submesh {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLsizei vertexCount = 0;
    MaterialAsset material;

    // NEW: separate names
    std::string objectName;   // from OBJ "o ..." (via tinyobj shape.name)
    std::string materialName; // from MTL "newmtl ..." used via "usemtl ..."

    // For debugging
    std::string debugName;

    // Bounds for this submesh (still useful)
    glm::vec3 aabbMin = glm::vec3(1e30f);
    glm::vec3 aabbMax = glm::vec3(-1e30f);
    bool hasBounds = false;
  };

private:
  struct ObjectTRSOverride {
    glm::vec3 posLocal{0.0f};
    glm::vec3 rotDegLocal{0.0f};
    glm::vec3 scaleLocal{1.0f};
    bool enabled = false;

    bool hasPivot = false;
    glm::vec3 pivotLocal{0.0f};
  };
  std::unordered_map<std::string, ObjectTRSOverride> mObjectTRS;

  static std::string makeKey(const std::string &obj, const std::string &mtl) {
    return obj + "|" + mtl;
  }

  std::vector<Submesh> mSubmeshes;

  // NEW: aggregate bounds per OBJ object name
  std::unordered_map<std::string, ObjectBounds> mObjectBounds;

  // NEW: per-object yaw override (degrees)
  struct YawOverride {
    float yawDeg = 0.0f;
    glm::vec3 pivotLocal = glm::vec3(0.0f);
    bool hasPivot = false;
  };

  std::unordered_map<std::string, YawOverride> mYawOverride;
};
