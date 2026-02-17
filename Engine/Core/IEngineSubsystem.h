#pragma once

#include <string>
#include <vector>

class IEngineSubsystem {
public:
  virtual ~IEngineSubsystem() = default;

  virtual std::string name() const = 0;
  virtual std::vector<std::string> dependencies() const { return {}; }

  virtual bool initialize() = 0;
  virtual void shutdown() = 0;
};

