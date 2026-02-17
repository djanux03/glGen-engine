#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include "AssetManager.h"
#include <string>
#include <vector>
#include <cstdint>

struct TransformComponent {
  glm::vec3 position = {0.0f, 0.0f, 0.0f};
  glm::vec3 rotation = {0.0f, 0.0f, 0.0f}; // Euler angles in degrees
  glm::vec3 scale = {1.0f, 1.0f, 1.0f};

  glm::mat4 getMatrix() const {
    glm::mat4 m(1.0f);
    m = glm::translate(m, position);
    m = glm::rotate(m, glm::radians(rotation.y), glm::vec3(0, 1, 0));
    m = glm::rotate(m, glm::radians(rotation.x), glm::vec3(1, 0, 0));
    m = glm::rotate(m, glm::radians(rotation.z), glm::vec3(0, 0, 1));
    m = glm::scale(m, scale);
    return m;
  }
};

struct MeshComponent {
  enum class AssetType { None, OBJ, FBX };

  MeshComponent(bool vis = true, bool shadow = true)
      : visible(vis), castsShadow(shadow) {}

  MeshComponent(class OBJModel *m, bool vis = true, bool shadow = true)
      : objModel(m), type(AssetType::OBJ), visible(vis), castsShadow(shadow) {}

  MeshComponent(class FBXModel *m, bool vis = true, bool shadow = true)
      : fbxModel(m), type(AssetType::FBX), visible(vis), castsShadow(shadow) {}

  class OBJModel *objModel = nullptr; // Raw pointer to cached model in Scene
  class FBXModel *fbxModel = nullptr; // Raw pointer to cached model in Scene
  std::string assetId;
  OBJHandle objHandle{};
  GLTFHandle gltfHandle{};
  AssetType type = AssetType::None;
  bool visible = true;
  bool castsShadow = true;
};

enum class EntityLifecycleState : uint8_t { Alive = 0, Disabled = 1, PendingDestroy = 2 };

struct LifecycleComponent {
  EntityLifecycleState state = EntityLifecycleState::Alive;
};

struct HierarchyComponent {
  uint32_t parent = 0;
  std::vector<uint32_t> children;
};

struct PhysicsComponent {
  glm::vec3 velocity = {0.0f, 0.0f, 0.0f};
  float gravity = 0.01f;
  bool onGround = false;
};

struct BoundsComponent {
  float radius = 1.0f;
};

struct LODComponent {
  float minDistance = 0.0f;
  float maxDistance = 10000.0f;
};

struct CameraComponent {
  float fov = 50.0f;
  glm::vec3 front = {0.0f, 0.0f, -1.0f};
  glm::vec3 right = {1.0f, 0.0f, 0.0f}; // Cached right vector
  glm::vec3 up = {0.0f, 1.0f, 0.0f};
  float yaw = -90.0f;
  float pitch = 0.0f;
  bool isPrimary = true;
};

struct NameComponent {
  std::string name;
  NameComponent() = default;
  explicit NameComponent(const std::string &n) : name(n) {}
  explicit NameComponent(std::string &&n) : name(std::move(n)) {}
};
