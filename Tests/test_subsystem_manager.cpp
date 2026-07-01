#include <doctest/doctest.h>
#include "SubsystemManager.h"

// Minimal test subsystem — no engine deps, just name/phase/deps.
class StubSubsystem : public IEngineSubsystem {
public:
  StubSubsystem(std::string n, SubsystemPhase p,
                std::vector<std::string> deps = {},
                bool initOk = true)
      : mName(std::move(n)), mPhase(p), mDeps(std::move(deps)),
        mInitOk(initOk) {}

  std::string name() const override { return mName; }
  SubsystemPhase phase() const override { return mPhase; }
  std::vector<std::string> dependencies() const override { return mDeps; }
  bool initialize() override {
    initCount++;
    return mInitOk;
  }
  void shutdown() override { shutdownCount++; }

  int initCount = 0;
  int shutdownCount = 0;

private:
  std::string mName;
  SubsystemPhase mPhase;
  std::vector<std::string> mDeps;
  bool mInitOk;
};

TEST_CASE("SubsystemManager — single subsystem init") {
  SubsystemManager mgr;
  auto* raw = new StubSubsystem("Audio", SubsystemPhase::Runtime);
  mgr.registerSubsystem(std::unique_ptr<IEngineSubsystem>(raw));

  CHECK(mgr.initializeAll());
  CHECK(raw->initCount == 1);

  auto names = mgr.resolvedInitOrderNames();
  REQUIRE(names.size() == 1);
  CHECK(names[0] == "Audio");

  mgr.shutdownAll();
  CHECK(raw->shutdownCount == 1);
}

TEST_CASE("SubsystemManager — dependency ordering") {
  SubsystemManager mgr;
  auto* renderer = new StubSubsystem("Renderer", SubsystemPhase::Foundation);
  auto* physics = new StubSubsystem("Physics", SubsystemPhase::Runtime,
                                     {"Renderer"});

  mgr.registerSubsystem(std::unique_ptr<IEngineSubsystem>(physics));
  mgr.registerSubsystem(std::unique_ptr<IEngineSubsystem>(renderer));

  CHECK(mgr.initializeAll());

  auto names = mgr.resolvedInitOrderNames();
  REQUIRE(names.size() == 2);
  // Renderer must come before Physics
  CHECK(names[0] == "Renderer");
  CHECK(names[1] == "Physics");
}

TEST_CASE("SubsystemManager — duplicate name rejected") {
  SubsystemManager mgr;
  mgr.registerSubsystem(
      std::make_unique<StubSubsystem>("Audio", SubsystemPhase::Runtime));
  mgr.registerSubsystem(
      std::make_unique<StubSubsystem>("Audio", SubsystemPhase::Runtime));

  CHECK(mgr.initializeAll());
  auto names = mgr.resolvedInitOrderNames();
  CHECK(names.size() == 1);  // second was rejected
}

TEST_CASE("SubsystemManager — null subsystem ignored") {
  SubsystemManager mgr;
  mgr.registerSubsystem(nullptr);
  CHECK(mgr.initializeAll());
  CHECK(mgr.resolvedInitOrderNames().empty());
}

TEST_CASE("SubsystemManager — profile selects subset") {
  SubsystemManager mgr;
  mgr.registerSubsystem(
      std::make_unique<StubSubsystem>("Audio", SubsystemPhase::Runtime));
  auto* input = new StubSubsystem("Input", SubsystemPhase::Platform);
  mgr.registerSubsystem(std::unique_ptr<IEngineSubsystem>(input));

  mgr.registerProfile({"HeadlessProfile", {"Audio"}});
  CHECK(mgr.initializeProfile("HeadlessProfile"));

  auto names = mgr.resolvedInitOrderNames();
  CHECK(names.size() == 1);
  CHECK(names[0] == "Audio");
  CHECK(input->initCount == 0);  // Input was excluded
}

TEST_CASE("SubsystemManager — profile pulls in deps") {
  SubsystemManager mgr;
  auto* base = new StubSubsystem("Base", SubsystemPhase::Platform);
  auto* derived = new StubSubsystem("Derived", SubsystemPhase::Runtime,
                                     {"Base"});
  mgr.registerSubsystem(std::unique_ptr<IEngineSubsystem>(base));
  mgr.registerSubsystem(std::unique_ptr<IEngineSubsystem>(derived));

  mgr.registerProfile({"MinProfile", {"Derived"}});
  CHECK(mgr.initializeProfile("MinProfile"));

  auto names = mgr.resolvedInitOrderNames();
  CHECK(names.size() == 2);  // Base pulled in
  CHECK(names[0] == "Base");
  CHECK(names[1] == "Derived");
}

TEST_CASE("SubsystemManager — missing dep fails") {
  SubsystemManager mgr;
  mgr.registerSubsystem(std::make_unique<StubSubsystem>(
      "Orphan", SubsystemPhase::Runtime, std::vector<std::string>{"Missing"}));
  CHECK_FALSE(mgr.initializeAll());
}

TEST_CASE("SubsystemManager — init failure triggers shutdown") {
  SubsystemManager mgr;
  auto* ok = new StubSubsystem("OK", SubsystemPhase::Platform);
  auto* fail = new StubSubsystem("Fail", SubsystemPhase::Runtime,
                                  {}, /*initOk=*/false);

  mgr.registerSubsystem(std::unique_ptr<IEngineSubsystem>(ok));
  mgr.registerSubsystem(std::unique_ptr<IEngineSubsystem>(fail));

  CHECK_FALSE(mgr.initializeAll());
  // OK was initialized, then Fail failed, triggering shutdown of OK
  CHECK(ok->initCount == 1);
  CHECK(ok->shutdownCount == 1);
}

TEST_CASE("SubsystemManager — phase ordering respected") {
  SubsystemManager mgr;
  mgr.registerSubsystem(std::make_unique<StubSubsystem>(
      "Tooling", SubsystemPhase::Tooling));
  mgr.registerSubsystem(std::make_unique<StubSubsystem>(
      "Platform", SubsystemPhase::Platform));
  mgr.registerSubsystem(std::make_unique<StubSubsystem>(
      "Runtime", SubsystemPhase::Runtime));

  CHECK(mgr.initializeAll());
  auto names = mgr.resolvedInitOrderNames();
  REQUIRE(names.size() == 3);
  CHECK(names[0] == "Platform");
  CHECK(names[1] == "Runtime");
  CHECK(names[2] == "Tooling");
}

TEST_CASE("SubsystemManager — unknown profile fails") {
  SubsystemManager mgr;
  CHECK_FALSE(mgr.initializeProfile("DoesNotExist"));
}
