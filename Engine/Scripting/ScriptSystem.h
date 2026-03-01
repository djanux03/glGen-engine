#pragma once
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "Logger.h"
#include "ScriptBindings.h"

#include <string>
#include <unordered_map>

class ScriptSystem {
public:
  // Initialize the Lua VM and register all API bindings.
  // Must be called once after the Registry is available.
  void initialize(Registry &registry, class PhysicsSystem *physics = nullptr) {
    mLua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                        sol::lib::table, sol::lib::io, sol::lib::os);
    registerScriptBindings(mLua, registry, physics);
    mInitialized = true;
    LOG_INFO("Script", "Lua scripting system initialized");
  }

  // Run all scripts for entities with ScriptComponent.
  // Called once per frame from CoreAppLayer::update().
  void update(Registry &registry, float dt) {
    if (!mInitialized)
      return;

    for (auto entity : registry.view<ScriptComponent>()) {
      if (registry.has<LifecycleComponent>(entity)) {
        auto s = registry.get<LifecycleComponent>(entity).state;
        if (s != EntityLifecycleState::Alive)
          continue;
      }

      auto &sc = registry.get<ScriptComponent>(entity);
      if (sc.scriptPath.empty())
        continue;

      // First frame: load the script file and call on_spawn
      if (!sc.initialized) {
        if (!loadScript(entity, sc)) {
          sc.scriptPath.clear(); // Prevent retrying a broken script
          continue;
        }
        sc.initialized = true;

        // Call on_spawn if defined
        callScriptFunction(entity, sc, "on_spawn", registry);
      }

      // Every frame: call on_update(entity, dt)
      callScriptFunction(entity, sc, "on_update", registry, dt);
    }
  }

  void shutdown() {
    mScriptEnvs.clear();
    mInitialized = false;
    LOG_INFO("Script", "Lua scripting system shut down");
  }

  bool isInitialized() const { return mInitialized; }

private:
  sol::state mLua;
  bool mInitialized = false;

  // Per-entity Lua environments (sandboxes)
  std::unordered_map<EntityId, sol::environment> mScriptEnvs;

  bool loadScript(EntityId entity, ScriptComponent &sc) {
    try {
      // Create a sandbox environment that inherits from globals
      sol::environment env(mLua, sol::create, mLua.globals());
      mScriptEnvs[entity] = env;

      // Load and execute the script file in this environment
      auto result = mLua.script_file(sc.scriptPath, env);
      if (!result.valid()) {
        sol::error err = result;
        LOG_ERROR("Script", "Failed to load script '" + sc.scriptPath +
                                "': " + err.what());
        return false;
      }

      LOG_INFO("Script", "Loaded script: " + sc.scriptPath + " for entity " +
                             std::to_string(entity));
      return true;
    } catch (const std::exception &e) {
      LOG_ERROR("Script", "Exception loading script '" + sc.scriptPath +
                              "': " + e.what());
      return false;
    }
  }

  // Call a named function in the entity's script environment
  template <typename... Args>
  void callScriptFunction(EntityId entity, const ScriptComponent &sc,
                          const std::string &funcName, Registry &registry,
                          Args &&...args) {
    auto it = mScriptEnvs.find(entity);
    if (it == mScriptEnvs.end())
      return;

    sol::environment &env = it->second;
    sol::object fnObj = env[funcName];
    if (!fnObj.valid() || fnObj.get_type() != sol::type::function)
      return;

    sol::protected_function fn = fnObj;

    // Create the EntityProxy that scripts receive as first argument
    EntityProxy proxy{entity, &registry};

    try {
      sol::protected_function_result result =
          fn(proxy, std::forward<Args>(args)...);
      if (!result.valid()) {
        sol::error err = result;
        LOG_ERROR("Script", "Error in " + sc.scriptPath + "::" + funcName +
                                "(): " + err.what());
      }
    } catch (const std::exception &e) {
      LOG_ERROR("Script", "Exception in " + sc.scriptPath + "::" + funcName +
                              "(): " + e.what());
    }
  }
};
