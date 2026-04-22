#include "PlayerInteractionSystem.h"

#include "AppState.h"
#include "ECS/Components.h"
#include "ECS/Systems/DestructionSystem.h"
#include "Keyboard.h"
#include "Mouse.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace {
struct GameplayHit {
  bool hit = false;
  bool fallbackHit = false;
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

glm::vec3 axeLaserStart(const AppState &state, const TransformComponent &camTr,
                        const glm::vec3 &front) {
  glm::vec3 right, up;
  cameraBasis(front, right, up);
  return camTr.position + right * state.axeOffset.x + up * state.axeOffset.y +
         front * (std::abs(state.axeOffset.z) + 0.10f);
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
    best.fallbackHit = true;
    best.entityId = entity;
    best.distance = t;
    best.position = origin + dir * t;
    best.normal = normal;
  }
}

GameplayHit fallbackSceneRaycast(AppState &state, const glm::vec3 &origin,
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

GameplayHit gameplayRaycast(AppState &state, const glm::vec3 &origin,
                            const glm::vec3 &dir, float maxDistance,
                            uint32_t ignoreEntity, float radius) {
  GameplayHit best;

  PhysicsRaycastResult physics =
      state.physicsSystem.raycast(origin, dir, maxDistance, ignoreEntity);
  if (physics.hit) {
    best.hit = true;
    best.fallbackHit = false;
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

bool damageDestructibleHit(AppState &state, Registry &reg,
                           const GameplayHit &hit,
                           const glm::vec3 &impulseDirection, float damage) {
  if (!hit.hit || hit.entityId == 0 ||
      !reg.has<DestructibleComponent>(hit.entityId))
    return false;

  auto &destructible = reg.get<DestructibleComponent>(hit.entityId);
  if (!destructible.enabled || destructible.fractured)
    return false;

  destructible.health = std::max(0.0f, destructible.health - damage);
  if (destructible.health <= 0.0f) {
    DestructionSystem::fractureEntity(state.scene, state.assets, hit.entityId,
                                      hit.position, impulseDirection,
                                      &state.physicsSystem);
  }
  return true;
}

bool damageTreeHit(AppState &state, Registry &reg, const GameplayHit &hit,
                   const glm::vec3 &impulseDirection, float damage) {
  if (!hit.hit || hit.entityId == 0 || !reg.has<TreeComponent>(hit.entityId))
    return false;

  auto &tree = reg.get<TreeComponent>(hit.entityId);
  tree.health = std::max(0.0f, tree.health - damage);
  if (tree.health > 0.0f)
    return true;

  const bool hasRenderableTree = reg.has<MeshComponent>(hit.entityId);
  if (!hasRenderableTree &&
      !state.terrainSystem.convertTreeToEntity(hit.entityId)) {
    state.terrainSystem.chopTree(hit.entityId);
    return true;
  }

  DestructibleComponent *destructible = nullptr;
  if (reg.has<DestructibleComponent>(hit.entityId)) {
    destructible = &reg.get<DestructibleComponent>(hit.entityId);
  } else {
    destructible = &reg.emplace<DestructibleComponent>(hit.entityId);
  }

  destructible->enabled = true;
  destructible->health = 0.0f;
  destructible->shardCount = std::max(destructible->shardCount, 18);
  destructible->shardScale = std::min(destructible->shardScale, 0.55f);
  destructible->explosionForce = std::max(destructible->explosionForce, 16.0f);
  destructible->upwardImpulse = std::max(destructible->upwardImpulse, 4.0f);
  destructible->hideOriginal = true;

  DestructionSystem::fractureEntity(state.scene, state.assets, hit.entityId,
                                    hit.position, impulseDirection,
                                    &state.physicsSystem);
  return true;
}

void setGameplayDebug(AppState &state, Registry &reg, const GameplayHit &hit,
                      const char *kind, const char *missReason) {
  state.debugGameplayHit = hit.hit;
  state.debugGameplayHitId = hit.entityId;
  state.debugGameplayHitDist = hit.hit ? hit.distance : 0.0f;
  state.debugGameplayHitKind = kind ? kind : "";
  state.debugGameplayMissReason = hit.hit ? "" : (missReason ? missReason : "");
  state.debugGameplayHitName.clear();
  if (hit.entityId != 0 && reg.has<NameComponent>(hit.entityId))
    state.debugGameplayHitName = reg.get<NameComponent>(hit.entityId).name;

  state.debugGrabHitId = hit.entityId;
  state.debugGrabHitDist = hit.hit ? hit.distance : 0.0f;
  state.debugGrabHitName = state.debugGameplayHitName;
}

bool raycastTerrain(const TerrainSystem &terrain, const glm::vec3 &origin,
                    const glm::vec3 &dir, float maxDistance,
                    glm::vec3 &outHit) {
  if (!terrain.isEnabled())
    return false;
  constexpr float kStep = 0.5f;
  for (float t = 0.0f; t <= maxDistance; t += kStep) {
    const glm::vec3 p = origin + dir * t;
    if (!terrain.isChunkLoadedAt(p.x, p.z))
      continue;
    const float h = terrain.getHeightAt(p.x, p.z);
    if (p.y <= h) {
      outHit = glm::vec3(p.x, h, p.z);
      return true;
    }
  }
  return false;
}

void beginGrab(AppState &state, Registry &reg, const TransformComponent &camTr,
               const glm::vec3 &front) {
  GameplayHit hit =
      gameplayRaycast(state, camTr.position, front, 20.0f, state.playerId, 0.2f);
  setGameplayDebug(state, reg, hit, "Grab", "No grabbable hit");
  if (!hit.hit || hit.entityId == 0 || !reg.has<TransformComponent>(hit.entityId))
    return;

  bool canGrab = hit.entityId != state.playerId;
  if (reg.has<MeshComponent>(hit.entityId)) {
    const auto &mesh = reg.get<MeshComponent>(hit.entityId);
    if (mesh.isTerrain || mesh.isViewModel)
      canGrab = false;
  }
  if (!canGrab)
    return;

  state.grabbedEntityId = hit.entityId;
  state.grabbedDistance = std::max(1.0f, hit.distance);
  state.grabbedHadRigidbody = false;
  state.grabbedPrevBodyType = 0;
  state.grabbedIsTreeInstance = false;
  state.grabbedPrefab.clear();
  state.grabbedInstanceIndex = 0;
  state.grabbedBaseMatrix = glm::mat4(1.0f);

  if (reg.has<RigidbodyComponent>(hit.entityId)) {
    auto &rb = reg.get<RigidbodyComponent>(hit.entityId);
    state.grabbedHadRigidbody = true;
    state.grabbedPrevBodyType = static_cast<int>(rb.type);
    if (rb.type == RigidbodyComponent::Type::Static)
      rb.type = RigidbodyComponent::Type::Kinematic;
  }

  if (reg.has<TreeComponent>(hit.entityId)) {
    auto &tree = reg.get<TreeComponent>(hit.entityId);
    state.grabbedIsTreeInstance = true;
    state.grabbedPrefab = tree.prefabName;
    state.grabbedInstanceIndex = tree.instanceIndex;
    glm::mat4 instM;
    if (state.terrainSystem.getPrefabInstanceMatrix(tree.prefabName,
                                                    tree.instanceIndex, instM)) {
      state.grabbedBaseMatrix = instM;
      state.grabbedOffset = glm::vec3(instM[3]) - camTr.position;
      return;
    }
  }

  state.grabbedOffset = front * state.grabbedDistance;
}

void updateGrab(AppState &state, Registry &reg, float dt, const glm::vec3 &front,
                const glm::vec3 &right, const glm::vec3 &up) {
  if (state.grabbedEntityId == 0)
    return;
  if (!reg.has<TransformComponent>(state.grabbedEntityId) ||
      state.playerId == 0 || !reg.has<TransformComponent>(state.playerId)) {
    state.grabbedEntityId = 0;
    return;
  }

  auto &grabTr = reg.get<TransformComponent>(state.grabbedEntityId);
  const auto &camTr = reg.get<TransformComponent>(state.playerId);
  const float dragScale = 0.01f * state.grabbedDistance;
  state.grabbedOffset += right * (-state.debugMouseDX) * dragScale;
  state.grabbedOffset += up * (state.debugMouseDY) * dragScale;

  const glm::vec3 targetPos = camTr.position + state.grabbedOffset;
  const glm::vec3 oldPos = grabTr.position;
  const glm::vec3 delta = targetPos - oldPos;
  const float invDt = dt > 0.0001f ? 1.0f / dt : 0.0f;
  state.grabbedReleaseVelocity = delta * invDt;

  if (reg.has<RigidbodyComponent>(state.grabbedEntityId)) {
    auto &rb = reg.get<RigidbodyComponent>(state.grabbedEntityId);
    rb.pendingLinearVelocity = delta * invDt * 0.85f;
    rb.setLinearVelocity = true;
  }

  grabTr.position = targetPos;

  state.debugGrabPrefab.clear();
  state.debugGrabInstance = -1;
  state.debugGrabMoved = false;
  if (reg.has<TreeComponent>(state.grabbedEntityId)) {
    auto &tree = reg.get<TreeComponent>(state.grabbedEntityId);
    state.debugGrabPrefab = tree.prefabName;
    state.debugGrabInstance = static_cast<int>(tree.instanceIndex);
    glm::mat4 instM = state.grabbedBaseMatrix;
    instM[3] = glm::vec4(targetPos, 1.0f);
    state.debugGrabMoved =
        state.terrainSystem.setPrefabInstanceMatrix(tree.prefabName,
                                                    tree.instanceIndex, instM);
  }
}

void endGrab(AppState &state, Registry &reg) {
  if (state.grabbedEntityId != 0 && reg.has<TreeComponent>(state.grabbedEntityId)) {
    if (state.terrainSystem.convertTreeToEntity(state.grabbedEntityId)) {
      RigidbodyComponent *rb = nullptr;
      if (reg.has<RigidbodyComponent>(state.grabbedEntityId))
        rb = &reg.get<RigidbodyComponent>(state.grabbedEntityId);
      else
        rb = &reg.emplace<RigidbodyComponent>(state.grabbedEntityId);
      rb->type = RigidbodyComponent::Type::Dynamic;
      rb->lockRotation = false;
      rb->pendingLinearVelocity = state.grabbedReleaseVelocity;
      rb->setLinearVelocity = true;
    }
  }

  if (state.grabbedEntityId != 0 && state.grabbedHadRigidbody &&
      reg.has<RigidbodyComponent>(state.grabbedEntityId)) {
    auto &rb = reg.get<RigidbodyComponent>(state.grabbedEntityId);
    rb.type = static_cast<RigidbodyComponent::Type>(state.grabbedPrevBodyType);
  }

  state.grabbedEntityId = 0;
  state.grabbedHadRigidbody = false;
  state.grabbedPrevBodyType = 0;
  state.grabbedIsTreeInstance = false;
  state.grabbedPrefab.clear();
  state.grabbedInstanceIndex = 0;
  state.grabbedBaseMatrix = glm::mat4(1.0f);
  state.grabbedReleaseVelocity = glm::vec3(0.0f);
}
} // namespace

void PlayerInteractionSystem::reset() { mCraterCooldown = 0.0f; }

void PlayerInteractionSystem::update(AppState &state, float dt) {
  Registry &reg = state.scene.registry();
  if (state.playerId == 0 || !reg.has<CameraComponent>(state.playerId) ||
      !reg.has<TransformComponent>(state.playerId) ||
      !isAlive(reg, state.playerId)) {
    state.playerId = findPlayerCamera(reg);
  }
  if (state.playerId == 0 || !reg.has<TransformComponent>(state.playerId))
    return;

  const auto &camTr = reg.get<TransformComponent>(state.playerId);
  const glm::vec3 front = cameraForwardFromRotation(camTr.rotation);
  glm::vec3 right, up;
  cameraBasis(front, right, up);

  state.debugCamFront = front;
  state.debugCamUp = glm::vec3(0.0f, 1.0f, 0.0f);
  state.debugGameplayAimOrigin = camTr.position;
  state.debugGameplayAimDirection = front;

  const bool primaryDown = Mouse::button(GLFW_MOUSE_BUTTON_LEFT);
  const bool primaryPressed = Mouse::buttonWentDown(GLFW_MOUSE_BUTTON_LEFT);
  const bool primaryReleased = Mouse::buttonWentUp(GLFW_MOUSE_BUTTON_LEFT);
  const bool grabModifier =
      Keyboard::key(GLFW_KEY_LEFT_SHIFT) || Keyboard::key(GLFW_KEY_RIGHT_SHIFT);

  if (primaryPressed && grabModifier && state.grabbedEntityId == 0) {
    beginGrab(state, reg, camTr, front);
  } else if (primaryPressed && !grabModifier &&
             state.activeSlot == AppState::HotbarSlot::Axe && state.axeEnabled &&
             state.grabbedEntityId == 0) {
    constexpr float kLaserDistance = 240.0f;
    constexpr float kLaserRadius = 0.35f;
    GameplayHit hit = gameplayRaycast(state, camTr.position, front,
                                      kLaserDistance, state.playerId,
                                      kLaserRadius);

    const glm::vec3 visualStart = axeLaserStart(state, camTr, front);
    const glm::vec3 visualEnd =
        hit.hit ? hit.position : camTr.position + front * kLaserDistance;
    state.projectiles.setLaserBeam(visualStart, visualEnd, hit.hit,
                                   hit.entityId, hit.normal, 0.24f);

    state.debugGameplayHitPosition = visualEnd;
    const bool damagedTree = damageTreeHit(state, reg, hit, front, 3.0f);
    const bool damagedDestructible =
        !damagedTree && damageDestructibleHit(state, reg, hit, front, 100.0f);
    setGameplayDebug(state, reg, hit,
                     damagedTree      ? "Laser: tree"
                     : damagedDestructible ? "Laser: destructible"
                                           : "Laser",
                     "Laser missed every gameplay target");
  }

  if (state.grabbedEntityId != 0 && primaryDown)
    updateGrab(state, reg, dt, front, right, up);

  if (primaryReleased)
    endGrab(state, reg);

  if (mCraterCooldown > 0.0f)
    mCraterCooldown -= dt;

  const bool secondaryDown =
      Mouse::button(GLFW_MOUSE_BUTTON_RIGHT) ||
      glfwGetMouseButton(state.window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
  if (secondaryDown && mCraterCooldown <= 0.0f) {
    constexpr float kToolDistance = 100.0f;
    GameplayHit hit = gameplayRaycast(state, camTr.position, front,
                                      kToolDistance, state.playerId, 0.25f);
    const bool damagedTree = damageTreeHit(state, reg, hit, front, 1.0f);
    const bool damaged =
        damagedTree || damageDestructibleHit(state, reg, hit, front, 100.0f);
    if (damaged) {
      setGameplayDebug(state, reg, hit,
                       damagedTree ? "Mouse2: tree" : "Mouse2: destructible",
                       nullptr);
      mCraterCooldown = 0.18f;
    } else {
      glm::vec3 terrainHit;
      const bool hitTerrain =
          raycastTerrain(state.terrainSystem, camTr.position, front,
                         kToolDistance, terrainHit);
      if (hitTerrain && state.terrainSystem.applyHeightBrush(terrainHit, 2.5f,
                                                             -0.8f)) {
        GameplayHit terrainDebug;
        terrainDebug.hit = true;
        terrainDebug.distance = glm::length(terrainHit - camTr.position);
        terrainDebug.position = terrainHit;
        terrainDebug.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        setGameplayDebug(state, reg, terrainDebug, "Mouse2: terrain", nullptr);
        mCraterCooldown = 0.1f;
      } else {
        setGameplayDebug(state, reg, hit, "Mouse2", "No terrain or entity hit");
      }
    }
  }
}
