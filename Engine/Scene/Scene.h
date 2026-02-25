#pragma once

#include "ECS/Registry.h"
#include <memory>
#include <string>

// Forward declarations
class OBJModel;
class FBXModel;
class AssetManager;

class Scene {
public:
  using EntityId = std::uint32_t;

  Scene();
  ~Scene();

  void setAssetManager(AssetManager *assets) { mAssets = assets; }

  // ---- ECS ----
  Registry &registry() { return mRegistry; }
  const Registry &registry() const { return mRegistry; }

  // ---- Asset loading (cached) ----
  OBJModel *getOrLoadOBJ(const std::string &objPath);
  FBXModel *getOrLoadFBX(const std::string &path);

  // ---- Entity helpers ----
  // Spawns an entity from supported mesh files (.obj, .fbx, .gltf, .glb):
  // creates Transform + Mesh + Name. Returns 0 on failure.
  EntityId spawnFromFile(const std::string &path);
  EntityId spawnPrimitive(const std::string &primitiveName);
  EntityId createEmptyEntity(const std::string &name = "Empty");
  bool saveToFile(const std::string &path) const;
  bool loadFromFile(const std::string &path);
  std::string serializeToString() const;
  bool loadFromString(const std::string &jsonText);
  void clear();

  // Lifecycle state controls
  void disableEntity(EntityId id);
  void enableEntity(EntityId id);
  void flushPendingDestroy();

  // Hierarchy
  bool setParent(EntityId child, EntityId parent);
  void clearParent(EntityId child);

  // Marks entity (and descendants) for deterministic destruction.
  void deleteEntity(EntityId id);

private:
  AssetManager *mAssets = nullptr;
  Registry mRegistry;
};
