#pragma once

#include "ECS/Components.h"
#include "ECS/Registry.h"

#include <glm/glm.hpp>
#include <string>

// =============================================================================
// The player archetype
// =============================================================================
// What "a Player" is made of, in one place: the name the gameplay systems
// look for, a camera, a capsule, and a locked-rotation dynamic body.
//
// This used to live only inside VkEditor::createPlayerEntity, which meant a
// player could be created by clicking Create > Player and no other way --
// a scene assembled over the command port (or by a Lua game setting itself
// up) could not produce one. Both paths now build the same entity, so a
// script-authored player behaves identically to an editor-authored one.
// =============================================================================

namespace gameplay {

inline EntityId spawnPlayer(Registry &reg, const glm::vec3 &pos, float yawDeg,
                            float pitchDeg, const std::string &scriptPath) {
  const EntityId id = reg.create();

  // VkPlayerControllerSystem finds the player by this name (or by a primary
  // CameraComponent), so both are set deliberately.
  reg.emplace<NameComponent>(id, NameComponent("Player"));
  reg.emplace<TransformComponent>(
      id, TransformComponent{pos, glm::vec3(pitchDeg, yawDeg, 0.0f),
                             glm::vec3(1.0f, 1.0f, 1.0f)});

  auto &cam = reg.emplace<CameraComponent>(id);
  cam.isPrimary = true;
  cam.fov = 60.0f;
  cam.yaw = yawDeg;
  cam.pitch = pitchDeg;

  auto &rb = reg.emplace<RigidbodyComponent>(id);
  rb.type = RigidbodyComponent::Type::Dynamic;
  rb.mass = 70.0f;
  rb.lockRotation = true; // upright: physics must not topple the camera

  auto &col = reg.emplace<ColliderComponent>(id);
  col.shape = ColliderComponent::Shape::Capsule;
  col.dimensions = glm::vec3(0.6f, 1.8f, 0.6f);
  // Transform position is the camera eye (1.62 m above ground). Keep the
  // capsule centred around the character's hips; a positive offset placed
  // the collider above the eye and made physics fight the controller's ground
  // snap every frame.
  col.offset = glm::vec3(0.0f, -0.72f, 0.0f);

  if (!scriptPath.empty())
    reg.emplace<ScriptComponent>(id).scriptPath = scriptPath;

  return id;
}

} // namespace gameplay
