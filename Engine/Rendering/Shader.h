#ifndef SHADER_H
#define SHADER_H

#include <glad/glad.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

class Shader {
public:
  // --- Rule of Five: GPU resource (GL program) requires explicit management
  // ---

  Shader(const char *vertexShaderPath, const char *fragmentShaderPath);
  ~Shader();

  // Non-copyable: two Shader objects must not share the same GL program ID.
  Shader(const Shader &) = delete;
  Shader &operator=(const Shader &) = delete;

  // Movable: transfer ownership of the GL program to a new Shader object.
  Shader(Shader &&other) noexcept;
  Shader &operator=(Shader &&other) noexcept;

  void activate();
  bool reload();

  // Accessor for GL program ID (read-only).
  GLuint programId() const { return mId; }

  // utility functions
  std::string loadShaderSrc(const char *filepath);
  GLuint compileShader(const char *filepath, GLenum type);

  // Cached uniform location lookup (avoids per-frame glGetUniformLocation)
  GLint loc(const std::string &name);

  // uniform functions
  void setMat4(const std::string &name, const glm::mat4 &value);
  void setInt(const std::string &name, int value);
  void setFloat(const std::string &name, float value);

  void setBool(const std::string &name, bool value);
  void setVec4(const std::string &name, const glm::vec4 &v);
  void setVec3(const char *name, const glm::vec3 &v);

  void setVec3(const std::string &name, const glm::vec3 &v);

  void setMat3(const std::string &name, const glm::mat3 &value);

private:
  GLuint mId = 0;
  std::string mVertexPath;
  std::string mFragmentPath;
  std::unordered_map<std::string, GLint> mUniformCache;
};

#endif
