#include "VkPlayerControllerSystem.h"

#include "VkAppState.h"
#include "subsystems/VkTerrainSubsystem.h"

#include "ECS/Components.h"
#include "Keyboard.h"
#include "Mouse.h"
#include "Gameplay/PlayerSpawn.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

// Ground detection combines two candidates, same "take the higher one" shape
// as the old app's TerrainSystem-height + physics-raycast blend: the
// procedural terrain height (VkTerrainSubsystem::heightAt(), a pure noise
// sample -- always correct immediately, independent of whether a chunk's
// Jolt heightfield has streamed in yet) and the physics raycast (catches
// props/platforms standing above the terrain, which should win over bare
// ground). Relying on the raycast alone left the player able to fall
// through: terrain chunks only get a Jolt collider within
// collisionChunkRadius of the CAMERA and that streams in asynchronously, so
// a raycast can legitimately find nothing under the player for the first
// several frames after spawn or a teleport.

namespace {
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
} // namespace

void VkPlayerControllerSystem::reset() {
  mVerticalVelocity = 0.0f;
  mGrounded = false;
  mLastPlayerId = 0;
}

void VkPlayerControllerSystem::update(VkAppState &state, float dt, bool active) {
  if (dt <= 0.0f)
    return;

  Registry &reg = state.scene.registry();
  if (state.gameplay.playerId == 0 || !reg.has<CameraComponent>(state.gameplay.playerId) ||
      !reg.has<TransformComponent>(state.gameplay.playerId) ||
      !isAlive(reg, state.gameplay.playerId)) {
    state.gameplay.playerId = findPlayerCamera(reg);
  }

  if (state.gameplay.playerId == 0 || !reg.has<TransformComponent>(state.gameplay.playerId))
    return;

  if (mLastPlayerId != state.gameplay.playerId) {
    mVerticalVelocity = 0.0f;
    mGrounded = false;
    mLastPlayerId = state.gameplay.playerId;
  }

  if (!active)
    return;

  auto &tr = reg.get<TransformComponent>(state.gameplay.playerId);

  const float dx = static_cast<float>(Mouse::getDX());
  const float dy = static_cast<float>(Mouse::getDY());
  state.gameplay.debug.debugMouseDX = dx;
  state.gameplay.debug.debugMouseDY = dy;

  const float sensitivity = state.input.mouseSensitivity;
  tr.rotation.y -= dx * sensitivity;
  tr.rotation.x += dy * sensitivity;
  tr.rotation.x = std::clamp(tr.rotation.x, -89.0f, 89.0f);
  if (tr.rotation.y > 180.0f)
    tr.rotation.y -= 360.0f;
  if (tr.rotation.y < -180.0f)
    tr.rotation.y += 360.0f;

  state.gameplay.debug.debugYaw = tr.rotation.y;
  state.gameplay.debug.debugPitch = tr.rotation.x;

  const glm::vec3 forward = cameraForwardFromRotation(tr.rotation);
  glm::vec3 flatForward(forward.x, 0.0f, forward.z);
  if (glm::length(flatForward) < 0.0001f)
    flatForward = glm::vec3(0.0f, 0.0f, -1.0f);
  flatForward = glm::normalize(flatForward);

  glm::vec3 right = glm::normalize(glm::cross(flatForward, glm::vec3(0.0f, 1.0f, 0.0f)));
  glm::vec3 wishMove(0.0f);
  if (Keyboard::key(GLFW_KEY_W))
    wishMove += flatForward;
  if (Keyboard::key(GLFW_KEY_S))
    wishMove -= flatForward;
  if (Keyboard::key(GLFW_KEY_D))
    wishMove += right;
  if (Keyboard::key(GLFW_KEY_A))
    wishMove -= right;

  if (glm::length(wishMove) > 0.0001f)
    wishMove = glm::normalize(wishMove);

  const bool sprint =
      Keyboard::key(GLFW_KEY_LEFT_SHIFT) || Keyboard::key(GLFW_KEY_RIGHT_SHIFT);
  const float baseSpeed = 5.0f;
  const float speed =
      baseSpeed *
      (!state.input.creativeFlight && sprint ? state.input.runMult : 1.0f);
  tr.position += wishMove * speed * dt;

  if (state.input.creativeFlight) {
    mVerticalVelocity = 0.0f;
    mGrounded = false;
    const float verticalInput = (Keyboard::key(GLFW_KEY_SPACE) ? 1.0f : 0.0f) -
                                (sprint ? 1.0f : 0.0f);
    tr.position.y += verticalInput * speed * dt;

    if (reg.has<CameraComponent>(state.gameplay.playerId)) {
      auto &cam = reg.get<CameraComponent>(state.gameplay.playerId);
      cam.front = forward;
      cam.right = right;
      cam.up = glm::vec3(0.0f, 1.0f, 0.0f);
      cam.yaw = tr.rotation.y;
      cam.pitch = tr.rotation.x;
    }
    return;
  }

  float groundClearance = 1.65f;
  if (reg.has<ColliderComponent>(state.gameplay.playerId)) {
    const auto &col = reg.get<ColliderComponent>(state.gameplay.playerId);
    groundClearance = gameplay::playerEyeHeight(tr, col);
  }
  constexpr float kGroundSnap = 1.2f;
  constexpr float kGravity = -9.8f;
  constexpr float kJumpSpeed = 4.5f;
  constexpr float kMaxFallSpeed = -25.0f;

  mGrounded = false;
  float groundY = -3.402823466e+38F;

  if (state.terrainSubsystem && state.terrainSubsystem->hasTerrain()) {
    // The terrain sampler is available before streamed collision chunks are
    // ready. A ray from eye height can hit an interactive tree capsule and
    // incorrectly treat its crown as walkable ground, snapping the player up.
    groundY = state.terrainSubsystem->heightAt(
        glm::vec2(tr.position.x, tr.position.z));
  } else {
    PhysicsRaycastResult groundHit = state.physicsSystem.raycast(
        tr.position, glm::vec3(0.0f, -1.0f, 0.0f), 5.0f,
        state.gameplay.playerId);
    if (groundHit.hit)
      groundY = groundHit.position.y;
  }

  const float desiredY = groundY + groundClearance;
  if (groundY > -3.0e+38f && tr.position.y <= desiredY + kGroundSnap &&
      mVerticalVelocity <= 0.0f) {
    tr.position.y = desiredY;
    mVerticalVelocity = 0.0f;
    mGrounded = true;
    if (reg.has<RigidbodyComponent>(state.gameplay.playerId)) {
      auto &rb = reg.get<RigidbodyComponent>(state.gameplay.playerId);
      // Ground samples precede streamed colliders. Do not bank Jolt gravity
      // while analytically grounded, then drop through the floor in one step.
      rb.pendingLinearVelocity = rb.linearVelocity;
      rb.pendingLinearVelocity.y = 0;
      rb.setLinearVelocity = true;
    }
  }

  if (mGrounded && Keyboard::key(GLFW_KEY_SPACE)) {
    mVerticalVelocity = kJumpSpeed;
    mGrounded = false;
  }

  if (!mGrounded) {
    mVerticalVelocity = std::max(kMaxFallSpeed, mVerticalVelocity + kGravity * dt);
    tr.position.y += mVerticalVelocity * dt;
  }

  if (reg.has<CameraComponent>(state.gameplay.playerId)) {
    auto &cam = reg.get<CameraComponent>(state.gameplay.playerId);
    cam.front = forward;
    cam.right = right;
    cam.up = glm::vec3(0.0f, 1.0f, 0.0f);
    cam.yaw = tr.rotation.y;
    cam.pitch = tr.rotation.x;
  }
}
