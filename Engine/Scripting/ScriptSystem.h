#pragma once
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "Logger.h"
#include "ScriptBindings.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class ScriptSystem {
public:
  // Extension point for bindings that EngineCore cannot see. render.* and
  // terrain.* need VulkanRenderer and VkTerrainSubsystem, which live above
  // this layer -- the Vulkan runtime registers them here instead of EngineCore
  // acquiring a dependency on its own consumer. Hooks run during initialize(),
  // in registration order, after the core bindings.
  using BindingHook = std::function<void(sol::state &)>;
  void addBindingHook(BindingHook hook) {
    mBindingHooks.push_back(std::move(hook));
  }

  // Initialize the Lua VM and register all API bindings.
  // Must be called once after the Registry is available.
  //
  // `io` and `os` are deliberately NOT opened. Scripts here are increasingly
  // machine-authored (see AI_ASSET_PIPELINE_PLAN.md), and those two libraries
  // are arbitrary file and process access -- os.execute alone makes the Lua
  // console a remote shell. Nothing in scripts/ used them.
  void initialize(Registry &registry, class PhysicsSystem *physics = nullptr,
                  class Scene *scene = nullptr,
                  gen::AssetLibrary *assetLibrary = nullptr,
                  ScriptGroundHeightFn groundHeight = nullptr) {
    mLua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                        sol::lib::table);
    registerScriptBindings(mLua, registry, physics, scene, assetLibrary,
                           std::move(groundHeight));
    for (auto &hook : mBindingHooks)
      hook(mLua);
    mInitialized = true;
    LOG_INFO("Script", "Lua scripting system initialized");
  }

  // Runs a chunk of Lua in the global environment. This is the console /
  // command-port entry point -- everything else in this class is per-entity
  // ScriptComponent execution, which cannot answer "run this one statement".
  // Returns false with `error` set; never throws.
  bool execString(const std::string &source, std::string &error) {
    if (!mInitialized) {
      error = "script system not initialized";
      return false;
    }
    try {
      sol::protected_function_result result = mLua.safe_script(
          source, sol::script_pass_on_error);
      if (!result.valid()) {
        const sol::error err = result;
        error = err.what();
        return false;
      }
      return true;
    } catch (const std::exception &e) {
      error = e.what();
      return false;
    }
  }

  // execString's counterpart for callers that need the VALUE back (the
  // command port, where "list the generators" has to return a list).
  //
  // `source` is tried as an EXPRESSION first ("return " + source) and only
  // then as a statement chunk -- the standard REPL trick, and what lets
  // `assets.generators()` and `x = 1` both work through one entry point.
  // Multiple return values come back as a JSON array.
  bool eval(const std::string &source, nlohmann::json &result,
            std::string &error) {
    if (!mInitialized) {
      error = "script system not initialized";
      return false;
    }
    try {
      sol::protected_function_result r =
          mLua.safe_script("return " + source, sol::script_pass_on_error);
      if (!r.valid()) {
        // Not an expression: run it as statements. Any error from THIS attempt
        // is the one worth reporting -- the expression attempt's error is
        // usually just "unexpected symbol near '='".
        r = mLua.safe_script(source, sol::script_pass_on_error);
        if (!r.valid()) {
          const sol::error err = r;
          error = err.what();
          return false;
        }
      }
      if (r.return_count() == 0) {
        result = nullptr;
      } else if (r.return_count() == 1) {
        result = scriptjson::toJson(r.get<sol::object>(0));
      } else {
        result = nlohmann::json::array();
        for (int i = 0; i < r.return_count(); ++i)
          result.push_back(scriptjson::toJson(r.get<sol::object>(i)));
      }
      return true;
    } catch (const std::exception &e) {
      error = e.what();
      return false;
    }
  }

  // Direct access for hooks that need to push state after initialize().
  sol::state &lua() { return mLua; }

  // Erase per-entity script environment when script component or entity is destroyed
  void cleanupEntity(EntityId entity) {
    mScriptEnvs.erase(entity);
  }

  // Run all scripts for entities with ScriptComponent.
  // Called once per frame from CoreAppLayer::update().
  void update(Registry &registry, float dt) {
    if (!mInitialized)
      return;

    // Erase script environments for entities that are no longer valid or no longer have ScriptComponent
    for (auto it = mScriptEnvs.begin(); it != mScriptEnvs.end();) {
      if (!registry.valid(it->first) || !registry.has<ScriptComponent>(it->first)) {
        it = mScriptEnvs.erase(it);
      } else {
        ++it;
      }
    }

    // Snapshot the view so that scripts calling self:destroy() during
    // on_update don't mutate the sparse set under iteration (swap-and-pop
    // would silently skip the entity moved into the destroyed slot).
    const auto &liveView = registry.view<ScriptComponent>();
    std::vector<EntityId> snapshot(liveView.begin(), liveView.end());
    for (auto entity : snapshot) {
      // The entity may have been destroyed by an earlier script this frame.
      if (!registry.valid(entity) || !registry.has<ScriptComponent>(entity))
        continue;

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
        if (!loadScript(entity, sc, registry)) {
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
  std::vector<BindingHook> mBindingHooks;

  // Per-entity Lua environments (sandboxes)
  std::unordered_map<EntityId, sol::environment> mScriptEnvs;

  bool loadScript(EntityId entity, ScriptComponent &sc, Registry &registry) {
    try {
      // Create a sandbox environment that inherits from globals
      sol::environment env(mLua, sol::create, mLua.globals());
      env["self"] = EntityProxy{entity, &registry};
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
