#include "ECS/Systems/PhysicsSystem.h"
#include "ECS/Components.h"

// Jolt must be included before any other Jolt headers or standard headers that
// might conflict
#include <Jolt/Jolt.h>
#define JPH_SUPPRESS_WARNINGS
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>

#include "Assets/PrimitiveMeshGenerator.h"
#include "Rendering/Shader.h"

#include <iostream>

using namespace JPH;

namespace Layers {
static constexpr ObjectLayer NON_MOVING = 0;
static constexpr ObjectLayer MOVING = 1;
static constexpr ObjectLayer NUM_LAYERS = 2;
}; // namespace Layers

namespace BroadPhaseLayers {
static constexpr BroadPhaseLayer NON_MOVING(0);
static constexpr BroadPhaseLayer MOVING(1);
static constexpr uint NUM_LAYERS(2);
}; // namespace BroadPhaseLayers

class ::PhysicsSystem::ObjectLayerPairFilterImpl
    : public ObjectLayerPairFilter {
public:
  virtual bool ShouldCollide(ObjectLayer inObject1,
                             ObjectLayer inObject2) const override {
    switch (inObject1) {
    case Layers::NON_MOVING:
      return inObject2 ==
             Layers::MOVING; // Non moving only collides with moving
    case Layers::MOVING:
      return true; // Moving collides with everything
    default:
      JPH_ASSERT(false);
      return false;
    }
  }
};

