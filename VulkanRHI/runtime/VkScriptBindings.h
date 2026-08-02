#pragma once
// VkScriptBindings.h — the render.* and terrain.* Lua tables.
//
// These live on the Vulkan side rather than in EngineCore's ScriptBindings.h
// because they need VulkanRenderer and VkTerrainSubsystem, which sit ABOVE
// EngineCore in the dependency graph. They are installed through
// ScriptSystem::addBindingHook() so EngineCore never has to know its own
// consumers exist.

#include "json.hpp"

#include <sol/forward.hpp>

struct VkAppState;

// Registers render.* and terrain.* against `state`. Call via
// ScriptSystem::addBindingHook() BEFORE ScriptSystem::initialize().
void registerVkScriptBindings(sol::state &lua, VkAppState &state);

// Render parameters in their canonical form: JSON. Both the Lua binding
// (which converts its table with ScriptJson) and the command port's
// render.setParams go through these, so the field mapping exists once. Any
// key absent from `params` keeps its current value.
void applyRenderParamsJson(VkAppState &state, const nlohmann::json &params);
nlohmann::json renderParamsToJson(const VkAppState &state);
