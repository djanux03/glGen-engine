#include "VkPlayerInteractionSystem.h"

#include "VkAppState.h"

#include "ECS/Components.h"
#include "Keyboard.h"
#include "Mouse.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

// Phase 1 scope: no CPU TerrainSystem, no ProjectileSystem (its GL-coupled
// draw() would drag glad/Shader symbols into glGenVk's link). Tree-chopping,
// terrain height-brush, and the cosmetic axe-laser visual are dropped.
// DestructionSystem::fractureEntity is also deferred: its .cpp pulls in
// OBJModel/FBXModel/UFBXModel and TerrainSystem.h, all GL-coupled, so it
// can't be linked into glGenVk yet -- destructible hits register damage on
// DestructibleComponent (health reaches 0) but don't spawn fracture shards
// until a Vulkan-side destruction path exists (Phase 2). Generic-entity grab
// is preserved as-is.

namespace {
struct GameplayHit {
  bool hit = false;
  uint32_t entityId = 0;
  float distance = 0.0f;
  glm::vec3 position{0.0f};
  glm::vec3 normal{0.0f, 1.0f, 0.0f};
};

bool isAlive(Registry &reg, uint32_t entity) {
  return !reg.has<LifecycleComponent>(entity) ||
         reg.get<LifecycleComponent>(entity).state == EntityLifecycleState::Alive;
}

uint32_t findPlayerCamera(Registry &reg) {
  for (auto entity : reg.view<CameraComponent>()) {
    if (isAlive(reg, entity))
      return entity;
  }
  return 0;
}

glm::vec3 cameraForwardFromRotation(const glm::vec3 &rotationDeg) {
  glm::vec3 front;
  front.x = -std::sin(glm::radians(rotationDeg.y)) *
            std::cos(glm::radians(rotationDeg.x));
  front.y = std::sin(glm::radians(rotationDeg.x));
  front.z = -std::cos(glm::radians(rotationDeg.y)) *
            std::cos(glm::radians(rotationDeg.x));
  return glm::normalize(front);
}

glm::quat physicsRotation(const TransformComponent &transform) {
  return glm::quat(glm::vec3(glm::radians(transform.rotation.x),
                            glm::radians(transform.rotation.y),
                            glm::radians(transform.rotation.z)));
}

void cameraBasis(const glm::vec3 &front, glm::vec3 &right, glm::vec3 &up) {
  const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
  right = glm::cross(front, worldUp);
  if (glm::length(right) < 0.0001f)
    right = glm::vec3(1.0f, 0.0f, 0.0f);
  right = glm::normalize(right);
  up = glm::normalize(glm::cross(right, front));
}

bool raySphere(const glm::vec3 &origin, const glm::vec3 &dir,
               const glm::vec3 &center, float radius, float &outT,
               glm::vec3 &outNormal) {
  const glm::vec3 oc = origin - center;
  const float b = glm::dot(oc, dir);
  const float c = glm::dot(oc, oc) - radius * radius;
  const float h = b * b - c;
  if (h < 0.0f)
    return false;
  const float sqrtH = std::sqrt(h);
  float t = -b - sqrtH;
  if (t < 0.0f)
    t = -b + sqrtH;
  if (t < 0.0f)
    return false;
  outT = t;
  const glm::vec3 p = origin + dir * t;
  outNormal = glm::length(p - center) > 0.0001f
                  ? glm::normalize(p - center)
                  : glm::vec3(0.0f, 1.0f, 0.0f);
  return true;
}

bool rayAabb(const glm::vec3 &origin, const glm::vec3 &dir,
             const glm::vec3 &minB, const glm::vec3 &maxB, float &outT,
             glm::vec3 &outNormal) {
  float tMin = 0.0f;
  float tMax = 3.402823466e+38F;
  glm::vec3 normal(0.0f);

  for (int axis = 0; axis < 3; ++axis) {
    const float o = origin[axis];
    const float d = dir[axis];
    if (std::abs(d) < 0.000001f) {
      if (o < minB[axis] || o > maxB[axis])
        return false;
      continue;
    }

    float t1 = (minB[axis] - o) / d;
    float t2 = (maxB[axis] - o) / d;
    float faceSign = -1.0f;
    if (t1 > t2) {
      std::swap(t1, t2);
      faceSign = 1.0f;
    }

    if (t1 > tMin) {
      tMin = t1;
      normal = glm::vec3(0.0f);
      normal[axis] = faceSign;
    }
    tMax = std::min(tMax, t2);
    if (tMin > tMax)
      return false;
  }

  outT = tMin;
  outNormal = glm::length(normal) > 0.0001f ? normal
                                            : glm::vec3(0.0f, 1.0f, 0.0f);
  return true;
}

bool shouldSkipEntity(Registry &reg, uint32_t entity, uint32_t ignoreEntity) {
  if (entity == 0 || entity == ignoreEntity || !isAlive(reg, entity) ||
      !reg.has<TransformComponent>(entity))
    return true;
  if (reg.has<MeshComponent>(entity)) {
    const auto &mesh = reg.get<MeshComponent>(entity);
    if (!mesh.visible || mesh.isTerrain || mesh.isViewModel)
      return true;
  }
  return false;
}

void considerCandidate(GameplayHit &best, uint32_t entity, float t,
                       float maxDistance, const glm::vec3 &origin,
                       const glm::vec3 &dir, const glm::vec3 &normal) {
  if (t < 0.0f || t > maxDistance)
    return;
  if (!best.hit || t < best.distance) {
    best.hit = true;
    best.entityId = entity;
    best.distance = t;
    best.position = origin + dir * t;
    best.normal = normal;
  }
}

GameplayHit fallbackSceneRaycast(VkAppState &state, const glm::vec3 &origin,
                                 const glm::vec3 &dir, float maxDistance,
                                 uint32_t ignoreEntity, float radius) {
  GameplayHit best;
  Registry &reg = state.scene.registry();

  for (auto entity : reg.view<TransformComponent>()) {
    if (shouldSkipEntity(reg, entity, ignoreEntity))
      continue;

    const auto &tr = reg.get<TransformComponent>(entity);
    float t = 0.0f;
    glm::vec3 normal(0.0f, 1.0f, 0.0f);

    if (reg.has<ColliderComponent>(entity)) {
      const auto &col = reg.get<ColliderComponent>(entity);
      const glm::vec3 scale = glm::max(glm::abs(tr.scale), glm::vec3(0.001f));
      const glm::vec3 center =
          tr.position + physicsRotation(tr) * (col.offset * tr.scale);

      if (col.shape == ColliderComponent::Shape::Sphere) {
        const float sphereRadius =
            col.dimensions.x * std::max(scale.x, std::max(scale.y, scale.z)) +
            radius;
        if (raySphere(origin, dir, center, sphereRadius, t, normal))
          considerCandidate(best, entity, t, maxDistance, origin, dir, normal);
      } else {
        glm::vec3 halfExtents(0.5f);
        if (col.shape == ColliderComponent::Shape::Box) {
          halfExtents = 0.5f * col.dimensions * scale;
        } else {
          const float capsuleRadius =
              col.dimensions.x * std::max(scale.x, scale.z);
          halfExtents = glm::vec3(capsuleRadius,
                                  col.dimensions.y * scale.y * 0.5f +
                                      capsuleRadius,
                                  capsuleRadius);
        }
        halfExtents += glm::vec3(radius);
        if (rayAabb(origin, dir, center - halfExtents, center + halfExtents, t,
                    normal))
          considerCandidate(best, entity, t, maxDistance, origin, dir, normal);
      }
    }

    if (reg.has<DestructibleComponent>(entity)) {
      float boundsRadius = 0.75f;
      if (reg.has<BoundsComponent>(entity))
        boundsRadius = std::max(0.1f, reg.get<BoundsComponent>(entity).radius);
      const glm::vec3 halfExtents =
          glm::max(glm::abs(tr.scale) * boundsRadius, glm::vec3(0.2f)) +
          glm::vec3(radius);
      if (rayAabb(origin, dir, tr.position - halfExtents,
                  tr.position + halfExtents, t, normal))
        considerCandidate(best, entity, t, maxDistance, origin, dir, normal);
    }
  }

  return best;
}

GameplayHit gameplayRaycast(VkAppState &state, const glm::vec3 &origin,
                            const glm::vec3 &dir, float maxDistance,
                            uint32_t ignoreEntity, float radius) {
  GameplayHit best;

  PhysicsRaycastResult physics =
      state.physicsSystem.raycast(origin, dir, maxDistance, ignoreEntity);
  if (physics.hit) {
    best.hit = true;
    best.entityId = physics.entityId;
    best.distance = physics.distance;
    best.position = physics.position;
    best.normal = physics.normal;
  }

  GameplayHit fallback =
      fallbackSceneRaycast(state, origin, dir, maxDistance, ignoreEntity, radius);
  if (fallback.hit && (!best.hit || fallback.distance <= best.distance + radius))
    best = fallback;

  return best;
}

bool damageDestructibleHit(VkAppState &state, Registry &reg,
                           const GameplayHit &hit,
                           const glm::vec3 &impulseDirection, float damage) {
  if (!hit.hit || hit.entityId == 0 ||
      !reg.has<DestructibleComponent>(hit.entityId))
    return false;

  auto &destructible = reg.get<DestructibleComponent>(hit.entityId);
  if (!destructible.enabled || destructible.fractured)
    return false;

  destructible.health = std::max(0.0f, destructible.health - damage);
  // Fracture spawn deferred to Phase 2 (see file header comment) -- health
  // still reaches 0 and `fractured`/further damage calls are gated by it,
  // just no shard entities are spawned yet.
  (void)impulseDirection;
  return true;
}

void setGameplayDebug(VkAppState &state, Registry &reg, const GameplayHit &hit,
                      const char *kind, const char *missReason) {
  state.gameplay.debug.debugGameplayHit = hit.hit;
  state.gameplay.debug.debugGameplayHitId = hit.entityId;
  state.gameplay.debug.debugGameplayHitDist = hit.hit ? hit.distance : 0.0f;
  state.gameplay.debug.debugGameplayHitKind = kind ? kind : "";
  state.gameplay.debug.debugGameplayMissReason = hit.hit ? "" : (missReason ? missReason : "");
  state.gameplay.debug.debugGameplayHitName.clear();
  if (hit.entityId != 0 && reg.has<NameComponent>(hit.entityId))
    state.gameplay.debug.debugGameplayHitName = reg.get<NameComponent>(hit.entityId).name;

  state.gameplay.debug.debugGrabHitId = hit.entityId;
  state.gameplay.debug.debugGrabHitDist = hit.hit ? hit.distance : 0.0f;
  state.gameplay.debug.debugGrabHitName = state.gameplay.debug.debugGameplayHitName;
}

void beginGrab(VkAppState &state, Registry &reg, const TransformComponent &camTr,
               const glm::vec3 &front) {
  GameplayHit hit =
      gameplayRaycast(state, camTr.position, front, 20.0f, state.gameplay.playerId, 0.2f);
  setGameplayDebug(state, reg, hit, "Grab", "No grabbable hit");
  if (!hit.hit || hit.entityId == 0 || !reg.has<TransformComponent>(hit.entityId))
    return;

  bool canGrab = hit.entityId != state.gameplay.playerId;
  if (reg.has<MeshComponent>(hit.entityId)) {
    const auto &mesh = reg.get<MeshComponent>(hit.entityId);
    if (mesh.isTerrain || mesh.isViewModel)
      canGrab = false;
  }
  if (!canGrab)
    return;

  state.gameplay.grab.grabbedEntityId = hit.entityId;
  state.gameplay.grab.grabbedDistance = std::max(1.0f, hit.distance);
  state.gameplay.grab.grabbedHadRigidbody = false;
  state.gameplay.grab.grabbedPrevBodyType = 0;
  state.gameplay.grab.grabbedIsTreeInstance = false;
  state.gameplay.grab.grabbedPrefab.clear();
  state.gameplay.grab.grabbedInstanceIndex = 0;
  state.gameplay.grab.grabbedBaseMatrix = glm::mat4(1.0f);

  if (reg.has<RigidbodyComponent>(hit.entityId)) {
    auto &rb = reg.get<RigidbodyComponent>(hit.entityId);
    state.gameplay.grab.grabbedHadRigidbody = true;
    state.gameplay.grab.grabbedPrevBodyType = static_cast<int>(rb.type);
    if (rb.type == RigidbodyComponent::Type::Static)
      rb.type = RigidbodyComponent::Type::Kinematic;
  }

  state.gameplay.grab.grabbedOffset = front * state.gameplay.grab.grabbedDistance;
}

void updateGrab(VkAppState &state, Registry &reg, float dt, const glm::vec3 &front,
                const glm::vec3 &right, const glm::vec3 &up) {
  if (state.gameplay.grab.grabbedEntityId == 0)
    return;
  if (!reg.has<TransformComponent>(state.gameplay.grab.grabbedEntityId) ||
      state.gameplay.playerId == 0 || !reg.has<TransformComponent>(state.gameplay.playerId)) {
    state.gameplay.grab.grabbedEntityId = 0;
    return;
  }

  auto &grabTr = reg.get<TransformComponent>(state.gameplay.grab.grabbedEntityId);
  const auto &camTr = reg.get<TransformComponent>(state.gameplay.playerId);
  const float dragScale = 0.01f * state.gameplay.grab.grabbedDistance;
  state.gameplay.grab.grabbedOffset += right * (-state.gameplay.debug.debugMouseDX) * dragScale;
  state.gameplay.grab.grabbedOffset += up * (state.gameplay.debug.debugMouseDY) * dragScale;

  const glm::vec3 targetPos = camTr.position + state.gameplay.grab.grabbedOffset;
  const glm::vec3 oldPos = grabTr.position;
  const glm::vec3 delta = targetPos - oldPos;
  const float invDt = dt > 0.0001f ? 1.0f / dt : 0.0f;
  glm::vec3 releaseVel = delta * invDt;
  constexpr float kMaxReleaseSpeed = 25.0f;
  if (glm::length(releaseVel) > kMaxReleaseSpeed) {
    releaseVel = glm::normalize(releaseVel) * kMaxReleaseSpeed;
  }
  state.gameplay.grab.grabbedReleaseVelocity = releaseVel;

  if (reg.has<RigidbodyComponent>(state.gameplay.grab.grabbedEntityId)) {
    auto &rb = reg.get<RigidbodyComponent>(state.gameplay.grab.grabbedEntityId);
    rb.pendingLinearVelocity = releaseVel * 0.85f;
    rb.setLinearVelocity = true;
  }

  grabTr.position = targetPos;

  state.gameplay.debug.debugGrabPrefab.clear();
  state.gameplay.debug.debugGrabInstance = -1;
  state.gameplay.debug.debugGrabMoved = false;
}

void endGrab(VkAppState &state, Registry &reg) {
  if (state.gameplay.grab.grabbedEntityId != 0 && state.gameplay.grab.grabbedHadRigidbody &&
      reg.has<RigidbodyComponent>(state.gameplay.grab.grabbedEntityId)) {
    auto &rb = reg.get<RigidbodyComponent>(state.gameplay.grab.grabbedEntityId);
    rb.type = static_cast<RigidbodyComponent::Type>(state.gameplay.grab.grabbedPrevBodyType);
  }

  state.gameplay.grab.grabbedEntityId = 0;
  state.gameplay.grab.grabbedHadRigidbody = false;
  state.gameplay.grab.grabbedPrevBodyType = 0;
  state.gameplay.grab.grabbedIsTreeInstance = false;
  state.gameplay.grab.grabbedPrefab.clear();
  state.gameplay.grab.grabbedInstanceIndex = 0;
  state.gameplay.grab.grabbedBaseMatrix = glm::mat4(1.0f);
  state.gameplay.grab.grabbedReleaseVelocity = glm::vec3(0.0f);
}
} // namespace

