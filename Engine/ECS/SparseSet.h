#pragma once
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

using EntityId = std::uint32_t;

namespace EntityTraits {
static constexpr EntityId IndexMask = 0x000FFFFF;   // Lower 20 bits for entity index
static constexpr EntityId VersionMask = 0xFFF00000; // Upper 12 bits for generation version
static constexpr uint32_t VersionShift = 20;
static constexpr EntityId NullIndex = 0x000FFFFF;
static constexpr EntityId NullEntity = 0xFFFFFFFF;

inline EntityId index(EntityId id) { return id & IndexMask; }
inline uint32_t generation(EntityId id) {
  return (id & VersionMask) >> VersionShift;
}
inline EntityId make(EntityId index, uint32_t generation) {
  return (index & IndexMask) | ((generation << VersionShift) & VersionMask);
}
} // namespace EntityTraits

// Interface for type erasure (so Registry can store list of these)
class ISparseSet {
public:
  virtual ~ISparseSet() = default;
  virtual bool has(EntityId entity) const = 0;
  virtual void remove(EntityId entity) = 0;
  virtual void clear() = 0;
};

template <typename T> class SparseSet : public ISparseSet {
public:
  static constexpr size_t PAGE_SIZE = 4096;

  SparseSet() = default;
  ~SparseSet() override = default;

  SparseSet(SparseSet &&) noexcept = default;
  SparseSet &operator=(SparseSet &&) noexcept = default;
  SparseSet(const SparseSet &) = delete;
  SparseSet &operator=(const SparseSet &) = delete;

  // Add component data for an entity (overwrites if component already exists)
  T &emplace(EntityId entity, T component) {
    uint32_t idx = EntityTraits::index(entity);
    if (has(entity)) {
      EntityId denseIdx = getSparse(idx);
      mComponents[denseIdx] = std::move(component);
      mPacked[denseIdx] = entity;
      return mComponents[denseIdx];
    }

    EntityId denseIdx = static_cast<EntityId>(mPacked.size());
    setSparse(idx, denseIdx);
    mPacked.push_back(entity);
    mComponents.push_back(std::move(component));

    return mComponents.back();
  }

  // Replace existing component data
  T &replace(EntityId entity, T component) {
    return emplace(entity, std::move(component));
  }

  void remove(EntityId entity) override {
    if (!has(entity))
      return;

    uint32_t idx = EntityTraits::index(entity);
    EntityId denseIndex = getSparse(idx);
    EntityId lastIndex = static_cast<EntityId>(mPacked.size()) - 1;
    EntityId lastEntity = mPacked[lastIndex];
    uint32_t lastIdx = EntityTraits::index(lastEntity);

    // Move last component into hole
    mComponents[denseIndex] = std::move(mComponents[lastIndex]);
    mPacked[denseIndex] = lastEntity;

    // Update sparse map
    setSparse(lastIdx, denseIndex);
    setSparse(idx, EntityTraits::NullIndex);

    mPacked.pop_back();
    mComponents.pop_back();
  }

  bool has(EntityId entity) const override {
    uint32_t idx = EntityTraits::index(entity);
    uint32_t pageIdx = idx / PAGE_SIZE;
    uint32_t pageOffset = idx % PAGE_SIZE;

    if (pageIdx >= mPages.size() || !mPages[pageIdx])
      return false;

    return mPages[pageIdx][pageOffset] != EntityTraits::NullIndex;
  }

  void clear() override {
    mPages.clear();
    mPacked.clear();
    mComponents.clear();
  }

  T &get(EntityId entity) {
    uint32_t idx = EntityTraits::index(entity);
    return mComponents[getSparse(idx)];
  }

  const T &get(EntityId entity) const {
    uint32_t idx = EntityTraits::index(entity);
    return mComponents[getSparse(idx)];
  }

  // Direct access to data for Systems (cache friendly iteration)
  std::vector<T> &components() { return mComponents; }
  const std::vector<T> &components() const { return mComponents; }

  const std::vector<EntityId> &entities() const { return mPacked; }
  std::vector<EntityId> &entities() { return mPacked; }

  size_t size() const { return mPacked.size(); }
  bool empty() const { return mPacked.empty(); }

private:
  EntityId getSparse(uint32_t idx) const {
    uint32_t pageIdx = idx / PAGE_SIZE;
    uint32_t pageOffset = idx % PAGE_SIZE;
    if (pageIdx >= mPages.size() || !mPages[pageIdx])
      return EntityTraits::NullIndex;
    return mPages[pageIdx][pageOffset];
  }

  void setSparse(uint32_t idx, EntityId denseIdx) {
    uint32_t pageIdx = idx / PAGE_SIZE;
    uint32_t pageOffset = idx % PAGE_SIZE;
    if (pageIdx >= mPages.size()) {
      mPages.resize(pageIdx + 1);
    }
    if (!mPages[pageIdx]) {
      mPages[pageIdx] = std::make_unique<EntityId[]>(PAGE_SIZE);
      std::fill_n(mPages[pageIdx].get(), PAGE_SIZE, EntityTraits::NullIndex);
    }
    mPages[pageIdx][pageOffset] = denseIdx;
  }

  std::vector<std::unique_ptr<EntityId[]>> mPages;
  std::vector<EntityId> mPacked;
  std::vector<T> mComponents;
};
