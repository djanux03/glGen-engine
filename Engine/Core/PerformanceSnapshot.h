#pragma once

#include <string>
#include <vector>

enum class PerformanceBottleneck {
  Collecting,
  CpuDriver,
  Gpu,
  Mixed,
  WithinBudget
};

inline const char *performanceBottleneckLabel(PerformanceBottleneck bottleneck) {
  switch (bottleneck) {
  case PerformanceBottleneck::CpuDriver:
    return "CPU / driver submission";
  case PerformanceBottleneck::Gpu:
    return "GPU";
  case PerformanceBottleneck::Mixed:
    return "Mixed CPU + GPU";
  case PerformanceBottleneck::WithinBudget:
    return "Frame is within 60 FPS budget";
  case PerformanceBottleneck::Collecting:
  default:
    return "Collecting GPU timing";
  }
}

struct FramePerformanceSnapshot {
  float frameMs = 0.0f;
  float fps = 0.0f;
  float gpuFrameMs = 0.0f;
  float gpuShadowMs = 0.0f;
  float gpuMainMs = 0.0f;
  float gpuMainSkyMs = 0.0f;
  float gpuMainTerrainMs = 0.0f;
  float gpuMainSceneMs = 0.0f;
  float gpuMainPostMs = 0.0f;
  float cpuGpuGapMs = 0.0f;
  bool gpuTimerReady = false;
  PerformanceBottleneck bottleneck = PerformanceBottleneck::Collecting;

  int entityCount = 0;
  int particleCount = 0;
  int visibleDrawn = 0;
  int visibleCulled = 0;
  int drawCallsMain = 0;
  int drawCallsShadow = 0;
  int instancedDrawCallsMain = 0;
  int instancedDrawCallsShadow = 0;
  int instancedUploadsMain = 0;
  int instancedUploadsShadow = 0;
  int instancedUploadSkipsMain = 0;
  int instancedUploadSkipsShadow = 0;
  int instancedUploadBytesMain = 0;
  int instancedUploadBytesShadow = 0;
  int instancedClustersTestedMain = 0;
  int instancedClustersTestedShadow = 0;
  int instancedClustersVisibleMain = 0;
  int instancedClustersVisibleShadow = 0;
  int shadowDistanceCulled = 0;
  int shadowSmallCasterCulled = 0;
  int shadowCascadeCount = 0;
  int shadowCascadesUpdated = 0;
  bool shadowCascadeStaggered = false;

  int terrainDrawCalls = 0;
  int terrainVisibleChunks = 0;
  int terrainFrustumCulledChunks = 0;
  int terrainHorizonCulledChunks = 0;
  int terrainGpuPagesUsed = 0;
  int terrainUploads = 0;
  int terrainUploadSkips = 0;
  int terrainUploadBytes = 0;
  int terrainPendingJobs = 0;
  int terrainPendingUploads = 0;
  int terrainCollisionBodies = 0;
  bool gpuTerrainActive = false;
  bool gpuTerrainFallback = false;

  int glProgramBinds = 0;
  int glTextureBinds = 0;
  int glVaoBinds = 0;
  int glStateChanges = 0;

  std::string submissionBackendLabel = "Direct";
  bool modernSubmissionBackend = false;
  std::vector<std::string> renderPassOrder;
};

struct PlayPerfHudSettings {
  bool enabled = true;
  bool expanded = false;
  float opacity = 0.38f;
};

struct PerformanceLogSettings {
  bool enabled = true;
  bool logEveryFrame = false;
  bool logPeriodicSummary = true;
  bool logSpikeFrames = true;
  float spikeThresholdMs = 40.0f;
  float summaryIntervalSec = 2.0f;
  float spikeCooldownSec = 1.0f;
  int cpuSampleLimit = 6;
};
