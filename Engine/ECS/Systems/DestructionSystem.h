#pragma once

#include "Scene/Scene.h"
#include <glm/glm.hpp>
#include <vector>

class AssetManager;
class PhysicsSystem;
class TerrainSystem;

class DestructionSystem {
public:
  static std::vector<Scene::EntityId>
  fractureEntity(Scene &scene, AssetManager &assets, Scene::EntityId entity,
                 const glm::vec3 &hitPosition,
                 const glm::vec3 &impulseDirection,
                 PhysicsSystem *physics = nullptr);

  static void preparePendingDestroy(Scene &scene, AssetManager &assets,
                                    PhysicsSystem *physics = nullptr);
  static void updateRuntime(Scene &scene, float dt,
                            PhysicsSystem *physics = nullptr,
                            const TerrainSystem *terrain = nullptr);
  static void updateEditorPreview(Scene &scene, float dt,
                                  const TerrainSystem *terrain = nullptr);
};