class ::PhysicsSystem::BPLayerInterfaceImpl final
    : public BroadPhaseLayerInterface {
public:
  BPLayerInterfaceImpl() {
    mObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
    mObjectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
  }

  virtual uint GetNumBroadPhaseLayers() const override {
    return BroadPhaseLayers::NUM_LAYERS;
  }

  virtual BroadPhaseLayer
  GetBroadPhaseLayer(ObjectLayer inLayer) const override {
    JPH_ASSERT(inLayer < Layers::NUM_LAYERS);
    return mObjectToBroadPhase[inLayer];
  }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  virtual const char *
  GetBroadPhaseLayerName(BroadPhaseLayer inLayer) const override {
    switch ((BroadPhaseLayer::Type)inLayer) {
    case (BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING:
      return "NON_MOVING";
    case (BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:
      return "MOVING";
    default:
      JPH_ASSERT(false);
      return "INVALID";
    }
  }
#endif // JPH_EXTERNAL_PROFILE || JPH_PROFILE_ENABLED
private:
  BroadPhaseLayer mObjectToBroadPhase[Layers::NUM_LAYERS];
};

class ::PhysicsSystem::ObjectVsBroadPhaseLayerFilterImpl
    : public ObjectVsBroadPhaseLayerFilter {
public:
  virtual bool ShouldCollide(ObjectLayer inLayer1,
                             BroadPhaseLayer inLayer2) const override {
    switch (inLayer1) {
    case Layers::NON_MOVING:
      return inLayer2 == BroadPhaseLayers::MOVING;
    case Layers::MOVING:
      return true;
    default:
      JPH_ASSERT(false);
      return false;
    }
  }
};

::PhysicsSystem::PhysicsSystem() {}

::PhysicsSystem::~PhysicsSystem() { shutdown(); }

void ::PhysicsSystem::init() {
  JPH::RegisterDefaultAllocator();

  JPH::Factory::sInstance = new JPH::Factory();

  JPH::RegisterTypes();

  mTempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);
  mJobSystem = std::make_unique<JPH::JobSystemThreadPool>(
      JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
      std::thread::hardware_concurrency() - 1);

  mBroadPhaseLayerInterface = std::make_unique<BPLayerInterfaceImpl>();
  mObjectVsBroadphaseLayerFilter =
      std::make_unique<ObjectVsBroadPhaseLayerFilterImpl>();
  mObjectVsObjectLayerFilter = std::make_unique<ObjectLayerPairFilterImpl>();

  mPhysicsSystem = std::make_unique<JPH::PhysicsSystem>();
  mPhysicsSystem->Init(1024, 0, 1024, 1024, *mBroadPhaseLayerInterface,
                       *mObjectVsBroadphaseLayerFilter,
                       *mObjectVsObjectLayerFilter);

  mDebugCube = PrimitiveMeshGenerator::createCube();
  mDebugSphere = PrimitiveMeshGenerator::createSphere(16, 16);
}

void ::PhysicsSystem::shutdown() {
  if (mPhysicsSystem) {
    mPhysicsSystem.reset();
    mJobSystem.reset();
    mTempAllocator.reset();
    mBroadPhaseLayerInterface.reset();
    mObjectVsBroadphaseLayerFilter.reset();
    mObjectVsObjectLayerFilter.reset();
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
  }

  delete mDebugCube;
  mDebugCube = nullptr;
  delete mDebugSphere;
  mDebugSphere = nullptr;
}

void ::PhysicsSystem::update(Registry &registry, float dt) {
  if (!mPhysicsSystem || dt <= 0.0f)
    return;

  createBodies(registry);

  // Apply pending forces/velocities from scripts before stepping
  auto rv = registry.view<RigidbodyComponent>();
  auto &bodyInterface = mPhysicsSystem->GetBodyInterface();
  for (auto entity : rv) {
    if (!registry.has<TransformComponent>(entity))
      continue;

    auto &rb = registry.get<RigidbodyComponent>(entity);
    auto &transform = registry.get<TransformComponent>(entity);

    if (rb.bodyID != 0xFFFFFFFF) {
      JPH::BodyID id(rb.bodyID);

      // Detect if transform was manually modified outside PhysicsSystem (e.g.
      // by Gizmos)
      if (glm::distance(rb.lastPosition, transform.position) > 0.001f ||
          glm::distance(rb.lastRotation, transform.rotation) > 0.001f) {

        JPH::RVec3 jphPos((JPH::Real)transform.position.x,
                          (JPH::Real)transform.position.y,
                          (JPH::Real)transform.position.z);

        glm::quat q(glm::vec3(glm::radians(transform.rotation.x),
                              glm::radians(transform.rotation.y),
                              glm::radians(transform.rotation.z)));
        JPH::Quat jphRot(q.x, q.y, q.z, q.w);

        bodyInterface.SetPositionAndRotation(id, jphPos, jphRot,
                                             JPH::EActivation::Activate);

        rb.lastPosition = transform.position;
        rb.lastRotation = transform.rotation;
      }

      if (rb.setLinearVelocity) {
        bodyInterface.SetLinearVelocity(id,
                                        JPH::Vec3(rb.pendingLinearVelocity.x,
                                                  rb.pendingLinearVelocity.y,
                                                  rb.pendingLinearVelocity.z));
        bodyInterface.ActivateBody(id);
        rb.setLinearVelocity = false;
      }
      if (glm::length(rb.pendingImpulse) > 0.001f) {
        bodyInterface.AddImpulse(id, JPH::Vec3(rb.pendingImpulse.x,
                                               rb.pendingImpulse.y,
                                               rb.pendingImpulse.z));
        bodyInterface.ActivateBody(id);
        rb.pendingImpulse = glm::vec3(0.0f);
      }
    }
  }

  // Step the simulation
  // 1 collision step per frame for simplicity
  mPhysicsSystem->Update(dt, 1, mTempAllocator.get(), mJobSystem.get());

  syncTransforms(registry);
}

void ::PhysicsSystem::createBodies(Registry &registry) {
  auto &bodyInterface = mPhysicsSystem->GetBodyInterface();

  auto view = registry.view<RigidbodyComponent>();
  for (auto entity : view) {
    if (!registry.has<TransformComponent>(entity) ||
        !registry.has<ColliderComponent>(entity))
      continue;

    auto &rigidBody = registry.get<RigidbodyComponent>(entity);
    if (rigidBody.bodyID != 0xFFFFFFFF)
      continue; // Already created

    auto &transform = registry.get<TransformComponent>(entity);
    auto &collider = registry.get<ColliderComponent>(entity);

    JPH::ShapeRefC shape;
    if (collider.shape == ColliderComponent::Shape::Box) {
      // Jolt boxes take half-extents
      shape = new JPH::BoxShape(JPH::Vec3(collider.dimensions.x * 0.5f,
                                          collider.dimensions.y * 0.5f,
                                          collider.dimensions.z * 0.5f));
    } else if (collider.shape == ColliderComponent::Shape::Sphere) {
      shape = new JPH::SphereShape(collider.dimensions.x);
    } else if (collider.shape == ColliderComponent::Shape::Capsule) {
      shape = new JPH::CapsuleShape(collider.dimensions.y * 0.5f,
                                    collider.dimensions.x);
    } else {
      shape = new JPH::BoxShape(JPH::Vec3(0.5f, 0.5f, 0.5f));
    }

    JPH::EMotionType motionType;
    JPH::ObjectLayer layer;

    if (rigidBody.type == RigidbodyComponent::Type::Static) {
      motionType = JPH::EMotionType::Static;
      layer = Layers::NON_MOVING;
    } else if (rigidBody.type == RigidbodyComponent::Type::Kinematic) {
      motionType = JPH::EMotionType::Kinematic;
      layer = Layers::MOVING;
    } else {
      motionType = JPH::EMotionType::Dynamic;
      layer = Layers::MOVING;
    }

    // Convert transform to Jolt Math
    JPH::RVec3 position((JPH::Real)transform.position.x,
                        (JPH::Real)transform.position.y,
                        (JPH::Real)transform.position.z);

    // Extract quaternion from glm rotation matrix
    glm::mat4 tMat = transform.getMatrix();
    // glm::mat4 scale is baked in; don't extract rotation from scaled mat,
    // better to construct purely from pitch/yaw/roll or normalize the columns
    glm::quat q(glm::vec3(glm::radians(transform.rotation.x),
                          glm::radians(transform.rotation.y),
                          glm::radians(transform.rotation.z)));

    JPH::Quat rotation(q.x, q.y, q.z, q.w);

    JPH::BodyCreationSettings settings(shape, position, rotation, motionType,
                                       layer);
    settings.mRestitution = rigidBody.restitution;
    settings.mFriction = rigidBody.friction;
    if (rigidBody.type == RigidbodyComponent::Type::Dynamic) {
      settings.mOverrideMassProperties =
          JPH::EOverrideMassProperties::CalculateInertia;
      settings.mMassPropertiesOverride.mMass = rigidBody.mass;
    }

    JPH::Body *body = bodyInterface.CreateBody(settings);
    if (body) {
      body->SetUserData(static_cast<uint64_t>(entity));
      rigidBody.bodyID = body->GetID().GetIndexAndSequenceNumber();
      bodyInterface.AddBody(body->GetID(), JPH::EActivation::Activate);

      rigidBody.lastPosition = transform.position;
      rigidBody.lastRotation = transform.rotation;
    }
  }
}

void ::PhysicsSystem::syncTransforms(Registry &registry) {
  auto &bodyInterface = mPhysicsSystem->GetBodyInterface();

  auto view = registry.view<RigidbodyComponent>();
  for (auto entity : view) {
    if (!registry.has<TransformComponent>(entity))
      continue;

    auto &rigidBody = registry.get<RigidbodyComponent>(entity);
    if (rigidBody.bodyID == 0xFFFFFFFF ||
        rigidBody.type == RigidbodyComponent::Type::Static)
      continue; // Skip invalid or static bodies

    JPH::BodyID id(rigidBody.bodyID);
    if (bodyInterface.IsActive(id)) {
      JPH::RVec3 position = bodyInterface.GetCenterOfMassPosition(id);
      JPH::Quat rotation = bodyInterface.GetRotation(id);

      auto &transform = registry.get<TransformComponent>(entity);
      transform.position =
          glm::vec3(position.GetX(), position.GetY(), position.GetZ());

      glm::quat q(rotation.GetW(), rotation.GetX(), rotation.GetY(),
                  rotation.GetZ());
      glm::vec3 euler = glm::eulerAngles(q);
      transform.rotation = glm::degrees(euler);

      rigidBody.lastPosition = transform.position;
      rigidBody.lastRotation = transform.rotation;
    }
  }
}

void ::PhysicsSystem::drawDebugColliders(Registry &reg, const glm::mat4 &view,
                                         const glm::mat4 &proj,
                                         Shader &shader) {
  if (!mDebugCube || !mDebugSphere)
    return;

  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  glDisable(GL_CULL_FACE);

  shader.activate();
  shader.setMat4("view", view);
  shader.setMat4("projection", proj);

  auto group = reg.view<ColliderComponent>();
  for (auto entity : group) {
    if (!reg.has<TransformComponent>(entity))
      continue;
    const auto &col = reg.get<ColliderComponent>(entity);
    const auto &transform = reg.get<TransformComponent>(entity);

    glm::vec4 color(1.0f); // Default white
    if (reg.has<RigidbodyComponent>(entity)) {
      auto type = reg.get<RigidbodyComponent>(entity).type;
      if (type == RigidbodyComponent::Type::Static) {
        color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f); // Red = Static
      } else if (type == RigidbodyComponent::Type::Kinematic) {
        color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f); // Green = Kinematic
      } else {
        color = glm::vec4(0.2f, 0.6f, 1.0f, 1.0f); // Blue = Dynamic
      }
    }

    shader.setVec4("uColor", color);

    glm::mat4 model = glm::translate(glm::mat4(1.0f), transform.position);
    model = glm::rotate(model, glm::radians(transform.rotation.y),
                        glm::vec3(0, 1, 0));
    model = glm::rotate(model, glm::radians(transform.rotation.x),
                        glm::vec3(1, 0, 0));
    model = glm::rotate(model, glm::radians(transform.rotation.z),
                        glm::vec3(0, 0, 1));

    // The physics collider size needs to be adjusted.
    // The visual mesh scale might be different from the collider size if the
    // collider is offset or scaled. Since primitive meshes are generated at
    // bounds [-0.5, 0.5] (size 1.0), and Jolt boxes take half-extents, we need
    // to make sure the visual wireframe matches perfectly. Our Component uses
    // "dimensions" which is full size for Box, Radius for Sphere
    glm::vec3 drawScale(1.0f);
    OBJModel *drawModel = mDebugCube;

    if (col.shape == ColliderComponent::Shape::Box) {
      drawScale = col.dimensions; // Full dimensions, our cube is 1x1x1
      drawModel = mDebugCube;
    } else if (col.shape == ColliderComponent::Shape::Sphere) {
      drawScale =
          glm::vec3(col.dimensions.x *
                    2.0f); // Radius * 2 to get diameter, sphere is 1 diameter
      drawModel = mDebugSphere;
    } else if (col.shape == ColliderComponent::Shape::Capsule) {
      // Using sphere to approximate capsule for now, stretching it along Y
      drawScale = glm::vec3(col.dimensions.x * 2.0f,
                            col.dimensions.y + col.dimensions.x * 2.0f,
                            col.dimensions.x * 2.0f);
      drawModel = mDebugSphere;
    }

    model = glm::scale(model, drawScale);

    shader.setMat4("model", model);
    shader.setBool("uUseColor", true);

    // OBJModel::draw applies its own internal modeling if not bypassed.
    // Passing 0 for pos/rot/scale effectively bypasses it and uses our shader
    // uniform.
    drawModel->draw(shader, glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f));
  }

  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glEnable(GL_CULL_FACE);
}