void VkPlayerInteractionSystem::reset() { mCraterCooldown = 0.0f; }

void VkPlayerInteractionSystem::update(VkAppState &state, float dt) {
  Registry &reg = state.scene.registry();
  if (state.gameplay.playerId == 0 || !reg.has<CameraComponent>(state.gameplay.playerId) ||
      !reg.has<TransformComponent>(state.gameplay.playerId) ||
      !isAlive(reg, state.gameplay.playerId)) {
    state.gameplay.playerId = findPlayerCamera(reg);
  }
  if (state.gameplay.playerId == 0 || !reg.has<TransformComponent>(state.gameplay.playerId))
    return;

  const auto &camTr = reg.get<TransformComponent>(state.gameplay.playerId);
  const glm::vec3 front = cameraForwardFromRotation(camTr.rotation);
  glm::vec3 right, up;
  cameraBasis(front, right, up);

  state.gameplay.debug.debugCamFront = front;
  state.gameplay.debug.debugCamUp = glm::vec3(0.0f, 1.0f, 0.0f);
  state.gameplay.debug.debugGameplayAimOrigin = camTr.position;
  state.gameplay.debug.debugGameplayAimDirection = front;

  const bool primaryDown = Mouse::button(GLFW_MOUSE_BUTTON_LEFT);
  const bool primaryPressed = Mouse::buttonWentDown(GLFW_MOUSE_BUTTON_LEFT);
  const bool primaryReleased = Mouse::buttonWentUp(GLFW_MOUSE_BUTTON_LEFT);
  const bool grabModifier =
      Keyboard::key(GLFW_KEY_LEFT_SHIFT) || Keyboard::key(GLFW_KEY_RIGHT_SHIFT);

  if (primaryPressed && grabModifier && state.gameplay.grab.grabbedEntityId == 0) {
    beginGrab(state, reg, camTr, front);
  } else if (primaryPressed && !grabModifier &&
             state.gameplay.activeSlot == GameplayState::HotbarSlot::Axe && state.gameplay.viewmodel.axeEnabled &&
             state.gameplay.grab.grabbedEntityId == 0) {
    constexpr float kLaserDistance = 240.0f;
    constexpr float kLaserRadius = 0.35f;
    GameplayHit hit = gameplayRaycast(state, camTr.position, front,
                                      kLaserDistance, state.gameplay.playerId,
                                      kLaserRadius);

    state.gameplay.debug.debugGameplayHitPosition =
        hit.hit ? hit.position : camTr.position + front * kLaserDistance;
    const bool damagedDestructible =
        damageDestructibleHit(state, reg, hit, front, 100.0f);
    setGameplayDebug(state, reg, hit,
                     damagedDestructible ? "Laser: destructible" : "Laser",
                     "Laser missed every gameplay target");
  }

  if (state.gameplay.grab.grabbedEntityId != 0 && primaryDown)
    updateGrab(state, reg, dt, front, right, up);

  if (primaryReleased)
    endGrab(state, reg);

  if (mCraterCooldown > 0.0f)
    mCraterCooldown -= dt;

  const bool secondaryDown =
      Mouse::button(GLFW_MOUSE_BUTTON_RIGHT) ||
      (state.window != nullptr &&
       glfwGetMouseButton(state.window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
  if (secondaryDown && mCraterCooldown <= 0.0f) {
    constexpr float kToolDistance = 100.0f;
    GameplayHit hit = gameplayRaycast(state, camTr.position, front,
                                      kToolDistance, state.gameplay.playerId, 0.25f);
    const bool damaged = damageDestructibleHit(state, reg, hit, front, 100.0f);
    if (damaged) {
      setGameplayDebug(state, reg, hit, "Mouse2: destructible", nullptr);
      mCraterCooldown = 0.18f;
    } else {
      setGameplayDebug(state, reg, hit, "Mouse2", "No entity hit (no CPU terrain in Phase 1)");
    }
  }
}
