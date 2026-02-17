#include "GLDebug.h"

#include "Logger.h"

#include <glad/glad.h>

namespace {
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

void APIENTRY glDebugCallback(GLenum, GLenum, GLuint, GLenum severity,
                              GLsizei, const GLchar *message, const void *) {
  if (!message)
    return;
  const std::string msg =
      std::string("GL[") + glSeverityToStr(severity) + "]: " + message;
  if (severity == GL_DEBUG_SEVERITY_HIGH || severity == GL_DEBUG_SEVERITY_MEDIUM)
    LOG_ERROR("Render", msg);
  else if (severity == GL_DEBUG_SEVERITY_LOW)
    LOG_WARN("Render", msg);
  else
    LOG_TRACE("Render", msg);
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
