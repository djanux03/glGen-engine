#include "SubsystemManager.h"
#include "Logger.h"

#include <algorithm>
#include <functional>
#include <sstream>

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

void SubsystemManager::registerProfile(StartupProfile profile) {
  if (profile.name.empty()) {
    LOG_ERROR("Core", "SubsystemManager rejected unnamed startup profile");
    return;
  }
  mProfiles[profile.name] = std::move(profile.roots);
}

bool SubsystemManager::resolveInitOrder_(const std::vector<std::string> &roots,
                                         const std::string &profileName) {
  enum class Visit { None, Visiting, Done };
  mInitOrder.clear();

  const std::size_t subsystemCount = mSubsystems.size();
  std::vector<Visit> visit(subsystemCount, Visit::None);
  std::vector<uint8_t> selected(subsystemCount, 0);
  std::vector<std::size_t> order;
  order.reserve(subsystemCount);

  std::function<bool(std::size_t)> markSelected = [&](std::size_t i) {
    if (selected[i])
      return true;
    selected[i] = 1;
    for (const std::string &depName : mSubsystems[i]->dependencies()) {
      auto it = mNameToIndex.find(depName);
      if (it == mNameToIndex.end()) {
        LOG_ERROR("Core", "SubsystemManager missing dependency '" + depName +
                              "' for subsystem '" + mSubsystems[i]->name() +
                              "'");
        return false;
      }
      if (!markSelected(it->second))
        return false;
    }
    return true;
  };

  if (roots.empty()) {
    for (std::size_t i = 0; i < subsystemCount; ++i)
      selected[i] = 1;
  } else {
    for (const std::string &rootName : roots) {
      auto it = mNameToIndex.find(rootName);
      if (it == mNameToIndex.end()) {
        LOG_ERROR("Core", "SubsystemManager unknown profile root '" + rootName +
                              "' for profile '" + profileName + "'");
        return false;
      }
      if (!markSelected(it->second))
        return false;
    }
  }

  std::function<bool(std::size_t)> dfs = [&](std::size_t i) {
    if (!selected[i])
      return true;
    if (visit[i] == Visit::Done)
      return true;
    if (visit[i] == Visit::Visiting) {
      LOG_ERROR("Core", "SubsystemManager dependency cycle at '" +
                            mSubsystems[i]->name() + "'");
      return false;
    }

    visit[i] = Visit::Visiting;

    const SubsystemPhase phase = mSubsystems[i]->phase();
    for (const std::string &depName : mSubsystems[i]->dependencies()) {
      auto it = mNameToIndex.find(depName);
      if (it == mNameToIndex.end()) {
        LOG_ERROR("Core", "SubsystemManager missing dependency '" + depName +
                              "' for subsystem '" + mSubsystems[i]->name() +
                              "'");
        return false;
      }
      const std::size_t depIdx = it->second;
      if (!selected[depIdx]) {
        LOG_ERROR("Core",
                  "SubsystemManager profile '" + profileName +
                      "' excludes required dependency '" + depName +
                      "' for subsystem '" + mSubsystems[i]->name() + "'");
        return false;
      }
      const SubsystemPhase depPhase = mSubsystems[depIdx]->phase();
      if ((int)depPhase > (int)phase) {
        LOG_ERROR("Core", "SubsystemManager phase violation: '" + depName +
                              "' (" + toString(depPhase) + ") -> '" +
                              mSubsystems[i]->name() + "' (" +
                              toString(phase) + ")");
        return false;
      }
      if (!dfs(it->second))
        return false;
    }

    visit[i] = Visit::Done;
    order.push_back(i);
    return true;
  };

  std::vector<std::size_t> rootsOrdered;
  rootsOrdered.reserve(subsystemCount);
  for (std::size_t i = 0; i < subsystemCount; ++i) {
    if (selected[i])
      rootsOrdered.push_back(i);
  }

  std::stable_sort(rootsOrdered.begin(), rootsOrdered.end(),
                   [this](std::size_t a, std::size_t b) {
                     const auto pa = (int)mSubsystems[a]->phase();
                     const auto pb = (int)mSubsystems[b]->phase();
                     if (pa != pb)
                       return pa < pb;
                     return a < b;
                   });

  for (std::size_t i : rootsOrdered) {
    if (!dfs(i))
      return false;
  }

  mInitOrder = std::move(order);
  return true;
}

bool SubsystemManager::initializeAll() {
  return initializeProfile("All");
}

bool SubsystemManager::initializeProfile(const std::string &profileName) {
  mInitialized.clear();

  std::vector<std::string> roots;
  if (profileName != "All") {
    const auto it = mProfiles.find(profileName);
    if (it == mProfiles.end()) {
      LOG_ERROR("Core", "SubsystemManager unknown startup profile '" +
                            profileName + "'");
      return false;
    }
    roots = it->second;
  }

  if (!resolveInitOrder_(roots, profileName))
    return false;

  mActiveProfile = profileName;

  std::ostringstream plan;
  plan << "Subsystem startup profile '" << profileName << "': ";
  bool first = true;
  for (std::size_t idx : mInitOrder) {
    if (!first)
      plan << " -> ";
    first = false;
    plan << mSubsystems[idx]->name() << "[" << toString(mSubsystems[idx]->phase())
         << "]";
  }
  LOG_INFO("Core", plan.str());

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

std::vector<std::string> SubsystemManager::resolvedInitOrderNames() const {
  std::vector<std::string> names;
  names.reserve(mInitOrder.size());
  for (std::size_t idx : mInitOrder)
    names.push_back(mSubsystems[idx]->name());
  return names;
}
