#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class SubsystemPhase : uint8_t {
  Platform = 0,    // Windowing / graphics context
  Foundation = 1,  // Core services and shared systems
  Runtime = 2,     // Runtime simulation systems
  Tooling = 3,     // Editor/debug tooling
  Application = 4, // App-layer orchestration
};

inline const char *toString(SubsystemPhase p) {
  switch (p) {
  case SubsystemPhase::Platform:
    return "Platform";
  case SubsystemPhase::Foundation:
    return "Foundation";
  case SubsystemPhase::Runtime:
    return "Runtime";
  case SubsystemPhase::Tooling:
    return "Tooling";
  case SubsystemPhase::Application:
    return "Application";
  }
  return "Unknown";
}

class IEngineSubsystem {
public:
  virtual ~IEngineSubsystem() = default;

  virtual std::string name() const = 0;
  virtual SubsystemPhase phase() const { return SubsystemPhase::Runtime; }
  virtual std::vector<std::string> dependencies() const { return {}; }

  virtual bool initialize() = 0;
  virtual void shutdown() = 0;
};
