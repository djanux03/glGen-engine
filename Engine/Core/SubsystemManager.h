#pragma once

#include "IEngineSubsystem.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class SubsystemManager {
public:
  void registerSubsystem(std::unique_ptr<IEngineSubsystem> subsystem);

  bool initializeAll();
  void shutdownAll();

private:
  bool resolveInitOrder_();

  std::vector<std::unique_ptr<IEngineSubsystem>> mSubsystems;
  std::unordered_map<std::string, std::size_t> mNameToIndex;

  std::vector<std::size_t> mInitOrder;
  std::vector<std::size_t> mInitialized;
};