auto ::PhysicsSystem::raycast(glm::vec3 origin, glm::vec3 direction,
                              float maxDistance) -> PhysicsRaycastResult {
  PhysicsRaycastResult result;

  JPH::RVec3 jphOrigin(origin.x, origin.y, origin.z);
  JPH::Vec3 jphDirection(direction.x * maxDistance, direction.y * maxDistance,
                         direction.z * maxDistance);
  JPH::RRayCast ray(jphOrigin, jphDirection);

  JPH::RayCastResult hit;
  // Use default broad/object layer filters (accept everything)
  bool hasHit = mPhysicsSystem->GetNarrowPhaseQuery().CastRay(ray, hit);

  if (hasHit) {
    result.hit = true;
    result.distance = hit.mFraction * maxDistance;

    JPH::RVec3 hitPos = jphOrigin + hit.mFraction * jphDirection;
    result.position = glm::vec3(hitPos.GetX(), hitPos.GetY(), hitPos.GetZ());

    auto &bodyInterface = mPhysicsSystem->GetBodyInterface();
    JPH::BodyLockRead lock(mPhysicsSystem->GetBodyLockInterface(), hit.mBodyID);
    if (lock.Succeeded()) {
      const JPH::Body &hitBody = lock.GetBody();
      result.entityId = static_cast<uint32_t>(hitBody.GetUserData());

      // Try to get normal
      JPH::Vec3 normal = hitBody.GetShape()->GetSurfaceNormal(
          hit.mSubShapeID2, hitPos - hitBody.GetPosition());
      normal = hitBody.GetRotation() * normal;
      result.normal = glm::vec3(normal.GetX(), normal.GetY(), normal.GetZ());
    }
  }

  return result;
}
