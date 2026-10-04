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
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
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

namespace {
glm::quat physicsRotation(const TransformComponent &transform, bool locked) {
  // A locked character's pitch belongs to its camera, never its capsule.
  return glm::quat(glm::vec3(locked ? 0.f : glm::radians(transform.rotation.x),
                            glm::radians(transform.rotation.y),
                            locked ? 0.f : glm::radians(transform.rotation.z)));
}

glm::vec3 safeAbsScale(const glm::vec3 &scale) {
  return glm::max(glm::abs(scale), glm::vec3(0.001f));
}

float maxComponent(const glm::vec3 &v) {
  return std::max(v.x, std::max(v.y, v.z));
}

glm::vec3 colliderBodyPosition(const TransformComponent &transform,
                               const ColliderComponent &collider, bool locked) {
  return transform.position + physicsRotation(transform, locked) *
                                  (collider.offset * transform.scale);
}

bool colliderShapeChanged(const RigidbodyComponent &rigidBody,
                          const TransformComponent &transform,
                          const ColliderComponent &collider) {
  return glm::distance(rigidBody.lastScale, transform.scale) > 0.001f ||
         glm::distance(rigidBody.lastColliderDimensions,
                       collider.dimensions) > 0.001f ||
         glm::distance(rigidBody.lastColliderOffset, collider.offset) >
             0.001f ||
         rigidBody.lastColliderShape != static_cast<int>(collider.shape);
}
} // namespace

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
  mPhysicsSystem->SetGravity(JPH::Vec3(0.0f, -9.8f, 0.0f));
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
      const ColliderComponent *collider =
          registry.has<ColliderComponent>(entity)
              ? &registry.get<ColliderComponent>(entity)
              : nullptr;
      if (collider && colliderShapeChanged(rb, transform, *collider)) {
        removeBody(rb.bodyID);
        rb.bodyID = 0xFFFFFFFF;
        continue;
      }

      // Detect if transform was manually modified outside PhysicsSystem (e.g.
      // by Gizmos)
      if (glm::distance(rb.lastPosition, transform.position) > 0.001f ||
          glm::distance(rb.lastRotation, transform.rotation) > 0.001f) {

        const glm::vec3 bodyPos =
            collider ? colliderBodyPosition(transform, *collider, rb.lockRotation && registry.has<CameraComponent>(entity))
                     : transform.position;
        JPH::RVec3 jphPos((JPH::Real)bodyPos.x, (JPH::Real)bodyPos.y,
                          (JPH::Real)bodyPos.z);

        glm::quat q = physicsRotation(transform, rb.lockRotation && registry.has<CameraComponent>(entity));
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
    const glm::vec3 colliderScale = safeAbsScale(transform.scale);

    JPH::ShapeRefC shape;
    if (collider.shape == ColliderComponent::Shape::Box) {
      const glm::vec3 dimensions =
          glm::max(collider.dimensions * colliderScale, glm::vec3(0.02f));
      // Jolt boxes take half-extents
      shape = new JPH::BoxShape(JPH::Vec3(dimensions.x * 0.5f,
                                          dimensions.y * 0.5f,
                                          dimensions.z * 0.5f));
    } else if (collider.shape == ColliderComponent::Shape::Sphere) {
      const float radius =
          std::max(0.01f, collider.dimensions.x * maxComponent(colliderScale));
      shape = new JPH::SphereShape(radius);
    } else if (collider.shape == ColliderComponent::Shape::Capsule) {
      const float radius =
          std::max(0.01f, collider.dimensions.x *
                              std::max(colliderScale.x, colliderScale.z));
      const float scaledHeight = collider.dimensions.y * colliderScale.y;
      const float halfHeight =
          std::max(0.05f, (scaledHeight - 2.0f * radius) * 0.5f);
      shape = new JPH::CapsuleShape(halfHeight, radius);
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

    const bool cameraBody = rigidBody.lockRotation && registry.has<CameraComponent>(entity);
    glm::quat q = physicsRotation(transform, cameraBody);
    const glm::vec3 bodyPos = colliderBodyPosition(transform, collider, cameraBody);
    JPH::RVec3 position((JPH::Real)bodyPos.x, (JPH::Real)bodyPos.y,
                        (JPH::Real)bodyPos.z);
    JPH::Quat rotation(q.x, q.y, q.z, q.w);

    JPH::BodyCreationSettings settings(shape, position, rotation, motionType,
                                       layer);
    settings.mRestitution = rigidBody.restitution;
    settings.mFriction = rigidBody.friction;
    if (rigidBody.lockRotation)
      settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
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
      rigidBody.lastScale = transform.scale;
      rigidBody.lastColliderDimensions = collider.dimensions;
      rigidBody.lastColliderOffset = collider.offset;
      rigidBody.lastColliderShape = static_cast<int>(collider.shape);
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
      JPH::Vec3 velocity = bodyInterface.GetLinearVelocity(id);

      auto &transform = registry.get<TransformComponent>(entity);
      const glm::vec3 bodyPos(position.GetX(), position.GetY(),
                              position.GetZ());
      const ColliderComponent *collider =
          registry.has<ColliderComponent>(entity)
              ? &registry.get<ColliderComponent>(entity)
              : nullptr;

      glm::quat q(rotation.GetW(), rotation.GetX(), rotation.GetY(),
                  rotation.GetZ());
      transform.position =
          collider ? bodyPos - q * (collider->offset * transform.scale)
                   : bodyPos;

      if (!rigidBody.lockRotation) {
        glm::vec3 euler = glm::eulerAngles(q);
        transform.rotation = glm::degrees(euler);
      }

      rigidBody.linearVelocity =
          glm::vec3(velocity.GetX(), velocity.GetY(), velocity.GetZ());
      rigidBody.lastPosition = transform.position;
      rigidBody.lastRotation = transform.rotation;
      rigidBody.lastScale = transform.scale;
      if (collider) {
        rigidBody.lastColliderDimensions = collider->dimensions;
        rigidBody.lastColliderOffset = collider->offset;
        rigidBody.lastColliderShape = static_cast<int>(collider->shape);
      }
    }
  }
}

