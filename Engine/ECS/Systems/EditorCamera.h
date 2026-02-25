#pragma once

#include "Mouse.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Free-floating editor camera controlled entirely by mouse.
//   Right-drag  → orbit around focus point
//   Middle-drag → pan sideways/up/down
//   Scroll      → zoom in/out
//   F key       → focus on selected entity position
class EditorCamera {
public:
  // Camera state
  glm::vec3 focusPoint = glm::vec3(0.0f);
  float distance = 8.0f;
  float yaw = -90.0f;  // degrees
  float pitch = 25.0f; // degrees

  // Tuning
  float orbitSensitivity = 0.25f;
  float panSensitivity = 0.01f;
  float zoomSensitivity = 0.8f;
  float minDistance = 0.5f;
  float maxDistance = 200.0f;

  // Call once per frame. Pass whether ImGui wants the mouse.
  void update(GLFWwindow *window, bool imguiWantsMouse) {
    if (imguiWantsMouse)
      return;

    float dx = (float)Mouse::getDX();
    float dy = (float)Mouse::getDY();

    bool rightDown =
        glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    bool middleDown =
        glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;

    // Alt + Left-click also orbits (like Maya/Blender)
    bool altDown = glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
                   glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
    bool leftDown =
        glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

    if (rightDown || (altDown && leftDown)) {
      // ── Orbit ──
      yaw += dx * orbitSensitivity;
      pitch -= dy * orbitSensitivity;
      pitch = std::clamp(pitch, -89.0f, 89.0f);
    } else if (middleDown ||
               (altDown &&
                glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) ==
                    GLFW_PRESS)) {
      // ── Pan ──
      glm::vec3 right = getRightVector();
      glm::vec3 up = getUpVector();
      focusPoint -= right * dx * panSensitivity * distance;
      focusPoint += up * dy * panSensitivity * distance;
    }

    // ── Scroll zoom ──
    double scrollX, scrollY;
    // We read scroll from Mouse utility if available, otherwise handle via GLFW
    // For now we use a simple approach via Mouse scroll
    scrollY = Mouse::getSCrollDY();
    if (scrollY != 0.0) {
      distance -= (float)scrollY * zoomSensitivity;
      distance = std::clamp(distance, minDistance, maxDistance);
    }
  }

  // Focus camera on a world-space point (e.g. selected entity)
  void focusOn(const glm::vec3 &target) {
    focusPoint = target;
    // Optionally adjust distance based on object size
    distance = std::clamp(distance, 3.0f, maxDistance);
  }

  glm::vec3 getPosition() const {
    float yawRad = glm::radians(yaw);
    float pitchRad = glm::radians(pitch);
    glm::vec3 offset;
    offset.x = distance * cos(pitchRad) * cos(yawRad);
    offset.y = distance * sin(pitchRad);
    offset.z = distance * cos(pitchRad) * sin(yawRad);
    return focusPoint + offset;
  }

  glm::vec3 getForwardVector() const {
    return glm::normalize(focusPoint - getPosition());
  }

  glm::vec3 getRightVector() const {
    return glm::normalize(
        glm::cross(getForwardVector(), glm::vec3(0.0f, 1.0f, 0.0f)));
  }

  glm::vec3 getUpVector() const {
    return glm::normalize(glm::cross(getRightVector(), getForwardVector()));
  }

  glm::mat4 getViewMatrix() const {
    glm::vec3 pos = getPosition();
    return glm::lookAt(pos, focusPoint, glm::vec3(0.0f, 1.0f, 0.0f));
  }
};
