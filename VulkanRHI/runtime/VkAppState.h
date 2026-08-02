#pragma once

// The Vulkan-side counterpart of the old OpenGL app's AppState: a state
// struct that ties together EngineCore's GL-free systems (Scene, Assets,
// Jolt physics, ScriptSystem) with the ported gameplay logic and the
// Vk*Subsystem set, without depending on anything GL-coupled. Phase 1 scope
// only -- FX systems (clouds/fire/fog/black hole/projectiles/fireflies) and
// the CPU TerrainSystem are intentionally not represented here yet.

#include "Assets/AssetManager.h"
#include "Scene/Scene.h"
#include "ECS/Systems/PhysicsSystem.h"
#include "Scripting/ScriptSystem.h"

#include "GameplayState.h"
#include "InputSettings.h"
#include "ProjectConfig.h"

#include "gameplay/VkPlayerControllerSystem.h"
#include "gameplay/VkPlayerInteractionSystem.h"
#include "gameplay/VkSpaceshipControlSystem.h"

#include "SubsystemManager.h"

struct GLFWwindow;

namespace vkrhi {
class VulkanRenderer;
class VulkanRenderSystem;
} // namespace vkrhi
class VkEditor;

struct VkAppState {
  // Window / timing
  GLFWwindow *window = nullptr;
  int scrW = 1280;
  int scrH = 720;
  float lastT = 0.0f;

  // Core systems (EngineCore, GL-free)
  AssetManager assets;
  Scene scene;
  PhysicsSystem physicsSystem;
  ScriptSystem scriptSystem;

  GameplayState gameplay;
  InputSettings input;
  ProjectConfig projectConfig;

  // Play state (mirrors the old app's AppState::PlayState; controls Lua
  // script execution and footstep-audio gating).
  enum class PlayState { Stopped, Playing, Paused };
  PlayState playState = PlayState::Playing;

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

  SubsystemManager subsystems;
};