auto ::PhysicsSystem::raycast(glm::vec3 origin, glm::vec3 direction,
                              float maxDistance,
                              uint32_t ignoreEntityId)
    -> PhysicsRaycastResult {
  PhysicsRaycastResult result;

  glm::vec3 dir = direction;
  if (glm::length(dir) < 1e-6f)
    return result;
  dir = glm::normalize(dir);

  float remaining = maxDistance;
  float traveled = 0.0f;
  const float skipEps = 0.05f;

  for (int attempt = 0; attempt < 4 && remaining > 0.0f; ++attempt) {
    JPH::RVec3 jphOrigin(origin.x, origin.y, origin.z);
    JPH::Vec3 jphDirection(dir.x * remaining, dir.y * remaining,
                           dir.z * remaining);
    JPH::RRayCast ray(jphOrigin, jphDirection);

    JPH::RayCastResult hit;
    bool hasHit = mPhysicsSystem->GetNarrowPhaseQuery().CastRay(ray, hit);
    if (!hasHit)
      break;

    const float hitDist = hit.mFraction * remaining;
    JPH::RVec3 hitPos = jphOrigin + hit.mFraction * jphDirection;

    uint32_t hitEntity = 0;
    glm::vec3 hitNormal(0.0f);
    {
      JPH::BodyLockRead lock(mPhysicsSystem->GetBodyLockInterface(),
                             hit.mBodyID);
      if (lock.Succeeded()) {
        const JPH::Body &hitBody = lock.GetBody();
        hitEntity = static_cast<uint32_t>(hitBody.GetUserData());
        JPH::Vec3 normal = hitBody.GetShape()->GetSurfaceNormal(
            hit.mSubShapeID2, hitPos - hitBody.GetPosition());
        normal = hitBody.GetRotation() * normal;
        hitNormal = glm::vec3(normal.GetX(), normal.GetY(), normal.GetZ());
      }
    }

    if (ignoreEntityId != 0 && hitEntity == ignoreEntityId) {
      traveled += hitDist + skipEps;
      remaining = maxDistance - traveled;
      origin += dir * (hitDist + skipEps);
      continue;
    }

    result.hit = true;
    result.distance = traveled + hitDist;
    result.position = glm::vec3(hitPos.GetX(), hitPos.GetY(), hitPos.GetZ());
    result.normal = hitNormal;
    result.entityId = hitEntity;
    break;
  }

  return result;
}

void ::PhysicsSystem::removeBody(uint32_t bodyId) {
  if (!mPhysicsSystem || bodyId == 0xFFFFFFFF)
    return;

  auto &bodyInterface = mPhysicsSystem->GetBodyInterface();
  JPH::BodyID id(bodyId);
  if (bodyInterface.IsAdded(id)) {
    bodyInterface.RemoveBody(id);
  }
  bodyInterface.DestroyBody(id);
}

using EnginePhysics = ::PhysicsSystem;

void EnginePhysics::setGravity(glm::vec3 gravity) {
  if (mPhysicsSystem) {
    mPhysicsSystem->SetGravity(JPH::Vec3(gravity.x, gravity.y, gravity.z));
  }
}

glm::vec3 EnginePhysics::getGravity() const {
  if (mPhysicsSystem) {
    JPH::Vec3 g = mPhysicsSystem->GetGravity();
    return glm::vec3(g.GetX(), g.GetY(), g.GetZ());
  }
  return glm::vec3(0.0f, -9.81f, 0.0f);
}
