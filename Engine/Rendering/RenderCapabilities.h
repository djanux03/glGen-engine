#pragma once

#include <cstdint>

enum class RenderSubmissionBackend : uint8_t {
  Direct = 0,
  Modern = 1
};

inline const char *renderSubmissionBackendLabel(RenderSubmissionBackend backend) {
  switch (backend) {
  case RenderSubmissionBackend::Modern:
    return "Modern";
  case RenderSubmissionBackend::Direct:
  default:
    return "Direct";
  }
}

struct RendererCapabilities {
  int glMajor = 0;
  int glMinor = 0;
  bool supportsOpenGL43 = false;
  bool supportsMultiDrawIndirect = false;
  bool supportsMapBufferRange = false;
  bool supportsBufferStorage = false;
  RenderSubmissionBackend preferredSubmissionBackend =
      RenderSubmissionBackend::Direct;
};
