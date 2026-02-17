#pragma once
#include "Registry.h"
#include <cstdint>

class Entity {
public:
  EntityId id = 0;
  Registry *registry = nullptr;

  Entity() = default;
  Entity(EntityId id, Registry *registry) : id(id), registry(registry) {}

  bool isValid() const { return registry != nullptr; }

  template <typename T, typename... Args> T &addComponent(Args &&...args) {
    return registry->emplace<T>(id, std::forward<Args>(args)...);
  }

  template <typename T> T &getComponent() { return registry->get<T>(id); }

  // In a real engine, add hasComponent<T>(), removeComponent<T>()
};
