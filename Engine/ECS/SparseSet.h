#pragma once
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

using EntityId = std::uint32_t;

// Interface for type erasure (so Registry can store list of these)
class ISparseSet {
public:
  virtual ~ISparseSet() = default;
  virtual bool has(EntityId entity) const = 0;
  virtual void remove(EntityId entity) = 0;
};

template <typename T> class SparseSet : public ISparseSet {
public:
  // Add component data for an entity
  T &emplace(EntityId entity, T component) {
    if (has(entity)) {
      // Already exists; update it? For now just return existing
      return mComponents[mSparse[entity]];
    }

    if (entity >= mSparse.size()) {
      mSparse.resize(entity + 1, NullIndex);
    }

    mSparse[entity] = (EntityId)mPacked.size();
    mPacked.push_back(entity);
    mComponents.push_back(std::move(component));

    return mComponents.back();
  }

  void remove(EntityId entity) override {
    if (!has(entity))
      return;

    // Swap and pop
    EntityId denseIndex = mSparse[entity];
    EntityId lastIndex = (EntityId)mPacked.size() - 1;
    EntityId lastEntity = mPacked[lastIndex];

    // Move last component into hole
    mComponents[denseIndex] = std::move(mComponents[lastIndex]);
    mPacked[denseIndex] = lastEntity;

    // Update sparse map
    mSparse[lastEntity] = denseIndex;
    mSparse[entity] = NullIndex;

    mPacked.pop_back();
    mComponents.pop_back();
  }

  bool has(EntityId entity) const override {
    return entity < mSparse.size() && mSparse[entity] != NullIndex;
  }

  T &get(EntityId entity) { return mComponents[mSparse[entity]]; }

  // Direct access to data for Systems (cache friendly iteration)
  std::vector<T> &components() { return mComponents; }
  const std::vector<EntityId> &entities() const { return mPacked; }
  std::vector<EntityId> &entities() { return mPacked; }

private:
  static constexpr EntityId NullIndex = 0xFFFFFFFF;

  std::vector<EntityId> mSparse; // EntityId -> Index in mComponents
  std::vector<EntityId> mPacked; // Index -> EntityId (for reverse lookup)
  std::vector<T> mComponents;    // Dense data
};
