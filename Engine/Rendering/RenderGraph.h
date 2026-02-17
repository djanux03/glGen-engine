#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class RenderGraph {
public:
  struct Pass {
    std::string name;
    std::vector<std::string> deps;
    std::function<void()> execute;
  };

  void clear();
  void addPass(Pass pass);
  bool execute();

  const std::vector<std::string> &lastExecutionOrder() const {
    return mLastExecutionOrder;
  }

private:
  bool buildExecutionOrder_(std::vector<std::size_t> &outOrder);

  std::vector<Pass> mPasses;
  std::unordered_map<std::string, std::size_t> mNameToIndex;
  std::vector<std::string> mLastExecutionOrder;
};

