#pragma once

// The Vulkan-side counterpart of the old OpenGL app's AppState: a state
// struct that ties together EngineCore's GL-free systems (Scene, Assets,
// Jolt physics, ScriptSystem, chunked terrain streaming) with the ported
// gameplay logic and the Vk*Subsystem set, without depending on anything
// GL-coupled. FX systems (clouds/fire/fog/black hole/projectiles/fireflies)
// are still not represented here.

#include "Assets/AssetManager.h"
#include "Generators/AssetLibrary.h"
#include "Scene/Scene.h"
#include "ECS/Systems/PhysicsSystem.h"
#include "Scripting/ScriptSystem.h"
#include "TerrainChunkManager.h"

#include "GameplayState.h"
#include "InputSettings.h"
#include "ProjectConfig.h"

#include "gameplay/VkPlayerControllerSystem.h"
#include "gameplay/VkPlayerInteractionSystem.h"
#include "gameplay/VkSpaceshipControlSystem.h"

#include "SubsystemManager.h"

#include <string>

struct GLFWwindow;

namespace vkrhi {
class VulkanRenderer;
class VulkanRenderSystem;
} // namespace vkrhi
class VkEditor;
class VkTerrainSubsystem;

struct VkAppState {
  // Window / timing
  GLFWwindow *window = nullptr;
  int scrW = 1280;
  int scrH = 720;
  float lastT = 0.0f;

  // Set once in main() from GLGEN_VK_ASSET_DIR. Subsystems that load files
  // by path (VkTerrainSubsystem's R2 terrain material textures) resolve
  // their defaults relative to this rather than hardcoding an absolute path.
  std::string assetDir;

  // Core systems (EngineCore, GL-free)
  AssetManager assets;
  // Procedurally generated assets (AI_ASSET_PIPELINE_PLAN.md Phase 1): owns
  // the recipes under <assetDir>/recipes and registers their meshes with
  // `assets` above. Editing a recipe file regenerates and hot-swaps it.
  gen::AssetLibrary assetLibrary;
  Scene scene;
  PhysicsSystem physicsSystem;
  ScriptSystem scriptSystem;
  TerrainChunkManager terrain;

  GameplayState gameplay;
  InputSettings input;
  ProjectConfig projectConfig;

  // Play state (mirrors the old app's AppState::PlayState; controls Lua
  // script execution and footstep-audio gating).
  enum class PlayState { Stopped, Playing, Paused };
  PlayState playState = PlayState::Playing;

  // Deferred play-mode requests (game.play() / game.stop() from Lua, and so
  // the command port by extension). Entering play mode snapshots the scene
  // and touches the editor's state, which is only safe at the point in the
  // frame where main.cpp already builds a VkEditor::Context -- so a script
  // raises a flag here and main.cpp acts on it there, rather than the script
  // reaching into the editor mid-update.
  bool requestPlayMode = false;
  bool requestStopMode = false;

  // Ported gameplay systems (VulkanRHI/runtime/gameplay/) -- logic-only,
  // adapted from Runtime/Gameplay/* to run against VkAppState instead of the
  // GL app's AppState.
  VkPlayerControllerSystem playerController;
  VkPlayerInteractionSystem playerInteraction;
  VkSpaceshipControlSystem spaceshipControl;

  // Non-owning: these live in glGenVk's main() for the app's lifetime; the
  // subsystems and gameplay systems above only reference them.
  vkrhi::VulkanRenderer *renderer = nullptr;
  vkrhi::VulkanRenderSystem *renderSystem = nullptr;
  VkEditor *vkEditor = nullptr;
  // Lets gameplay systems query the procedural terrain surface directly
  // (VkTerrainSubsystem::heightAt()) instead of only the Jolt physics
  // raycast -- the physics heightfield only exists within
  // collisionChunkRadius of the camera and streams in asynchronously, so a
  // ground candidate that doesn't depend on it is what keeps the player
  // from falling through on spawn or right after a terrain regenerate().
  VkTerrainSubsystem *terrainSubsystem = nullptr;

  SubsystemManager subsystems;
};
