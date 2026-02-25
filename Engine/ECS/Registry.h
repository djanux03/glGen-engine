#pragma once
#include "SparseSet.h"
#include <algorithm>
#include <memory>
#include <vector>

// ---------------------------------------------------------------------------
// Compile-time component ID — replaces std::type_index + unordered_map
// Each unique component type T gets a unique uint32_t at first use.
// ---------------------------------------------------------------------------
inline uint32_t nextComponentId() {
  static uint32_t counter = 0;
  return counter++;
}

template <typename T> uint32_t componentId() {
  static uint32_t id = nextComponentId();
  return id;
}

class Registry {
public:
  EntityId create() {
    if (!mFreeIds.empty()) {
      EntityId id = mFreeIds.back();
      mFreeIds.pop_back();
      return id;
    }
    return mNextId++;
  }

  void destroy(EntityId entity) {
    // Remove components from all sparse sets
    for (auto &pool : mComponentPools) {
      if (pool)
        pool->remove(entity);
    }
    mFreeIds.push_back(entity);
  }

  template <typename T, typename... Args>
  T &emplace(EntityId entity, Args &&...args) {
    return getPool<T>()->emplace(entity, T(std::forward<Args>(args)...));
  }

  template <typename T> T &get(EntityId entity) {
    return getPool<T>()->get(entity);
  }

  template <typename T> bool has(EntityId entity) {
    return getPool<T>()->has(entity);
  }

  template <typename T> void removeComponent(EntityId entity) {
    getPool<T>()->remove(entity);
  }

  // Single-component view: iterate all entities with component T
  template <typename T> std::vector<EntityId> &view() {
    return getPool<T>()->entities();
  }

  // Multi-component view: iterate entities that have BOTH A and B
  // Iterates the smaller pool and checks membership in the larger.
  template <typename A, typename B> std::vector<EntityId> view2() {
    auto *poolA = getPool<A>();
    auto *poolB = getPool<B>();

    auto &smallEntities = (poolA->entities().size() <= poolB->entities().size())
                              ? poolA->entities()
                              : poolB->entities();
    auto *other = (poolA->entities().size() <= poolB->entities().size())
                      ? static_cast<ISparseSet *>(poolB)
                      : static_cast<ISparseSet *>(poolA);

    std::vector<EntityId> result;
    for (auto e : smallEntities) {
      if (other->has(e))
        result.push_back(e);
    }
    return result; // NRVO — no copy
  }

  template <typename... Ts> std::vector<EntityId> viewAll() {
    auto pickSmallest = [this]() -> const std::vector<EntityId> * {
      const std::vector<EntityId> *smallest = nullptr;
      (([&]() {
         auto &entities = getPool<Ts>()->entities();
         if (!smallest || entities.size() < smallest->size())
           smallest = &entities;
       }()),
       ...);
      return smallest;
    };

    const std::vector<EntityId> *seed = pickSmallest();
    std::vector<EntityId> result;
    if (!seed)
      return result;

    for (EntityId e : *seed) {
      bool hasAll = true;
      ((hasAll = hasAll && getPool<Ts>()->has(e)), ...);
      if (hasAll)
        result.push_back(e);
    }
    return result; // NRVO — no copy
  }

  template <typename... Ts, typename Pred>
  std::vector<EntityId> viewWhere(Pred pred) {
    auto entities = viewAll<Ts...>();
    std::vector<EntityId> result;
    for (EntityId e : entities) {
      if (pred(e))
        result.push_back(e);
    }
    return result; // NRVO — no copy
  }

  // Direct access to component array for cache-friendly iteration
  template <typename T> std::vector<T> &componentView() {
    return getPool<T>()->components();
  }

private:
  template <typename T> SparseSet<T> *getPool() {
    uint32_t id = componentId<T>();
    if (id >= mComponentPools.size()) {
      mComponentPools.resize(id + 1);
    }
    if (!mComponentPools[id]) {
      mComponentPools[id] = std::make_unique<SparseSet<T>>();
    }
    return static_cast<SparseSet<T> *>(mComponentPools[id].get());
  }

  // Flat vector indexed by compile-time component ID (replaces unordered_map)
  std::vector<std::unique_ptr<ISparseSet>> mComponentPools;

  EntityId mNextId = 1;
  std::vector<EntityId> mFreeIds;
};
