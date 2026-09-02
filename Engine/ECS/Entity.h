#pragma once
#include "Registry.h"
#include <cstdint>

class Entity {
public:
  EntityId id = EntityTraits::NullEntity;
  Registry *registry = nullptr;

  Entity() = default;
  Entity(EntityId id, Registry *registry) : id(id), registry(registry) {}

  bool isValid() const { return registry != nullptr && registry->valid(id); }
  explicit operator bool() const { return isValid(); }

  template <typename T, typename... Args> T &addComponent(Args &&...args) {
    return registry->emplace<T>(id, std::forward<Args>(args)...);
  }

  template <typename T> T &getComponent() { return registry->get<T>(id); }
  template <typename T> const T &getComponent() const {
    return registry->get<T>(id);
  }

  template <typename T> bool hasComponent() const {
    return registry != nullptr && registry->has<T>(id);
  }

  template <typename T> void removeComponent() {
    if (registry)
      registry->removeComponent<T>(id);
  }

  void destroy() {
    if (isValid()) {
      registry->destroy(id);
      id = EntityTraits::NullEntity;
    }
  }

  bool operator==(const Entity &other) const {
    return id == other.id && registry == other.registry;
  }
  bool operator!=(const Entity &other) const { return !(*this == other); }
  operator EntityId() const { return id; }
};
