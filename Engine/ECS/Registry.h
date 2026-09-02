#pragma once
#include "SparseSet.h"
#include <algorithm>
#include <functional>
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

class Registry;

// ---------------------------------------------------------------------------
// CommandBuffer for queuing structural mutations (create, destroy, emplace)
// safely during system execution or iterations.
// ---------------------------------------------------------------------------
class CommandBuffer {
public:
  void create(std::function<void(EntityId)> callback = nullptr);
  void destroy(EntityId entity);

  template <typename T, typename... Args>
  void emplace(EntityId entity, Args &&...args);

  template <typename T> void removeComponent(EntityId entity);

  void flush(Registry &registry);
  void clear() { mCommands.clear(); }

private:
  std::vector<std::function<void(Registry &)>> mCommands;
};

class Registry {
public:
  // Index 0 is reserved and never allocated, so the id 0 is never a live
  // entity.
  //
  // This matters because the rest of the engine already treats 0 as "no
  // entity": Scene::spawnPrimitive / spawnFromFile / createEmptyEntity all
  // `return 0` to report failure, Scene's hierarchy code uses `parent != 0`
  // to mean "has a parent", and the editor uses 0 for "nothing selected".
  // Allocation used to start at index 0, so the first entity created in a
  // fresh scene got id 0 and every one of those checks misread it as
  // null -- it could not be selected, focused, deleted or parented, and a
  // successful spawn reported itself as a failure.
  //
  // Reserving one index is far cheaper than auditing that convention out of
  // every call site, and it makes the convention true rather than assumed.
  static constexpr uint32_t kFirstIndex = 1;

  EntityId create() {
    uint32_t rawIdx = 0;
    uint32_t gen = 0;
    if (!mFreeIndices.empty()) {
      rawIdx = mFreeIndices.back();
      mFreeIndices.pop_back();
      gen = mGenerations[rawIdx];
    } else {
      rawIdx = mNextIndex++;
      if (rawIdx >= mGenerations.size()) {
        mGenerations.resize(rawIdx + 1, 0);
      }
      gen = mGenerations[rawIdx];
    }
    return EntityTraits::make(rawIdx, gen);
  }

  bool valid(EntityId entity) const {
    if (entity == EntityTraits::NullEntity)
      return false;
    uint32_t idx = EntityTraits::index(entity);
    uint32_t gen = EntityTraits::generation(entity);
    // Index 0 is never handed out, but generation 0 would otherwise match
    // the zero-initialised slot and make the null id look valid.
    if (idx < kFirstIndex)
      return false;
    return idx < mGenerations.size() && mGenerations[idx] == gen;
  }

  void destroy(EntityId entity) {
    if (!valid(entity))
      return;

    // Remove components from all sparse sets
    for (auto &pool : mComponentPools) {
      if (pool)
        pool->remove(entity);
    }

    uint32_t idx = EntityTraits::index(entity);
    mGenerations[idx] = (mGenerations[idx] + 1) & 0xFFF;
    mFreeIndices.push_back(idx);
  }

  void clear() {
    for (auto &pool : mComponentPools) {
      if (pool)
        pool->clear();
    }
    // Bump every generation rather than zeroing -- zeroing would let
    // stale handles from before the clear alias newly created entities
    // (their embedded generation-0 would match the reset slot).
    for (auto &g : mGenerations)
      g = (g + 1) & 0xFFF;
    mFreeIndices.clear();
    // Push all previously used indices back onto the free list so they
    // can be reused with their bumped generation.
    for (uint32_t i = kFirstIndex; i < mNextIndex; ++i)
      mFreeIndices.push_back(i);
    // Don't reset mNextIndex -- the slots still exist in mGenerations.
  }

  template <typename T, typename... Args>
  T &emplace(EntityId entity, Args &&...args) {
    return getPool<T>()->emplace(entity, T(std::forward<Args>(args)...));
  }

  template <typename T> T &get(EntityId entity) {
    return getPool<T>()->get(entity);
  }

  template <typename T> const T &get(EntityId entity) const {
    return const_cast<Registry *>(this)->getPool<T>()->get(entity);
  }

  template <typename T> bool has(EntityId entity) const {
    if (!valid(entity))
      return false;
    return const_cast<Registry *>(this)->getPool<T>()->has(entity);
  }

  template <typename T> void removeComponent(EntityId entity) {
    if (!valid(entity))
      return;
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
      // Guard against the packed array shrinking during iteration
      // (swap-and-pop on entity destroy). Without this, a cached end()
      // from a range-for overshoots the now-smaller array.
      if (seedEntities && currentIndex >= seedEntities->size())
        return false;
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
        bool hasAll = registry->valid(e);
        if (hasAll) {
          ((hasAll = hasAll && registry->has<Ts>(e)), ...);
        }
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
      // Guard against the packed array shrinking during iteration
      // (swap-and-pop on entity destroy). Without this, a cached end()
      // from a range-for overshoots the now-smaller array.
      if (seedEntities && currentIndex >= seedEntities->size())
        return false;
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
        bool hasAll = registry->valid(e);
        if (hasAll) {
          ((hasAll = hasAll && registry->has<Ts>(e)), ...);
        }
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

  // Flat vector indexed by compile-time component ID
  std::vector<std::unique_ptr<ISparseSet>> mComponentPools;

  uint32_t mNextIndex = kFirstIndex;
  std::vector<uint32_t> mGenerations;
  std::vector<uint32_t> mFreeIndices;
};

// ---------------------------------------------------------------------------
// CommandBuffer out-of-line implementations
// ---------------------------------------------------------------------------
inline void CommandBuffer::create(std::function<void(EntityId)> callback) {
  mCommands.push_back([callback](Registry &reg) {
    EntityId id = reg.create();
    if (callback)
      callback(id);
  });
}

inline void CommandBuffer::destroy(EntityId entity) {
  mCommands.push_back([entity](Registry &reg) { reg.destroy(entity); });
}

template <typename T, typename... Args>
inline void CommandBuffer::emplace(EntityId entity, Args &&...args) {
  T comp(std::forward<Args>(args)...);
  mCommands.push_back(
      [entity, component = std::move(comp)](Registry &reg) mutable {
        reg.emplace<T>(entity, std::move(component));
      });
}

template <typename T> inline void CommandBuffer::removeComponent(EntityId entity) {
  mCommands.push_back(
      [entity](Registry &reg) { reg.removeComponent<T>(entity); });
}

inline void CommandBuffer::flush(Registry &registry) {
  for (auto &cmd : mCommands) {
    cmd(registry);
  }
  mCommands.clear();
}
