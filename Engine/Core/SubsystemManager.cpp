#include "SubsystemManager.h"
#include "Logger.h"

#include <functional>

void SubsystemManager::registerSubsystem(
    std::unique_ptr<IEngineSubsystem> subsystem) {
  if (!subsystem)
    return;

  const std::string n = subsystem->name();
  if (mNameToIndex.find(n) != mNameToIndex.end()) {
    LOG_ERROR("Core", "SubsystemManager duplicate subsystem name '" + n + "'");
    return;
  }

  mNameToIndex[n] = mSubsystems.size();
  mSubsystems.push_back(std::move(subsystem));
}

bool SubsystemManager::resolveInitOrder_() {
  enum class Visit { None, Visiting, Done };
  std::vector<Visit> visit(mSubsystems.size(), Visit::None);
  std::vector<std::size_t> order;
  order.reserve(mSubsystems.size());

  std::function<bool(std::size_t)> dfs = [&](std::size_t i) {
    if (visit[i] == Visit::Done)
      return true;
    if (visit[i] == Visit::Visiting) {
      LOG_ERROR("Core", "SubsystemManager dependency cycle at '" +
                            mSubsystems[i]->name() + "'");
      return false;
    }

    visit[i] = Visit::Visiting;

    for (const std::string &depName : mSubsystems[i]->dependencies()) {
      auto it = mNameToIndex.find(depName);
      if (it == mNameToIndex.end()) {
        LOG_ERROR("Core", "SubsystemManager missing dependency '" + depName +
                              "' for subsystem '" + mSubsystems[i]->name() +
                              "'");
        return false;
      }
      if (!dfs(it->second))
        return false;
    }

    visit[i] = Visit::Done;
    order.push_back(i);
    return true;
  };

  for (std::size_t i = 0; i < mSubsystems.size(); ++i) {
    if (!dfs(i))
      return false;
  }

  mInitOrder = std::move(order);
  return true;
}

bool SubsystemManager::initializeAll() {
  mInitialized.clear();

  if (!resolveInitOrder_())
    return false;

  for (std::size_t idx : mInitOrder) {
    IEngineSubsystem &ss = *mSubsystems[idx];
    if (!ss.initialize()) {
      LOG_ERROR("Core", "SubsystemManager failed to initialize '" + ss.name() +
                            "'");
      shutdownAll();
      return false;
    }
    mInitialized.push_back(idx);
  }

  return true;
}

void SubsystemManager::shutdownAll() {
  for (auto it = mInitialized.rbegin(); it != mInitialized.rend(); ++it) {
    mSubsystems[*it]->shutdown();
  }
  mInitialized.clear();
}
