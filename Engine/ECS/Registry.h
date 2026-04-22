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

  template <typename... Ts> class ViewAllIterator {
  public:
    ViewAllIterator(Registry *reg, const std::vector<EntityId> *seed, size_t index)
        : registry(reg), seedEntities(seed), currentIndex(index) {
      advanceToValid();
    }

    bool operator!=(const ViewAllIterator &other) const {
      return currentIndex != other.currentIndex;
    }

    ViewAllIterator &operator++() {
      ++currentIndex;
      advanceToValid();
      return *this;
    }

    EntityId operator*() const { return (*seedEntities)[currentIndex]; }

  private:
    Registry *registry;
    const std::vector<EntityId> *seedEntities;
    size_t currentIndex;

    void advanceToValid() {
      if (!seedEntities)
        return;
      while (currentIndex < seedEntities->size()) {
        EntityId e = (*seedEntities)[currentIndex];
        bool hasAll = true;
        ((hasAll = hasAll && registry->has<Ts>(e)), ...);
        if (hasAll)
          break;
        ++currentIndex;
      }
    }
  };

  template <typename... Ts> class ViewAllProxy {
  public:
    ViewAllProxy(Registry *reg, const std::vector<EntityId> *seed)
        : registry(reg), seedEntities(seed) {}

    ViewAllIterator<Ts...> begin() const {
      return ViewAllIterator<Ts...>(registry, seedEntities, 0);
    }

    ViewAllIterator<Ts...> end() const {
      return ViewAllIterator<Ts...>(
          registry, seedEntities, seedEntities ? seedEntities->size() : 0);
    }

  private:
    Registry *registry;
    const std::vector<EntityId> *seedEntities;
  };

  template <typename Pred, typename... Ts> class ViewWhereIterator {
  public:
    ViewWhereIterator(Registry *reg, const std::vector<EntityId> *seed,
                      size_t index, Pred p)
        : registry(reg), seedEntities(seed), currentIndex(index), pred(p) {
      advanceToValid();
    }

    bool operator!=(const ViewWhereIterator &other) const {
      return currentIndex != other.currentIndex;
    }

    ViewWhereIterator &operator++() {
      ++currentIndex;
      advanceToValid();
      return *this;
    }

    EntityId operator*() const { return (*seedEntities)[currentIndex]; }

  private:
    Registry *registry;
    const std::vector<EntityId> *seedEntities;
    size_t currentIndex;
    Pred pred;

    void advanceToValid() {
      if (!seedEntities)
        return;
      while (currentIndex < seedEntities->size()) {
        EntityId e = (*seedEntities)[currentIndex];
        bool hasAll = true;
        ((hasAll = hasAll && registry->has<Ts>(e)), ...);
        if (hasAll && pred(e))
          break;
        ++currentIndex;
      }
    }
  };

  template <typename Pred, typename... Ts> class ViewWhereProxy {
  public:
    ViewWhereProxy(Registry *reg, const std::vector<EntityId> *seed, Pred p)
        : registry(reg), seedEntities(seed), pred(p) {}

    ViewWhereIterator<Pred, Ts...> begin() const {
      return ViewWhereIterator<Pred, Ts...>(registry, seedEntities, 0, pred);
    }

    ViewWhereIterator<Pred, Ts...> end() const {
      return ViewWhereIterator<Pred, Ts...>(
          registry, seedEntities, seedEntities ? seedEntities->size() : 0,
          pred);
    }

  private:
    Registry *registry;
    const std::vector<EntityId> *seedEntities;
    Pred pred;
  };

  // Multi-component view: iterate entities that have BOTH A and B
  template <typename A, typename B> ViewAllProxy<A, B> view2() {
    return viewAll<A, B>();
  }

  template <typename... Ts> ViewAllProxy<Ts...> viewAll() {
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

    return ViewAllProxy<Ts...>(this, pickSmallest());
  }

  template <typename... Ts, typename Pred>
  ViewWhereProxy<Pred, Ts...> viewWhere(Pred pred) {
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

    return ViewWhereProxy<Pred, Ts...>(this, pickSmallest(), pred);
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
