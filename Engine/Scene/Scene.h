#pragma once

#include "ECS/Registry.h"
#include <memory>
#include <string>
#include <unordered_map>

// Forward declarations
class OBJModel;

class Scene {
public:
  using EntityId = std::uint32_t;

  Scene();
  ~Scene();

  // ---- ECS ----
  Registry &registry() { return mRegistry; }
  const Registry &registry() const { return mRegistry; }

  // ---- Asset loading (cached) ----
  // Loads an OBJ once per unique path; returns a stable pointer owned by the
  // Scene.
  OBJModel *getOrLoadOBJ(const std::string &objPath);

private:
  // Asset cache
  std::unordered_map<std::string, std::unique_ptr<OBJModel>> mOBJCache;

  // ECS Registry
  Registry mRegistry;
};
