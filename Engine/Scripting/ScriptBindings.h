#pragma once
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "ECS/Systems/PhysicsSystem.h"
#include "Generators/AssetLibrary.h"
#include "Generators/GeneratorRegistry.h"
#include "Generators/TextureGen.h"
#include "Scene/Scene.h"
#include "Keyboard.h"
#include "Logger.h"
#include "Mouse.h"
#include <GLFW/glfw3.h>

#define SOL_ALL_SAFETIES_ON 1
#define SOL_PRINT_ERRORS 1
#include <sol/sol.hpp>

#include "ScriptJson.h"

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
inline void registerScriptBindings(sol::state &lua, Registry &registry,
                                   PhysicsSystem *physics,
                                   Scene *scene = nullptr,
                                   gen::AssetLibrary *assetLibrary = nullptr) {
  // ── Vec3 type ──────────────────────────────────────────────────────
  auto vec3Type = lua.new_usertype<glm::vec3>(
      "Vec3", sol::constructors<glm::vec3(), glm::vec3(float, float, float)>(),
      "x", &glm::vec3::x, "y", &glm::vec3::y, "z", &glm::vec3::z);

  // ── Entity proxy ───────────────────────────────────────────────────
  auto entityType = lua.new_usertype<EntityProxy>("Entity");

  entityType["id"] = [](const EntityProxy &e) -> uint32_t { return e.id; };

  entityType["is_valid"] = [](const EntityProxy &e) -> bool {
    return e.id != 0 && e.reg != nullptr && e.reg->valid(e.id);
  };

  entityType["destroy"] = [](const EntityProxy &e) {
    if (e.id != 0 && e.reg != nullptr && e.reg->valid(e.id)) {
      e.reg->destroy(e.id);
    }
  };

  entityType["get_position"] = [](const EntityProxy &e) -> glm::vec3 {
    if (e.reg && e.reg->has<TransformComponent>(e.id))
      return e.reg->get<TransformComponent>(e.id).position;
    return glm::vec3(0.0f);
  };

  entityType["set_position"] = [](const EntityProxy &e, float x, float y,
                                  float z) {
    if (e.reg && e.reg->has<TransformComponent>(e.id))
      e.reg->get<TransformComponent>(e.id).position = {x, y, z};
  };

  entityType["get_rotation"] = [](const EntityProxy &e) -> glm::vec3 {
    if (e.reg && e.reg->has<TransformComponent>(e.id))
      return e.reg->get<TransformComponent>(e.id).rotation;
    return glm::vec3(0.0f);
  };

  entityType["set_rotation"] = [](const EntityProxy &e, float x, float y,
                                  float z) {
    if (e.reg && e.reg->has<TransformComponent>(e.id))
      e.reg->get<TransformComponent>(e.id).rotation = {x, y, z};
  };

  entityType["get_forward"] = [](const EntityProxy &e) -> glm::vec3 {
    if (e.reg && e.reg->has<TransformComponent>(e.id)) {
      const auto &tr = e.reg->get<TransformComponent>(e.id);
      const float yaw = glm::radians(tr.rotation.y);
      const float pitch = glm::radians(tr.rotation.x);
      glm::vec3 front;
      front.x = -sin(yaw) * cos(pitch);
      front.y = sin(pitch);
      front.z = -cos(yaw) * cos(pitch);
      return glm::normalize(front);
    }
    if (e.reg && e.reg->has<CameraComponent>(e.id)) {
      return e.reg->get<CameraComponent>(e.id).front;
    }
    return glm::vec3(0.0f, 0.0f, -1.0f);
  };

  entityType["get_forward_flat"] = [](const EntityProxy &e) -> glm::vec3 {
    glm::vec3 f(0.0f, 0.0f, -1.0f);
    if (e.reg && e.reg->has<TransformComponent>(e.id)) {
      const auto &tr = e.reg->get<TransformComponent>(e.id);
      const float yaw = glm::radians(tr.rotation.y);
      f = glm::vec3(-sin(yaw), 0.0f, -cos(yaw));
    } else if (e.reg && e.reg->has<CameraComponent>(e.id)) {
      f = e.reg->get<CameraComponent>(e.id).front;
    }
    f.y = 0.0f;
    if (glm::length(f) < 1e-4f)
      return glm::vec3(0.0f, 0.0f, -1.0f);
    return glm::normalize(f);
  };

  entityType["get_right_flat"] = [](const EntityProxy &e) -> glm::vec3 {
    glm::vec3 f = glm::vec3(0.0f, 0.0f, -1.0f);
    if (e.reg && e.reg->has<TransformComponent>(e.id)) {
      const auto &tr = e.reg->get<TransformComponent>(e.id);
      const float yaw = glm::radians(tr.rotation.y);
      f = glm::vec3(-sin(yaw), 0.0f, -cos(yaw));
    } else if (e.reg && e.reg->has<CameraComponent>(e.id)) {
      f = e.reg->get<CameraComponent>(e.id).front;
    }
    f.y = 0.0f;
    if (glm::length(f) < 1e-4f)
      f = glm::vec3(0.0f, 0.0f, -1.0f);
    else
      f = glm::normalize(f);
    return glm::normalize(glm::cross(f, glm::vec3(0.0f, 1.0f, 0.0f)));
  };

  entityType["get_scale"] = [](const EntityProxy &e) -> glm::vec3 {
    if (e.reg && e.reg->has<TransformComponent>(e.id))
      return e.reg->get<TransformComponent>(e.id).scale;
    return glm::vec3(1.0f);
  };

  entityType["set_scale"] = [](const EntityProxy &e, float x, float y,
                               float z) {
    if (e.reg && e.reg->has<TransformComponent>(e.id))
      e.reg->get<TransformComponent>(e.id).scale = {x, y, z};
  };

  entityType["get_name"] = [](const EntityProxy &e) -> std::string {
    if (e.reg && e.reg->has<NameComponent>(e.id))
      return e.reg->get<NameComponent>(e.id).name;
    return "";
  };

  entityType["set_name"] = [](const EntityProxy &e, const std::string &name) {
    if (e.reg && e.reg->has<NameComponent>(e.id))
      e.reg->get<NameComponent>(e.id).name = name;
  };

  entityType["apply_impulse"] = [](const EntityProxy &e, float x, float y,
                                   float z) {
    if (e.reg && e.reg->has<RigidbodyComponent>(e.id)) {
      e.reg->get<RigidbodyComponent>(e.id).pendingImpulse += glm::vec3(x, y, z);
    }
  };

  entityType["set_velocity"] = [](const EntityProxy &e, float x, float y,
                                  float z) {
    if (e.reg && e.reg->has<RigidbodyComponent>(e.id)) {
      auto &rb = e.reg->get<RigidbodyComponent>(e.id);
      rb.pendingLinearVelocity = glm::vec3(x, y, z);
      rb.setLinearVelocity = true;
    }
  };

  entityType["get_velocity"] = [](const EntityProxy &e) -> glm::vec3 {
    if (e.reg && e.reg->has<RigidbodyComponent>(e.id)) {
      return e.reg->get<RigidbodyComponent>(e.id).linearVelocity;
    }
    return glm::vec3(0.0f);
  };

  // entity:set_material{ color={r,g,b}, roughness=, metallic=, emissive= }
  // Writes a MaterialOverrideComponent, which the renderer applies on top of
  // whatever the mesh's own material says.
  entityType["set_material"] = [](const EntityProxy &e, sol::table opts) {
    if (!e.reg || !e.reg->valid(e.id))
      return;
    if (!e.reg->has<MaterialOverrideComponent>(e.id))
      e.reg->emplace<MaterialOverrideComponent>(e.id);
    auto &ov = e.reg->get<MaterialOverrideComponent>(e.id);
    ov.enabled = opts.get_or("enabled", true);

    sol::optional<sol::table> color = opts["color"];
    if (color && color->size() >= 3)
      ov.material.baseColor =
          glm::vec4((*color)[1].get_or(1.0f), (*color)[2].get_or(1.0f),
                    (*color)[3].get_or(1.0f), opts.get_or("alpha", 1.0f));
    ov.material.roughness = opts.get_or("roughness", ov.material.roughness);
    ov.material.metallic = opts.get_or("metallic", ov.material.metallic);
    ov.material.ao = opts.get_or("ao", ov.material.ao);

    sol::optional<sol::table> emissive = opts["emissive"];
    if (emissive && emissive->size() >= 3)
      ov.material.emissiveColor =
          glm::vec3((*emissive)[1].get_or(0.0f), (*emissive)[2].get_or(0.0f),
                    (*emissive)[3].get_or(0.0f));
    ov.material.emissiveStrength =
        opts.get_or("emissiveStrength", ov.material.emissiveStrength);
  };

  entityType["set_asset"] = [](const EntityProxy &e, const std::string &assetId) {
    if (!e.reg || !e.reg->valid(e.id))
      return;
    if (!e.reg->has<MeshComponent>(e.id))
      e.reg->emplace<MeshComponent>(e.id);
    // Clear the cached handles: they point at the OLD asset's record, and
    // the render system prefers a valid handle over re-resolving assetId.
    MeshComponent &mc = e.reg->get<MeshComponent>(e.id);
    mc.assetId = assetId;
    mc.objHandle = {};
    mc.gltfHandle = {};
    mc.ufbxHandle = {};
  };

  // ── World / Spawner table ──────────────────────────────────────────
  auto worldTable = lua.create_named_table("world");

  worldTable["spawn_rock"] = [&registry, scene](float x, float y, float z,
                                                float scale) -> EntityProxy {
    uint32_t id = 0;
    if (scene) {
      id = scene->spawnPrimitive("sphere");
    } else {
      id = registry.create();
      registry.emplace<TransformComponent>(id);
    }
    if (id != 0) {
      if (registry.has<NameComponent>(id)) {
        registry.get<NameComponent>(id).name = "LuaRock";
      } else {
        registry.emplace<NameComponent>(id, NameComponent("LuaRock"));
      }
      if (registry.has<TransformComponent>(id)) {
        auto &tr = registry.get<TransformComponent>(id);
        tr.position = glm::vec3(x, y, z);
        tr.scale = glm::vec3(scale);
      }
      if (!registry.has<RigidbodyComponent>(id)) {
        auto &rb = registry.emplace<RigidbodyComponent>(id);
        rb.type = RigidbodyComponent::Type::Dynamic;
        rb.mass = 5.0f;
      }
      if (!registry.has<ColliderComponent>(id)) {
        auto &col = registry.emplace<ColliderComponent>(id);
        col.shape = ColliderComponent::Shape::Sphere;
        col.dimensions = glm::vec3(scale * 0.5f);
      }
    }
    return EntityProxy{id, &registry};
  };

  worldTable["spawn_primitive"] = [&registry, scene](const std::string &typeStr,
                                                      float x, float y, float z,
                                                      float scale) -> EntityProxy {
    uint32_t id = 0;
    if (scene) {
      id = scene->spawnPrimitive(typeStr);
    } else {
      id = registry.create();
      registry.emplace<TransformComponent>(id);
    }
    if (id != 0) {
      if (registry.has<TransformComponent>(id)) {
        auto &tr = registry.get<TransformComponent>(id);
        tr.position = glm::vec3(x, y, z);
        tr.scale = glm::vec3(scale);
      }
      if (!registry.has<RigidbodyComponent>(id)) {
        auto &rb = registry.emplace<RigidbodyComponent>(id);
        rb.type = RigidbodyComponent::Type::Dynamic;
        rb.mass = 5.0f;
      }
      if (!registry.has<ColliderComponent>(id)) {
        auto &col = registry.emplace<ColliderComponent>(id);
        col.shape = (typeStr == "sphere") ? ColliderComponent::Shape::Sphere
                                          : ColliderComponent::Shape::Box;
        col.dimensions = glm::vec3(scale * 0.5f);
      }
    }
    return EntityProxy{id, &registry};
  };

  // world.spawn(assetId, { pos={x,y,z}, rot={x,y,z}, scale=n|{x,y,z},
  //                        name="", collider="none"|"box"|"sphere"|"capsule",
  //                        dynamic=false, mass=1 })
  // The general spawner: unlike spawn_primitive/spawn_rock it takes any asset
  // id, including the "gen://" ids the generator pipeline produces.
  worldTable["spawn"] = [&registry](const std::string &assetId,
                                    sol::optional<sol::table> optsOpt) -> EntityProxy {
    if (assetId.empty())
      return EntityProxy{0, &registry};
    const sol::table opts = optsOpt ? *optsOpt : sol::table{};

    const EntityId id = registry.create();
    auto &tr = registry.emplace<TransformComponent>(id);
    if (opts.valid()) {
      sol::optional<sol::table> pos = opts["pos"];
      if (pos && pos->size() >= 3)
        tr.position = glm::vec3((*pos)[1].get_or(0.0f), (*pos)[2].get_or(0.0f),
                                (*pos)[3].get_or(0.0f));
      sol::optional<sol::table> rot = opts["rot"];
      if (rot && rot->size() >= 3)
        tr.rotation = glm::vec3((*rot)[1].get_or(0.0f), (*rot)[2].get_or(0.0f),
                                (*rot)[3].get_or(0.0f));
      // scale accepts a number or a {x,y,z} triple.
      sol::object scale = opts["scale"];
      if (scale.valid()) {
        if (scale.get_type() == sol::type::number)
          tr.scale = glm::vec3(scale.as<float>());
        else if (scale.get_type() == sol::type::table) {
          const sol::table s = scale.as<sol::table>();
          if (s.size() >= 3)
            tr.scale = glm::vec3(s[1].get_or(1.0f), s[2].get_or(1.0f),
                                 s[3].get_or(1.0f));
        }
      }
    }

    registry.emplace<MeshComponent>(id).assetId = assetId;
    registry.emplace<NameComponent>(
        id, NameComponent(opts.valid() ? opts.get_or("name", assetId) : assetId));

    const std::string collider =
        opts.valid() ? opts.get_or("collider", std::string("none")) : "none";
    if (collider != "none") {
      auto &col = registry.emplace<ColliderComponent>(id);
      if (collider == "sphere")
        col.shape = ColliderComponent::Shape::Sphere;
      else if (collider == "capsule")
        col.shape = ColliderComponent::Shape::Capsule;
      else
        col.shape = ColliderComponent::Shape::Box;
      col.dimensions = tr.scale * 0.5f;

      auto &rb = registry.emplace<RigidbodyComponent>(id);
      const bool dynamic = opts.valid() ? opts.get_or("dynamic", false) : false;
      rb.type = dynamic ? RigidbodyComponent::Type::Dynamic
                        : RigidbodyComponent::Type::Static;
      rb.mass = opts.valid() ? opts.get_or("mass", 1.0f) : 1.0f;
    }
    return EntityProxy{id, &registry};
  };

  worldTable["find_entity"] = [&registry](const std::string &name) -> EntityProxy {
    for (EntityId e : registry.view<NameComponent>()) {
      if (registry.get<NameComponent>(e).name == name) {
        return EntityProxy{e, &registry};
      }
    }
    return EntityProxy{0, &registry};
  };

  worldTable["destroy"] = [&registry](const EntityProxy &e) {
    if (e.id != 0 && registry.valid(e.id)) {
      registry.destroy(e.id);
    }
  };

  // ── Assets table (procedural generation) ───────────────────────────
  // The script-side face of the generator pipeline. Present even without an
  // AssetLibrary so a script can probe for it; every call reports a clear
  // error instead of silently doing nothing.
  auto assetsTable = lua.create_named_table("assets");

  // sol::as_table is load-bearing on every one of these. Returning a bare
  // std::vector makes sol push a CONTAINER USERDATA, not a Lua table: it
  // indexes and concatenates like a table (so scripts appear to work), but it
  // is not one, and anything converting it to JSON -- the command port --
  // sees an opaque userdata and yields null.
  assetsTable["generators"] = []() {
    return sol::as_table(gen::GeneratorRegistry::instance().names());
  };

  assetsTable["texture_generators"] = []() {
    return sol::as_table(gen::textureGeneratorNames());
  };

  // assets.schema("tree.v1") -> table of {type, default, minimum, maximum,
  // description} per parameter. The same document the engine validates
  // against, so a script (or an AI reading it) can never be told something
  // the validator disagrees with.
  assetsTable["schema"] = [&lua](const std::string &name) -> sol::object {
    if (const gen::GeneratorInfo *info =
            gen::GeneratorRegistry::instance().find(name))
      return scriptjson::toLua(lua, info->schema);
    if (const nlohmann::json *tex = gen::textureGeneratorSchema(name))
      return scriptjson::toLua(lua, *tex);
    return sol::nil;
  };

  // assets.define{ id=, generator=, seed=, params={}, material={} }
  // -> assetId, warnings   (nil, error on failure)
  //
  // Defining the same id again regenerates it IN PLACE, so entities already
  // spawned with that asset change shape -- the same mechanism the recipe
  // file watcher uses.
  assetsTable["define"] = [assetLibrary,
                           &lua](sol::table spec) -> std::tuple<sol::object, sol::object> {
    if (!assetLibrary)
      return {sol::nil, sol::make_object(lua, std::string(
                            "no asset library available in this context"))};

    gen::AssetRecipe recipe;
    recipe.id = spec.get_or("id", std::string{});
    recipe.generator = spec.get_or("generator", std::string{});
    recipe.seed = spec.get_or("seed", 0u);
    if (recipe.generator.empty())
      return {sol::nil,
              sol::make_object(lua, std::string("assets.define needs a "
                                                "'generator' field"))};

    sol::object params = spec["params"];
    if (params.valid() && params.get_type() == sol::type::table)
      recipe.params = scriptjson::toJson(params);
    sol::object material = spec["material"];
    if (material.valid() && material.get_type() == sol::type::table)
      recipe.material = scriptjson::toJson(material);

    gen::ResolveResult r = assetLibrary->resolve(recipe);
    // Warnings are returned AND logged: a script may ignore the second return
    // value, and a clamped parameter that nobody ever sees is how an asset
    // quietly stops matching its recipe.
    for (const std::string &w : r.warnings)
      LOG_WARN("Script", "assets.define(" + recipe.generator + "): " + w);
    if (!r.ok())
      return {sol::nil, sol::make_object(lua, r.error)};

    sol::table warnings = lua.create_table(static_cast<int>(r.warnings.size()), 0);
    for (size_t i = 0; i < r.warnings.size(); ++i)
      warnings[i + 1] = r.warnings[i];
    return {sol::make_object(lua, r.assetId), warnings};
  };

  assetsTable["list"] = [assetLibrary]() {
    return sol::as_table(assetLibrary ? assetLibrary->recipeIds()
                                      : std::vector<std::string>{});
  };

  assetsTable["asset_id"] = [assetLibrary](const std::string &recipeId) -> std::string {
    return assetLibrary ? assetLibrary->assetIdOfRecipe(recipeId) : std::string{};
  };

  // assets.info(assetId) -> { triangles, vertices, submeshes, boundsMin,
  // boundsMax } or nil. Deterministic facts, for regression checks and for
  // answering "how big did that come out" without rendering it.
  assetsTable["info"] = [assetLibrary, &lua](const std::string &assetId) -> sol::object {
    if (!assetLibrary)
      return sol::nil;
    const gen::AssetLibrary::AssetInfo i = assetLibrary->info(assetId);
    if (!i.valid)
      return sol::nil;
    sol::table t = lua.create_table();
    t["triangles"] = i.triangles;
    t["vertices"] = i.vertices;
    t["submeshes"] = i.submeshes;
    t["boundsMin"] = lua.create_table_with(1, i.boundsMin.x, 2, i.boundsMin.y,
                                           3, i.boundsMin.z);
    t["boundsMax"] = lua.create_table_with(1, i.boundsMax.x, 2, i.boundsMax.y,
                                           3, i.boundsMax.z);
    return t;
  };

  assetsTable["reload"] = [assetLibrary]() {
    return sol::as_table(assetLibrary ? assetLibrary->pollHotReload()
                                      : std::vector<std::string>{});
  };

  // ── Scene table ────────────────────────────────────────────────────
  auto sceneTable = lua.create_named_table("scene");

  sceneTable["stats"] = [&registry, &lua]() -> sol::table {
    sol::table t = lua.create_table();
    size_t entities = 0, meshes = 0;
    for (EntityId e : registry.view<TransformComponent>()) {
      (void)e;
      ++entities;
    }
    for (EntityId e : registry.view<MeshComponent>()) {
      (void)e;
      ++meshes;
    }
    t["entities"] = entities;
    t["meshes"] = meshes;
    return t;
  };

  sceneTable["save"] = [scene](const std::string &path) -> bool {
    return scene ? scene->saveToFile(path) : false;
  };

  sceneTable["load"] = [scene](const std::string &path) -> bool {
    return scene ? scene->loadFromFile(path) : false;
  };

  // ── Input table ────────────────────────────────────────────────────
  auto inputTable = lua.create_named_table("input");

  inputTable["key_down"] = [](const std::string &name) -> bool {
    int key = keyNameToGLFW(name);
    return key != GLFW_KEY_UNKNOWN && Keyboard::key(key);
  };
  inputTable["key_pressed"] = [](const std::string &name) -> bool {
    int key = keyNameToGLFW(name);
    return key != GLFW_KEY_UNKNOWN && Keyboard::keyWentDown(key);
  };
  inputTable["key_released"] = [](const std::string &name) -> bool {
    int key = keyNameToGLFW(name);
    return key != GLFW_KEY_UNKNOWN && Keyboard::keyWentUp(key);
  };

  inputTable["mouse_dx"] = []() -> float { return Mouse::getDX(); };
  inputTable["mouse_dy"] = []() -> float { return Mouse::getDY(); };
  inputTable["mouse_down"] = [](int button) -> bool {
    return Mouse::button(button);
  };
  inputTable["mouse_pressed"] = [](int button) -> bool {
    return Mouse::buttonWentDown(button);
  };
  inputTable["mouse_released"] = [](int button) -> bool {
    return Mouse::buttonWentUp(button);
  };

  // ── Logging table ──────────────────────────────────────────────────
  auto logTable = lua.create_named_table("log");

  logTable["info"] = [](const std::string &msg) { LOG_INFO("Script", msg); };
  logTable["warn"] = [](const std::string &msg) { LOG_WARN("Script", msg); };
  logTable["error"] = [](const std::string &msg) { LOG_ERROR("Script", msg); };

  // ── Physics table ──────────────────────────────────────────────────
  auto physicsTable = lua.create_named_table("physics");

  lua.new_usertype<PhysicsRaycastResult>(
      "RaycastResult", "hit", &PhysicsRaycastResult::hit, "distance",
      &PhysicsRaycastResult::distance, "position",
      &PhysicsRaycastResult::position, "normal", &PhysicsRaycastResult::normal,
      "entityId", &PhysicsRaycastResult::entityId);

  physicsTable["raycast"] = sol::overload(
      [physics](float ox, float oy, float oz, float dx, float dy, float dz,
                float maxDist) -> PhysicsRaycastResult {
        if (physics) {
          return physics->raycast(glm::vec3(ox, oy, oz), glm::vec3(dx, dy, dz),
                                  maxDist);
        }
        return PhysicsRaycastResult{};
      },
      [physics](float ox, float oy, float oz, float dx, float dy, float dz,
                float maxDist, uint32_t ignoreEntity)
          -> PhysicsRaycastResult {
        if (physics) {
          return physics->raycast(glm::vec3(ox, oy, oz), glm::vec3(dx, dy, dz),
                                  maxDist, ignoreEntity);
        }
        return PhysicsRaycastResult{};
      });
}
