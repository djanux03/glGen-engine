// PhysicsTerrainChunk.cpp — Terrain HeightFieldShape integration
// Split from PhysicsSystem.cpp to avoid 'using namespace JPH' ambiguity.

#include "ECS/Systems/PhysicsSystem.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <glm/glm.hpp>
#include <iostream>
#include <vector>

// Object layer constants (must match PhysicsSystem.cpp — NON_MOVING = 0)
static constexpr JPH::ObjectLayer kLayerNonMoving = 0;

uint32_t PhysicsSystem::addTerrainChunk(const std::vector<float> &heightSamples,
                                        uint32_t sampleCount,
                                        glm::vec2 chunkOrigin,
                                        float chunkWorldSize) {
  if (!mPhysicsSystem || heightSamples.empty())
    return 0xFFFFFFFF;

  const float step = chunkWorldSize / (float)(sampleCount - 1);

  JPH::HeightFieldShapeSettings hfSettings(
      heightSamples.data(), JPH::Vec3(chunkOrigin.x, 0.0f, chunkOrigin.y),
      JPH::Vec3(step, 1.0f, step), sampleCount);

  JPH::ShapeSettings::ShapeResult result = hfSettings.Create();
  if (result.HasError()) {
    std::cerr << "[Physics] HeightFieldShape creation failed: "
              << result.GetError() << "\n";
    return 0xFFFFFFFF;
  }

  // Use explicit Real cast — 0.0_r UDL requires 'using namespace JPH'
  JPH::BodyCreationSettings bodySettings(
      result.Get(), JPH::RVec3(JPH::Real(0), JPH::Real(0), JPH::Real(0)),
      JPH::Quat::sIdentity(), JPH::EMotionType::Static, kLayerNonMoving);

  auto &bodyInterface = mPhysicsSystem->GetBodyInterface();
  JPH::Body *body = bodyInterface.CreateBody(bodySettings);
  if (!body)
    return 0xFFFFFFFF;

  bodyInterface.AddBody(body->GetID(), JPH::EActivation::DontActivate);
  return body->GetID().GetIndexAndSequenceNumber();
}

void PhysicsSystem::removeTerrainChunk(uint32_t bodyId) {
  if (!mPhysicsSystem || bodyId == 0xFFFFFFFF)
    return;

  auto &bodyInterface = mPhysicsSystem->GetBodyInterface();
  JPH::BodyID id(bodyId);
  if (bodyInterface.IsAdded(id)) {
    bodyInterface.RemoveBody(id);
    bodyInterface.DestroyBody(id);
  }
}
