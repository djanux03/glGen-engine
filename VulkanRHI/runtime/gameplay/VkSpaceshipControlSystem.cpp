#include "VkSpaceshipControlSystem.h"

#include "VkAppState.h"

#include "ECS/Components.h"
#include "Keyboard.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace {
bool isAlive(Registry &reg, uint32_t entity) {
  return !reg.has<LifecycleComponent>(entity) ||
         reg.get<LifecycleComponent>(entity).state ==
             EntityLifecycleState::Alive;
}

glm::vec3 flatForwardFromYaw(float yawDeg) {
  return glm::normalize(glm::vec3(-std::sin(glm::radians(yawDeg)), 0.0f,
                                  -std::cos(glm::radians(yawDeg))));
}

float normalizeAngleDeg(float angle) {
  while (angle > 180.0f)
    angle -= 360.0f;
  while (angle < -180.0f)
    angle += 360.0f;
  return angle;
}

float smoothToward(float current, float target, float responsiveness, float dt) {
  const float alpha =
      1.0f - std::exp(-std::max(0.0f, responsiveness) * std::max(0.0f, dt));
  return current + (target - current) * std::clamp(alpha, 0.0f, 1.0f);
}
} // namespace

void VkSpaceshipControlSystem::reset() {}

void VkSpaceshipControlSystem::update(VkAppState &state, float dt) {
  if (dt <= 0.0f)
    return;

  Registry &reg = state.scene.registry();
  const bool forward = Keyboard::key(GLFW_KEY_UP);
  const bool backward = Keyboard::key(GLFW_KEY_DOWN);
  const bool turnLeft = Keyboard::key(GLFW_KEY_LEFT);
  const bool turnRight = Keyboard::key(GLFW_KEY_RIGHT);

  for (auto e : reg.viewAll<SpaceshipComponent, TransformComponent>()) {
    if (!isAlive(reg, e))
      continue;

    auto &ship = reg.get<SpaceshipComponent>(e);
    if (!ship.enabled)
      continue;

    auto &tr = reg.get<TransformComponent>(e);
    const float turnInput = (turnLeft ? 1.0f : 0.0f) -
                            (turnRight ? 1.0f : 0.0f);

    const float mass = std::max(1.0f, ship.dryMassKg + ship.fuelMassKg);
    const bool boost =
        Keyboard::key(GLFW_KEY_LEFT_SHIFT) || Keyboard::key(GLFW_KEY_RIGHT_SHIFT);
    const float thrustScale = boost ? ship.boostMultiplier : 1.0f;
    const float targetThrottle = (forward ? 1.0f : 0.0f) -
                                 (backward ? 0.45f : 0.0f);
    ship.throttle = smoothToward(ship.throttle, targetThrottle, 7.5f, dt);

    const float speed = glm::length(ship.velocity);
    const float speed01 =
        std::clamp(speed / std::max(1.0f, ship.maxSpeed), 0.0f, 1.0f);
    const float targetYawRate =
        turnInput * ship.turnRateDeg * (0.35f + 0.65f * speed01);
    ship.angularVelocity.y =
        smoothToward(ship.angularVelocity.y, targetYawRate,
                     ship.turnResponsiveness, dt);
    tr.rotation.y = normalizeAngleDeg(tr.rotation.y + ship.angularVelocity.y * dt);

    const float targetBank =
        -turnInput * ship.bankAngleDeg * std::clamp(speed01 + 0.25f, 0.0f, 1.0f);
    tr.rotation.z =
        smoothToward(tr.rotation.z, targetBank, ship.bankResponsiveness, dt);
    tr.rotation.z = normalizeAngleDeg(tr.rotation.z);

    const glm::vec3 shipForward = flatForwardFromYaw(tr.rotation.y);
    const glm::vec3 acceleration =
        shipForward * ((ship.mainThrustN / mass) * thrustScale * ship.throttle);

    ship.velocity += acceleration * dt;
    const float maxSpeed = std::max(1.0f, ship.maxSpeed * thrustScale);
    const float updatedSpeed = glm::length(ship.velocity);
    if (updatedSpeed > maxSpeed)
      ship.velocity = ship.velocity * (maxSpeed / updatedSpeed);

    const float throttleAbs = std::abs(ship.throttle);
    const float drag = throttleAbs < 0.05f ? ship.idleDrag : 0.0f;
    const float brakeDrag = backward && glm::dot(ship.velocity, shipForward) > 0.0f
                                ? ship.brakeDrag
                                : 0.0f;
    const float legacyDamping =
        std::pow(std::clamp(ship.damping, 0.0f, 0.999f), dt * 60.0f);
    const float dragDamping = std::exp(-(drag + brakeDrag) * dt);
    ship.velocity *= legacyDamping * dragDamping;
    tr.position += ship.velocity * dt;

    if (reg.has<RigidbodyComponent>(e)) {
      auto &rb = reg.get<RigidbodyComponent>(e);
      if (rb.type == RigidbodyComponent::Type::Static)
        rb.type = RigidbodyComponent::Type::Kinematic;
      rb.pendingLinearVelocity = ship.velocity;
      rb.setLinearVelocity = true;
    }
  }
}
