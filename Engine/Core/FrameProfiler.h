#pragma once

#include <chrono>
#include <string>
#include <vector>

class FrameProfiler {
public:
  struct Sample {
    std::string name;
    double ms = 0.0;
  };

  void beginFrame() { mCurrentSamples.clear(); }
  void endFrame() { mLastSamples = mCurrentSamples; }

  void addSample(const char *name, double ms) {
    if (!name)
      return;
    mCurrentSamples.push_back({name, ms});
  }

  const std::vector<Sample> &samples() const { return mLastSamples; }

  void setFrameMs(double ms) { mFrameMs = ms; }
  double frameMs() const { return mFrameMs; }

private:
  std::vector<Sample> mCurrentSamples;
  std::vector<Sample> mLastSamples;
  double mFrameMs = 0.0;
};

class ScopedCpuTimer {
public:
  ScopedCpuTimer(FrameProfiler &profiler, const char *name)
      : mProfiler(&profiler), mName(name),
        mStart(std::chrono::steady_clock::now()) {}

  ~ScopedCpuTimer() {
    if (!mProfiler || !mName)
      return;
    auto end = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(end - mStart).count();
    mProfiler->addSample(mName, ms);
  }

  ScopedCpuTimer(const ScopedCpuTimer &) = delete;
  ScopedCpuTimer &operator=(const ScopedCpuTimer &) = delete;

private:
  FrameProfiler *mProfiler = nullptr;
  const char *mName = nullptr;
  std::chrono::steady_clock::time_point mStart;
};
