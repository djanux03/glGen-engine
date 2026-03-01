#pragma once

#include "ECS/Registry.h"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

// Forward declarations
namespace JPH {
class PhysicsSystem;
class TempAllocatorImpl;
class JobSystemThreadPool;
} // namespace JPH

struct PhysicsRaycastResult {
  bool hit = false;
  float distance = 0.0f;
  glm::vec3 position{0.0f, 0.0f, 0.0f};
  glm::vec3 normal{0.0f, 0.0f, 0.0f};
  uint32_t entityId = 0;
};

class PhysicsSystem {
public:
  PhysicsSystem();
  ~PhysicsSystem();

  // Perform a physics raycast into the world
  PhysicsRaycastResult raycast(glm::vec3 origin, glm::vec3 direction,
                               float maxDistance = 1000.0f);

  // Initialize the Jolt physics engine
  void init();

  // Clean up physics engine resources
  void shutdown();

  // Step the simulation forward and sync transforms to the ECS
  void update(Registry &registry, float dt);

private:
  // Jolt Physics core systems
  std::unique_ptr<JPH::PhysicsSystem> mPhysicsSystem;
  std::unique_ptr<JPH::TempAllocatorImpl> mTempAllocator;
  std::unique_ptr<JPH::JobSystemThreadPool> mJobSystem;

  // Custom interface implementations (BPLayerInterface,
  // ObjectVsBroadPhaseLayerFilter, ObjectLayerPairFilter)
  class BPLayerInterfaceImpl;
  class ObjectVsBroadPhaseLayerFilterImpl;
  class ObjectLayerPairFilterImpl;

  std::unique_ptr<BPLayerInterfaceImpl> mBroadPhaseLayerInterface;
  std::unique_ptr<ObjectVsBroadPhaseLayerFilterImpl>
      mObjectVsBroadphaseLayerFilter;
  std::unique_ptr<ObjectLayerPairFilterImpl> mObjectVsObjectLayerFilter;

  // Sync ECS -> Jolt and Jolt -> ECS
  void createBodies(Registry &registry);
  void syncTransforms(Registry &registry);

  // Debug meshes
  class OBJModel *mDebugCube = nullptr;
  class OBJModel *mDebugSphere = nullptr;

public:
  // Render wireframe representations of all ColliderComponents
  void drawDebugColliders(Registry &reg, const glm::mat4 &view,
                          const glm::mat4 &proj, class Shader &shader);
};
