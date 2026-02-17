#pragma once

#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "Mouse.h"
#include <glm/glm.hpp>

class CameraSystem {
public:
  void update(Registry &registry, float mouseSensitivity) {
    // Only update if we are not in UI mode (assuming caller handles this check)
    // For now, we just apply mouse deltas to the primary camera.

    float xoff = (float)Mouse::getDX() * mouseSensitivity;
    float yoff = (float)Mouse::getDY() * mouseSensitivity;

    for (auto entity : registry.view<CameraComponent>()) {
      if (registry.has<LifecycleComponent>(entity)) {
        auto s = registry.get<LifecycleComponent>(entity).state;
        if (s != EntityLifecycleState::Alive)
          continue;
      }

      auto &cam = registry.get<CameraComponent>(entity);
      if (!cam.isPrimary)
        continue;

      cam.yaw += xoff;
      cam.pitch += yoff;

      // Clamp pitch
      if (cam.pitch > 89.0f)
        cam.pitch = 89.0f;
      if (cam.pitch < -89.0f)
        cam.pitch = -89.0f;

      // Update vectors
      glm::vec3 front;
      front.x = cos(glm::radians(cam.yaw)) * cos(glm::radians(cam.pitch));
      front.y = sin(glm::radians(cam.pitch));
      front.z = sin(glm::radians(cam.yaw)) * cos(glm::radians(cam.pitch));
      cam.front = glm::normalize(front);

      // Recalculate right and up
      cam.right =
          glm::normalize(glm::cross(cam.front, glm::vec3(0.0f, 1.0f, 0.0f)));
      cam.up = glm::normalize(glm::cross(cam.right, cam.front));
    }
  }
};
