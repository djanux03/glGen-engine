#pragma once

#include "IEngineSubsystem.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class SubsystemManager {
public:
  struct StartupProfile {
    std::string name;
    std::vector<std::string> roots;
  };

  void registerSubsystem(std::unique_ptr<IEngineSubsystem> subsystem);
  void registerProfile(StartupProfile profile);

  bool initializeAll();
  bool initializeProfile(const std::string &profileName);
  void shutdownAll();

  std::vector<std::string> resolvedInitOrderNames() const;

private:
  bool resolveInitOrder_(const std::vector<std::string> &roots,
                         const std::string &profileName);

  std::vector<std::unique_ptr<IEngineSubsystem>> mSubsystems;
  std::unordered_map<std::string, std::size_t> mNameToIndex;
  std::unordered_map<std::string, std::vector<std::string>> mProfiles;

  std::vector<std::size_t> mInitOrder;
  std::vector<std::size_t> mInitialized;
  std::string mActiveProfile = "All";
};
