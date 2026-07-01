#pragma once
#include "AssetManager.h"
#include "Rendering/Material.h"
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

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
  enum class AssetType { None, OBJ, GLTF, FBX };

  MeshComponent(bool vis = true, bool shadow = true)
      : visible(vis), castsShadow(shadow) {}

  MeshComponent(class OBJModel *m, bool vis = true, bool shadow = true)
      : objModel(m), type(AssetType::OBJ), visible(vis), castsShadow(shadow) {}

  MeshComponent(class FBXModel *m, bool vis = true, bool shadow = true)
      : gltfModel(m), type(AssetType::GLTF), visible(vis), castsShadow(shadow) {
  }

  MeshComponent(class UFBXModel *m, bool vis = true, bool shadow = true)
      : ufbxModel(m), type(AssetType::FBX), visible(vis), castsShadow(shadow) {}

  class OBJModel *objModel = nullptr;
  class FBXModel *gltfModel = nullptr;
  class UFBXModel *ufbxModel = nullptr;
  std::string assetId;
  OBJHandle objHandle{};
  GLTFHandle gltfHandle{};
  UFBXHandle ufbxHandle{};
  AssetType type = AssetType::None;
  bool visible = true;
  bool castsShadow = true;
  bool isTerrain = false; // Terrain chunks get height-based biome coloring
  bool isWater = false; // Water plane (avoid terrain shading path)
  bool isViewModel = false; // Rendered in viewmodel pass only
};

struct MaterialOverrideComponent {
  bool enabled = true;
  MaterialAsset material{};

  // Source texture paths used for scene serialization and editor display.
  std::string albedoPath;
  std::string normalPath;
  std::string roughnessPath;
  std::string metallicPath;
  std::string aoPath;
};

struct InstancedMeshComponent {
  struct InstanceCluster {
    glm::vec3 center = {0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
  };

  MeshComponent::AssetType type = MeshComponent::AssetType::None;
  class OBJModel *objModel = nullptr;
  class UFBXModel *ufbxModel = nullptr;
  OBJHandle objHandle{};
  UFBXHandle ufbxHandle{};
  std::vector<glm::mat4> instanceTransforms;
  std::vector<InstanceCluster> instanceClusters;
  std::vector<uint32_t> clusterInstanceIndices;
  std::vector<glm::mat4> culledTransforms;
  std::vector<glm::mat4> shadowCulledTransforms;
  uint64_t lastCullKey = 0;
  uint64_t shadowLastCullKey = 0;
  int lastVisibleCount = 0;
  int shadowLastVisibleCount = 0;
  int lastTestedClusterCount = 0;
  int shadowLastTestedClusterCount = 0;
  int lastVisibleClusterCount = 0;
  int shadowLastVisibleClusterCount = 0;
  unsigned int instanceVBO = 0;
  size_t instanceVBOCapacity = 0; // bytes currently allocated on GPU
  unsigned int shadowInstanceVBO = 0;
  size_t shadowInstanceVBOCapacity = 0;
  float maxDrawDistance = 800.0f; // cull instances beyond this distance
  float shadowMaxDrawDistance = 400.0f;
  float instanceCullRadius = 8.0f;
  // When true, instance fragments use terrain procedural shading path.
  bool useTerrainShading = false;
  bool isDirty = true;
  bool clusterDataDirty = true;
  bool mainCacheDirty = true;
  bool shadowCacheDirty = true;
  bool visible = true;
  bool castsShadow = true;
};

// Tags an entity so it is completely ignored by Scene serialization and
// Scene::clear()
struct TransientComponent {};

enum class EntityLifecycleState : uint8_t {
  Alive = 0,
  Disabled = 1,
  PendingDestroy = 2
};

struct LifecycleComponent {
  EntityLifecycleState state = EntityLifecycleState::Alive;
};

struct HierarchyComponent {
  uint32_t parent = 0;
  std::vector<uint32_t> children;
};

// Physics components for Jolt Integration
struct RigidbodyComponent {
  enum class Type { Static, Kinematic, Dynamic };
  Type type = Type::Dynamic;
  float mass = 1.0f;
  float friction = 0.5f;
  float restitution = 0.0f;
  // When true, physics does not override Transform rotation for this body.
  bool lockRotation = false;

