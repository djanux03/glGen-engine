#include "GLDebug.h"

#include "Logger.h"

#include <glad/glad.h>

namespace {
const char *glSourceToStr(GLenum source) {
  switch (source) {
  case GL_DEBUG_SOURCE_API:
    return "API";
  case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
    return "WINDOW";
  case GL_DEBUG_SOURCE_SHADER_COMPILER:
    return "SHADER";
  case GL_DEBUG_SOURCE_THIRD_PARTY:
    return "THIRD_PARTY";
  case GL_DEBUG_SOURCE_APPLICATION:
    return "APP";
  case GL_DEBUG_SOURCE_OTHER:
    return "OTHER";
  default:
    return "UNKNOWN";
  }
}

const char *glTypeToStr(GLenum type) {
  switch (type) {
  case GL_DEBUG_TYPE_ERROR:
    return "ERROR";
  case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
    return "DEPRECATED";
  case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
    return "UNDEFINED";
  case GL_DEBUG_TYPE_PORTABILITY:
    return "PORTABILITY";
  case GL_DEBUG_TYPE_PERFORMANCE:
    return "PERFORMANCE";
  case GL_DEBUG_TYPE_MARKER:
    return "MARKER";
  case GL_DEBUG_TYPE_PUSH_GROUP:
    return "PUSH";
  case GL_DEBUG_TYPE_POP_GROUP:
    return "POP";
  case GL_DEBUG_TYPE_OTHER:
    return "OTHER";
  default:
    return "UNKNOWN";
  }
}

const char *glSeverityToStr(GLenum severity) {
  switch (severity) {
  case GL_DEBUG_SEVERITY_HIGH:
    return "HIGH";
  case GL_DEBUG_SEVERITY_MEDIUM:
    return "MEDIUM";
  case GL_DEBUG_SEVERITY_LOW:
    return "LOW";
  case GL_DEBUG_SEVERITY_NOTIFICATION:
    return "NOTIFY";
  default:
    return "UNKNOWN";
  }
}

void APIENTRY glDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity,
                              GLsizei, const GLchar *message, const void *) {
  if (!message)
    return;
  const std::string msg = std::string("GL[") + glSeverityToStr(severity) + "] " +
                          glSourceToStr(source) + "/" + glTypeToStr(type) +
                          " id=" + std::to_string(id) + ": " + message;
  if (type == GL_DEBUG_TYPE_PERFORMANCE) {
    LOG_WARN("Render", msg);
  } else if (severity == GL_DEBUG_SEVERITY_HIGH ||
             severity == GL_DEBUG_SEVERITY_MEDIUM) {
    LOG_ERROR("Render", msg);
  } else if (severity == GL_DEBUG_SEVERITY_LOW) {
    LOG_WARN("Render", msg);
  } else {
    LOG_TRACE("Render", msg);
  }
}
} // namespace

namespace GLDebug {
void initialize() {
  if (glDebugMessageCallback == nullptr) {
    LOG_WARN("Render", "GL debug output not supported by this context.");
    return;
  }

  glEnable(GL_DEBUG_OUTPUT);
  glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  glDebugMessageCallback(glDebugCallback, nullptr);
  glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION,
                        0, nullptr, GL_FALSE);
  LOG_INFO("Render", "OpenGL debug callback initialized.");
}
} // namespace GLDebug
