#pragma once
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "Keyboard.h"
#include "Logger.h"
#include "Mouse.h"
#include <GLFW/glfw3.h>

#define SOL_ALL_SAFETIES_ON 1
#define SOL_PRINT_ERRORS 1
#include <sol/sol.hpp>

// Maps readable key names ("W", "A", "SPACE", etc.) to GLFW key codes
inline int keyNameToGLFW(const std::string &name) {
  // Letters
  if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z')
    return GLFW_KEY_A + (name[0] - 'A');
  if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z')
    return GLFW_KEY_A + (name[0] - 'a');
  // Numbers
  if (name.size() == 1 && name[0] >= '0' && name[0] <= '9')
    return GLFW_KEY_0 + (name[0] - '0');
  // Special keys
  if (name == "SPACE")
    return GLFW_KEY_SPACE;
  if (name == "ENTER")
    return GLFW_KEY_ENTER;
  if (name == "ESCAPE")
    return GLFW_KEY_ESCAPE;
  if (name == "TAB")
    return GLFW_KEY_TAB;
  if (name == "LSHIFT")
    return GLFW_KEY_LEFT_SHIFT;
  if (name == "RSHIFT")
    return GLFW_KEY_RIGHT_SHIFT;
  if (name == "LCTRL")
    return GLFW_KEY_LEFT_CONTROL;
  if (name == "RCTRL")
    return GLFW_KEY_RIGHT_CONTROL;
  if (name == "UP")
    return GLFW_KEY_UP;
  if (name == "DOWN")
    return GLFW_KEY_DOWN;
  if (name == "LEFT")
    return GLFW_KEY_LEFT;
  if (name == "RIGHT")
    return GLFW_KEY_RIGHT;
  return GLFW_KEY_UNKNOWN;
}

// A lightweight handle that scripts use to query/modify ECS data.
struct EntityProxy {
  EntityId id;
  Registry *reg;
};

// Register all script API bindings into a sol::state
inline void registerScriptBindings(sol::state &lua, Registry &registry) {
  // ── Vec3 type ──────────────────────────────────────────────────────
  auto vec3Type = lua.new_usertype<glm::vec3>(
      "Vec3", sol::constructors<glm::vec3(), glm::vec3(float, float, float)>(),
      "x", &glm::vec3::x, "y", &glm::vec3::y, "z", &glm::vec3::z);

  // ── Entity proxy ───────────────────────────────────────────────────
  auto entityType = lua.new_usertype<EntityProxy>("Entity");

  entityType["id"] = [](const EntityProxy &e) -> uint32_t { return e.id; };

  entityType["get_position"] = [](const EntityProxy &e) -> glm::vec3 {
    if (e.reg->has<TransformComponent>(e.id))
      return e.reg->get<TransformComponent>(e.id).position;
    return glm::vec3(0.0f);
  };

  entityType["set_position"] = [](const EntityProxy &e, float x, float y,
                                  float z) {
    if (e.reg->has<TransformComponent>(e.id))
      e.reg->get<TransformComponent>(e.id).position = {x, y, z};
  };

  entityType["get_rotation"] = [](const EntityProxy &e) -> glm::vec3 {
    if (e.reg->has<TransformComponent>(e.id))
      return e.reg->get<TransformComponent>(e.id).rotation;
    return glm::vec3(0.0f);
  };

  entityType["set_rotation"] = [](const EntityProxy &e, float x, float y,
                                  float z) {
    if (e.reg->has<TransformComponent>(e.id))
      e.reg->get<TransformComponent>(e.id).rotation = {x, y, z};
  };

  entityType["get_scale"] = [](const EntityProxy &e) -> glm::vec3 {
    if (e.reg->has<TransformComponent>(e.id))
      return e.reg->get<TransformComponent>(e.id).scale;
    return glm::vec3(1.0f);
  };

  entityType["set_scale"] = [](const EntityProxy &e, float x, float y,
                               float z) {
    if (e.reg->has<TransformComponent>(e.id))
      e.reg->get<TransformComponent>(e.id).scale = {x, y, z};
  };

  entityType["get_name"] = [](const EntityProxy &e) -> std::string {
    if (e.reg->has<NameComponent>(e.id))
      return e.reg->get<NameComponent>(e.id).name;
    return "";
  };

  // ── Input table ────────────────────────────────────────────────────
  auto inputTable = lua.create_named_table("input");

  inputTable["key_down"] = [](const std::string &name) -> bool {
    int key = keyNameToGLFW(name);
    return key != GLFW_KEY_UNKNOWN && Keyboard::key(key);
  };

  inputTable["mouse_dx"] = []() -> float { return Mouse::getDX(); };
  inputTable["mouse_dy"] = []() -> float { return Mouse::getDY(); };

  // ── Logging table ──────────────────────────────────────────────────
  auto logTable = lua.create_named_table("log");

  logTable["info"] = [](const std::string &msg) { LOG_INFO("Script", msg); };
  logTable["warn"] = [](const std::string &msg) { LOG_WARN("Script", msg); };
  logTable["error"] = [](const std::string &msg) { LOG_ERROR("Script", msg); };
}
