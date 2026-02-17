#include "RenderGraph.h"
#include "Logger.h"

#include <functional>

void RenderGraph::clear() {
  mPasses.clear();
  mNameToIndex.clear();
  mLastExecutionOrder.clear();
}

void RenderGraph::addPass(Pass pass) {
  if (pass.name.empty() || !pass.execute)
    return;
  if (mNameToIndex.find(pass.name) != mNameToIndex.end()) {
    LOG_ERROR("Render", "RenderGraph duplicate pass '" + pass.name + "'");
    return;
  }
  mNameToIndex[pass.name] = mPasses.size();
  mPasses.push_back(std::move(pass));
}

bool RenderGraph::buildExecutionOrder_(std::vector<std::size_t> &outOrder) {
  enum class Mark { None, Visiting, Done };
  std::vector<Mark> marks(mPasses.size(), Mark::None);
  outOrder.clear();
  outOrder.reserve(mPasses.size());

  std::function<bool(std::size_t)> dfs = [&](std::size_t i) {
    if (marks[i] == Mark::Done)
      return true;
    if (marks[i] == Mark::Visiting) {
      LOG_ERROR("Render", "RenderGraph cycle detected at pass '" +
                              mPasses[i].name + "'");
      return false;
    }
    marks[i] = Mark::Visiting;
    for (const std::string &dep : mPasses[i].deps) {
      auto it = mNameToIndex.find(dep);
      if (it == mNameToIndex.end()) {
        LOG_ERROR("Render", "RenderGraph missing dependency '" + dep +
                                "' for pass '" + mPasses[i].name + "'");
        return false;
      }
      if (!dfs(it->second))
        return false;
    }
    marks[i] = Mark::Done;
    outOrder.push_back(i);
    return true;
  };

  for (std::size_t i = 0; i < mPasses.size(); ++i) {
    if (!dfs(i))
      return false;
  }
  return true;
}

bool RenderGraph::execute() {
  std::vector<std::size_t> order;
  if (!buildExecutionOrder_(order))
    return false;

  mLastExecutionOrder.clear();
  for (std::size_t idx : order) {
    mPasses[idx].execute();
    mLastExecutionOrder.push_back(mPasses[idx].name);
  }
  return true;
}
