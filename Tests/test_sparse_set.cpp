#include <doctest/doctest.h>
#include "SparseSet.h"

struct Position {
  float x, y, z;
};

struct Velocity {
  float dx, dy, dz;
};

TEST_CASE("SparseSet — emplace and has") {
  SparseSet<Position> pool;
  EntityId e = 5;

  CHECK_FALSE(pool.has(e));
  pool.emplace(e, Position{1.0f, 2.0f, 3.0f});
  CHECK(pool.has(e));
}

TEST_CASE("SparseSet — get returns correct data") {
  SparseSet<Position> pool;
  pool.emplace(10, Position{4.0f, 5.0f, 6.0f});

  auto& p = pool.get(10);
  CHECK(p.x == doctest::Approx(4.0f));
  CHECK(p.y == doctest::Approx(5.0f));
  CHECK(p.z == doctest::Approx(6.0f));
}

TEST_CASE("SparseSet — remove makes has() return false") {
  SparseSet<Position> pool;
  pool.emplace(3, Position{1.0f, 2.0f, 3.0f});
  REQUIRE(pool.has(3));

  pool.remove(3);
  CHECK_FALSE(pool.has(3));
}

TEST_CASE("SparseSet — swap-and-pop preserves other entities") {
  SparseSet<Position> pool;
  pool.emplace(1, Position{1.0f, 0.0f, 0.0f});
  pool.emplace(2, Position{2.0f, 0.0f, 0.0f});
  pool.emplace(3, Position{3.0f, 0.0f, 0.0f});

  pool.remove(1);  // should swap entity 3 into slot 0

  CHECK_FALSE(pool.has(1));
  CHECK(pool.has(2));
  CHECK(pool.has(3));

  CHECK(pool.get(2).x == doctest::Approx(2.0f));
  CHECK(pool.get(3).x == doctest::Approx(3.0f));
}

TEST_CASE("SparseSet — entities() returns packed list") {
  SparseSet<Position> pool;
  pool.emplace(10, Position{});
  pool.emplace(20, Position{});
  pool.emplace(30, Position{});

  auto& ents = pool.entities();
  REQUIRE(ents.size() == 3);
  // Order is insertion order
  CHECK(ents[0] == 10);
  CHECK(ents[1] == 20);
  CHECK(ents[2] == 30);
}

TEST_CASE("SparseSet — components() gives dense array") {
  SparseSet<int> pool;
  pool.emplace(0, 100);
  pool.emplace(1, 200);
  pool.emplace(2, 300);

  auto& comps = pool.components();
  REQUIRE(comps.size() == 3);
  CHECK(comps[0] == 100);
  CHECK(comps[1] == 200);
  CHECK(comps[2] == 300);
}

TEST_CASE("SparseSet — emplace duplicate returns existing") {
  SparseSet<int> pool;
  auto& first = pool.emplace(5, 42);
  auto& second = pool.emplace(5, 99);  // duplicate — should NOT overwrite

  CHECK(&first == &second);
  CHECK(pool.get(5) == 42);  // original value preserved
}

TEST_CASE("SparseSet — remove nonexistent entity is a no-op") {
  SparseSet<int> pool;
  pool.emplace(1, 10);

  pool.remove(999);  // does not exist — must not crash
  CHECK(pool.has(1));
  CHECK(pool.entities().size() == 1);
}

TEST_CASE("SparseSet — large entity IDs") {
  SparseSet<int> pool;
  EntityId big = 100'000;
  pool.emplace(big, 777);

  CHECK(pool.has(big));
  CHECK(pool.get(big) == 777);
}

TEST_CASE("SparseSet — empty set") {
  SparseSet<int> pool;

  CHECK_FALSE(pool.has(0));
  CHECK(pool.entities().empty());
  CHECK(pool.components().empty());
}

TEST_CASE("SparseSet — re-emplace after remove") {
  SparseSet<int> pool;
  pool.emplace(7, 10);
  pool.remove(7);
  CHECK_FALSE(pool.has(7));

  pool.emplace(7, 20);
  CHECK(pool.has(7));
  CHECK(pool.get(7) == 20);
}

TEST_CASE("SparseSet — multiple removals shrink correctly") {
  SparseSet<int> pool;
  pool.emplace(1, 10);
  pool.emplace(2, 20);
  pool.emplace(3, 30);
  pool.emplace(4, 40);

  pool.remove(2);
  pool.remove(4);

  CHECK(pool.entities().size() == 2);
  CHECK(pool.has(1));
  CHECK_FALSE(pool.has(2));
  CHECK(pool.has(3));
  CHECK_FALSE(pool.has(4));
}

TEST_CASE("SparseSet — independent pools for different types") {
  SparseSet<Position> posPool;
  SparseSet<Velocity> velPool;

  posPool.emplace(1, Position{1.0f, 2.0f, 3.0f});
  velPool.emplace(1, Velocity{0.1f, 0.2f, 0.3f});

  CHECK(posPool.has(1));
  CHECK(velPool.has(1));

  posPool.remove(1);
  CHECK_FALSE(posPool.has(1));
  CHECK(velPool.has(1));  // velocity unaffected
}
