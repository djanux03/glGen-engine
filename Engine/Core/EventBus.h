#pragma once

#include <functional>
#include <typeindex>
#include <unordered_map>
#include <vector>

class EventBus {
public:
  template <typename Event>
  using Handler = std::function<void(const Event &)>;

  template <typename Event> void subscribe(Handler<Event> handler) {
    auto &list = mHandlers[std::type_index(typeid(Event))];
    list.push_back([h = std::move(handler)](const void *e) {
      h(*static_cast<const Event *>(e));
    });
  }

  template <typename Event> void publish(const Event &event) const {
    auto it = mHandlers.find(std::type_index(typeid(Event)));
    if (it == mHandlers.end())
      return;
    for (const auto &handler : it->second) {
      handler(&event);
    }
  }

private:
  using ErasedHandler = std::function<void(const void *)>;
  std::unordered_map<std::type_index, std::vector<ErasedHandler>> mHandlers;
};

