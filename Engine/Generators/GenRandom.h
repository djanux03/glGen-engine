#pragma once
// GenRandom.h — the deterministic RNG every generator uses.
//
// Determinism is the whole premise of the recipe model (AI_ASSET_PIPELINE_
// PLAN.md §2): the same recipe + seed must produce the same mesh on every
// machine, forever, or a recipe is not a reproducible description of an
// asset. std::mt19937's *sequence* is standardized, but std::uniform_real_
// distribution's mapping is NOT -- it legitimately differs between standard
// libraries. So the distributions are open-coded here rather than pulled from
// <random>, and this type is passed explicitly instead of any global state.

#include <cstdint>
#include <glm/glm.hpp>

namespace gen {

class GenRandom {
public:
  explicit GenRandom(uint32_t seed) : mState(seed ? seed : 0x9E3779B9u) {}

  uint32_t nextU32() {
    // xorshift32 — small, fast, and fully specified right here.
    uint32_t x = mState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    mState = x;
    return x;
  }

  // [0,1)
  float unit() { return static_cast<float>(nextU32() >> 8) * (1.0f / 16777216.0f); }
  // [lo,hi)
  float range(float lo, float hi) { return lo + unit() * (hi - lo); }
  // [-1,1)
  float signedUnit() { return unit() * 2.0f - 1.0f; }
  // [0,n)
  int index(int n) { return n > 0 ? static_cast<int>(nextU32() % static_cast<uint32_t>(n)) : 0; }
  bool chance(float p) { return unit() < p; }

  glm::vec3 inSphere() {
    // Rejection sampling; terminates fast (~52% acceptance) and avoids the
    // clustering a naive angular parameterisation produces.
    for (int i = 0; i < 32; ++i) {
      const glm::vec3 v(signedUnit(), signedUnit(), signedUnit());
      if (glm::dot(v, v) <= 1.0f)
        return v;
    }
    return glm::vec3(0.0f);
  }

  // A seed derived from this stream, for handing an independent, still
  // deterministic sub-stream to a child generator.
  uint32_t deriveSeed() { return nextU32(); }

private:
  uint32_t mState;
};

} // namespace gen
