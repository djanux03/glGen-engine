#include <doctest/doctest.h>
#include "Entity.h"
#include "Registry.h"

namespace reg_test {
struct Transform {
  float x = 0, y = 0, z = 0;
};
struct Health {
  int hp = 100;
};
struct Tag {
  std::string label;
};
} // namespace reg_test

using namespace reg_test;

TEST_CASE("Registry — create returns unique IDs") {
  Registry reg;
  EntityId a = reg.create();
  EntityId b = reg.create();
  EntityId c = reg.create();

  CHECK(a != b);
  CHECK(b != c);
  CHECK(a != c);
}

TEST_CASE("Registry — destroy recycles index with new generation version") {
  Registry reg;
  EntityId a = reg.create();
  reg.destroy(a);

  CHECK_FALSE(reg.valid(a)); // old handle is invalid

  EntityId b = reg.create();
  CHECK(b != a); // generation version differs
  CHECK(EntityTraits::index(b) == EntityTraits::index(a)); // index recycled
  CHECK(reg.valid(b));
}

TEST_CASE("Registry — emplace and get") {
  Registry reg;
  EntityId e = reg.create();
  reg.emplace<Transform>(e, Transform{1.0f, 2.0f, 3.0f});

  auto &t = reg.get<Transform>(e);
  CHECK(t.x == doctest::Approx(1.0f));
  CHECK(t.y == doctest::Approx(2.0f));
  CHECK(t.z == doctest::Approx(3.0f));
}

TEST_CASE("Registry — has") {
  Registry reg;
  EntityId e = reg.create();

  CHECK_FALSE(reg.has<Transform>(e));
  reg.emplace<Transform>(e);
  CHECK(reg.has<Transform>(e));
}

TEST_CASE("Registry — removeComponent") {
  Registry reg;
  EntityId e = reg.create();
  reg.emplace<Health>(e, Health{50});
  REQUIRE(reg.has<Health>(e));

  reg.removeComponent<Health>(e);
  CHECK_FALSE(reg.has<Health>(e));
}

TEST_CASE("Registry — view single component") {
  Registry reg;
  EntityId a = reg.create();
  EntityId b = reg.create();
  EntityId c = reg.create();

  reg.emplace<Transform>(a);
  reg.emplace<Transform>(c);
  // b has no Transform

  auto &entities = reg.view<Transform>();
  CHECK(entities.size() == 2);
}

TEST_CASE("Registry — viewAll multi-component") {
  Registry reg;
  EntityId a = reg.create();
  EntityId b = reg.create();
  EntityId c = reg.create();

  reg.emplace<Transform>(a);
  reg.emplace<Health>(a, Health{100});

  reg.emplace<Transform>(b);
  // b has no Health

  reg.emplace<Transform>(c);
  reg.emplace<Health>(c, Health{50});

  std::vector<EntityId> result;
  for (EntityId e : reg.viewAll<Transform, Health>()) {
    result.push_back(e);
  }

  CHECK(result.size() == 2);
  CHECK(std::find(result.begin(), result.end(), a) != result.end());
  CHECK(std::find(result.begin(), result.end(), c) != result.end());
}

TEST_CASE("Registry — view2 is alias for viewAll") {
  Registry reg;
  EntityId a = reg.create();
  reg.emplace<Transform>(a);
  reg.emplace<Health>(a, Health{100});

  std::vector<EntityId> result;
  for (EntityId e : reg.view2<Transform, Health>()) {
    result.push_back(e);
  }
  CHECK(result.size() == 1);
  CHECK(result[0] == a);
}

TEST_CASE("Registry — viewWhere with predicate") {
  Registry reg;
  EntityId a = reg.create();
  EntityId b = reg.create();

  reg.emplace<Health>(a, Health{10});
  reg.emplace<Health>(b, Health{90});

  std::vector<EntityId> result;
  for (EntityId e : reg.viewWhere<Health>([&](EntityId eid) {
         return reg.get<Health>(eid).hp > 50;
       })) {
    result.push_back(e);
  }

  CHECK(result.size() == 1);
  CHECK(result[0] == b);
}

TEST_CASE("Registry — componentView dense access") {
  Registry reg;
  EntityId a = reg.create();
  EntityId b = reg.create();

  reg.emplace<Health>(a, Health{10});
  reg.emplace<Health>(b, Health{20});

  auto &healths = reg.componentView<Health>();
  REQUIRE(healths.size() == 2);
  CHECK(healths[0].hp == 10);
  CHECK(healths[1].hp == 20);
}

TEST_CASE("Registry — destroy removes all components") {
  Registry reg;
  EntityId e = reg.create();
  reg.emplace<Transform>(e);
  reg.emplace<Health>(e, Health{100});

  reg.destroy(e);
  CHECK_FALSE(reg.has<Transform>(e));
  CHECK_FALSE(reg.has<Health>(e));
}

TEST_CASE("Registry — create after destroy reuses index but not handle") {
  Registry reg;
  EntityId a = reg.create();
  EntityId b = reg.create();

  reg.destroy(a);
  EntityId c = reg.create();
  CHECK(EntityTraits::index(c) == EntityTraits::index(a));
  CHECK(c != a);

  // c is a fresh entity — should not have old components
  CHECK_FALSE(reg.has<Transform>(c));
}

TEST_CASE("Registry — view on empty pool returns empty") {
  Registry reg;
  auto &entities = reg.view<Tag>();
  CHECK(entities.empty());
}

TEST_CASE("CommandBuffer — queues and flushes operations") {
  Registry reg;
  CommandBuffer cmdBuf;

  EntityId createdEntity = EntityTraits::NullEntity;
  cmdBuf.create([&](EntityId id) { createdEntity = id; });
  cmdBuf.flush(reg);

  REQUIRE(reg.valid(createdEntity));

  cmdBuf.emplace<Health>(createdEntity, Health{75});
  cmdBuf.flush(reg);

  CHECK(reg.has<Health>(createdEntity));
  CHECK(reg.get<Health>(createdEntity).hp == 75);

  cmdBuf.destroy(createdEntity);
  cmdBuf.flush(reg);

  CHECK_FALSE(reg.valid(createdEntity));
}

TEST_CASE("Entity wrapper class API") {
  Registry reg;
  EntityId id = reg.create();
  Entity entity(id, &reg);

  CHECK(entity.isValid());

  entity.addComponent<Health>(Health{100});
  CHECK(entity.hasComponent<Health>());
  CHECK(entity.getComponent<Health>().hp == 100);

  entity.removeComponent<Health>();
  CHECK_FALSE(entity.hasComponent<Health>());

  entity.destroy();
  CHECK_FALSE(entity.isValid());
}