  // Pending forces/velocities from scripts to be applied this frame
  glm::vec3 pendingLinearVelocity = {0.0f, 0.0f, 0.0f};
  bool setLinearVelocity = false;
  glm::vec3 pendingImpulse = {0.0f, 0.0f, 0.0f};
  glm::vec3 linearVelocity = {0.0f, 0.0f, 0.0f};

  // Tracking last known physics transform to detect external changes (e.g.
  // Gizmos)
  glm::vec3 lastPosition = {0.0f, 0.0f, 0.0f};
  glm::vec3 lastRotation = {0.0f, 0.0f, 0.0f};
  glm::vec3 lastScale = {1.0f, 1.0f, 1.0f};
  glm::vec3 lastColliderDimensions = {1.0f, 1.0f, 1.0f};
  glm::vec3 lastColliderOffset = {0.0f, 0.0f, 0.0f};
  int lastColliderShape = -1;

  // Internal Jolt Body ID wrapper
  uint32_t bodyID = 0xFFFFFFFF; // JPH::BodyID::cInvalidBodyID
};

struct ColliderComponent {
  enum class Shape { Box, Sphere, Capsule };
  Shape shape = Shape::Box;

  // Local-space center offset from the entity origin. This keeps fitted
  // colliders aligned when a mesh pivot is not at its visual center.
  glm::vec3 offset = {0.0f, 0.0f, 0.0f};

  // Dimensions depend on the shape (full size for Box, radius for Sphere).
  glm::vec3 dimensions = {1.0f, 1.0f, 1.0f};
};

struct DestructibleComponent {
  bool enabled = true;
  float health = 100.0f;
  int shardCount = 10;
  float shardScale = 0.75f;
  float explosionForce = 18.0f;
  float upwardImpulse = 5.0f;
  bool hideOriginal = true;
  bool fractured = false;
};

struct DestructionShardComponent {
  glm::vec3 velocity = {0.0f, 0.0f, 0.0f};
  glm::vec3 angularVelocity = {0.0f, 0.0f, 0.0f}; // Degrees per second
  float age = 0.0f;
  float lifetime = 8.0f;
  float settledTime = 0.0f;
  float shadowLifetime = 0.75f;
  float cleanupDelay = 1.0f;
  bool editorPreview = true;
  bool settled = false;
};

struct BoundsComponent {
  BoundsComponent() = default;
  explicit BoundsComponent(float r) : radius(r) {}
  BoundsComponent(const glm::vec3 &offset, float r)
      : centerOffset(offset), radius(r) {}

  glm::vec3 centerOffset = {0.0f, 0.0f, 0.0f};
  float radius = 1.0f;
};

struct TreeComponent {
  float health = 3.0f;
  uint32_t instanceIndex = 0;
  uint32_t chunkInstanceSlot = 0;
  std::string prefabName;
  int chunkX = 0;
  int chunkZ = 0;
};

struct SpaceshipComponent {
  bool enabled = true;
  float dryMassKg = 8500.0f;
  float fuelMassKg = 4200.0f;
  float mainThrustN = 180000.0f;
  float specificImpulseSec = 315.0f;
  float attitudeThrustN = 8500.0f;
  float dragAreaM2 = 18.0f;
  float heatShieldRating = 1.0f;
  float boostMultiplier = 2.5f;
  float damping = 0.985f;
  float maxSpeed = 160.0f;
  float turnRateDeg = 95.0f;
  float turnResponsiveness = 8.0f;
  float bankAngleDeg = 28.0f;
  float bankResponsiveness = 6.0f;
  float idleDrag = 0.55f;
  float brakeDrag = 1.25f;
  glm::vec3 centerOfMass = {0.0f, 0.0f, 0.0f};
  glm::vec3 velocity = {0.0f, 0.0f, 0.0f};
  glm::vec3 angularVelocity = {0.0f, 0.0f, 0.0f};
  float throttle = 0.0f;
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

struct ScriptComponent {
  std::string scriptPath;   // Path to .lua file
  bool initialized = false; // Has on_spawn been called?
  int envRef = -1;          // Lua registry ref to script environment
};
