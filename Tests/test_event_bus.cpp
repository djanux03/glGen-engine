#include <doctest/doctest.h>
#include "EventBus.h"

#include <string>

namespace eb_test {
struct DamageEvent {
  int amount;
};
struct HealEvent {
  int amount;
};
struct ChatEvent {
  std::string message;
};
} // namespace eb_test

using namespace eb_test;

TEST_CASE("EventBus — subscribe and publish") {
  EventBus bus;
  int received = 0;

  bus.subscribe<DamageEvent>([&](const DamageEvent& e) { received = e.amount; });
  bus.publish(DamageEvent{42});

  CHECK(received == 42);
}

TEST_CASE("EventBus — multiple handlers all fire") {
  EventBus bus;
  int count = 0;

  bus.subscribe<DamageEvent>([&](const DamageEvent&) { count++; });
  bus.subscribe<DamageEvent>([&](const DamageEvent&) { count++; });
  bus.subscribe<DamageEvent>([&](const DamageEvent&) { count++; });

  bus.publish(DamageEvent{10});
  CHECK(count == 3);
}

TEST_CASE("EventBus — publish with no subscribers is a no-op") {
  EventBus bus;
  // Must not crash
  bus.publish(DamageEvent{99});
}

TEST_CASE("EventBus — independent event types") {
  EventBus bus;
  int damageCount = 0;
  int healCount = 0;

  bus.subscribe<DamageEvent>([&](const DamageEvent&) { damageCount++; });
  bus.subscribe<HealEvent>([&](const HealEvent&) { healCount++; });

  bus.publish(DamageEvent{10});
  bus.publish(DamageEvent{20});
  bus.publish(HealEvent{5});

  CHECK(damageCount == 2);
  CHECK(healCount == 1);
}

TEST_CASE("EventBus — handler receives correct payload") {
  EventBus bus;
  std::string msg;

  bus.subscribe<ChatEvent>(
      [&](const ChatEvent& e) { msg = e.message; });

  bus.publish(ChatEvent{"hello world"});
  CHECK(msg == "hello world");
}

TEST_CASE("EventBus — handlers fire in subscription order") {
  EventBus bus;
  std::vector<int> order;

  bus.subscribe<DamageEvent>([&](const DamageEvent&) { order.push_back(1); });
  bus.subscribe<DamageEvent>([&](const DamageEvent&) { order.push_back(2); });
  bus.subscribe<DamageEvent>([&](const DamageEvent&) { order.push_back(3); });

  bus.publish(DamageEvent{0});

  REQUIRE(order.size() == 3);
  CHECK(order[0] == 1);
  CHECK(order[1] == 2);
  CHECK(order[2] == 3);
}
