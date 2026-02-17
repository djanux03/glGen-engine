#pragma once

#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "Keyboard.h"
#include "Mouse.h"
#include <GLFW/glfw3.h>

class MovementSystem {
public:
  void update(Registry &registry, float dt) {
    for (auto entity : registry.view<TransformComponent>()) {
      // We also need PhysicsComponent
      if (!registry.has<PhysicsComponent>(entity))
        continue;

      auto &tr = registry.get<TransformComponent>(entity);
      auto &phys = registry.get<PhysicsComponent>(entity);

      // Handle Camera control if this entity also has a CameraComponent
      if (registry.has<CameraComponent>(entity)) {
        handleInput(registry.get<CameraComponent>(entity), tr, phys);
      }

      // Apply Physics (Gravity)
      if (!phys.onGround) {
        tr.position.y += phys.velocity.y;
        phys.velocity.y -= phys.gravity;

        // Simple ground collision
        float groundY = 0.0f;
        if (tr.position.y <= groundY) {
          tr.position.y = groundY;
          phys.velocity.y = 0.0f;
          phys.onGround = true;
        }
      }
    }
  }

private:
  void handleInput(CameraComponent &cam, TransformComponent &tr,
                   PhysicsComponent &phys) {
    float speed = 0.03f; // Base walk speed

    // Sprint
    if (Keyboard::key(GLFW_KEY_LEFT_SHIFT) ||
        Keyboard::key(GLFW_KEY_RIGHT_SHIFT)) {
      speed *= 2.0f;
    }

    // Calculate forward/right vectors flattened on XZ plane
    glm::vec3 forward =
        glm::normalize(glm::vec3(cam.front.x, 0.0f, cam.front.z));
    glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));

    if (Keyboard::key(GLFW_KEY_W))
      tr.position += forward * speed;
    if (Keyboard::key(GLFW_KEY_S))
      tr.position -= forward * speed;
    if (Keyboard::key(GLFW_KEY_D))
      tr.position += right * speed;
    if (Keyboard::key(GLFW_KEY_A))
      tr.position -= right * speed;

    if (Keyboard::key(GLFW_KEY_UP))
      tr.position.y += speed;
    if (Keyboard::key(GLFW_KEY_DOWN))
      tr.position.y -= speed;

    // Jump
    static bool spaceWasDown = false;
    bool spaceDown = Keyboard::key(GLFW_KEY_SPACE);
    if (spaceDown && !spaceWasDown && phys.onGround) {
      phys.velocity.y = 0.18f; // Jump strength
      phys.onGround = false;
    }
    spaceWasDown = spaceDown;
  }
};
