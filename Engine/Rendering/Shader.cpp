#include "Shader.h"
#include <glm/gtc/type_ptr.hpp>

Shader::Shader(const char *vertexShaderPath, const char *fragmentShaderPath) {
  int success;
  char infoLog[512];

  GLuint vertexShader = compileShader(vertexShaderPath, GL_VERTEX_SHADER);
  GLuint fragShader = compileShader(fragmentShaderPath, GL_FRAGMENT_SHADER);

  id = glCreateProgram();
  glAttachShader(id, vertexShader);
  glAttachShader(id, fragShader);
  glLinkProgram(id);

  glGetProgramiv(id, GL_LINK_STATUS, &success);
  if (!success) {
    glGetProgramInfoLog(id, 512, NULL, infoLog);
    std::cout << "Linking error: " << std::endl << infoLog << std::endl;
  }

  glDeleteShader(vertexShader);
  glDeleteShader(fragShader);
}

void Shader::activate() { glUseProgram(id); }
std::string Shader::loadShaderSrc(const char *filename) {

  std::ifstream file;
  std::stringstream buf;

  std::string ret = "";

  file.open(filename);
  if (file.is_open()) {
    buf << file.rdbuf();
    ret = buf.str();
  } else {
    std::cout << "could not open " << filename << std::endl;
  }
  file.close();
  return ret;
}

GLuint Shader::compileShader(const char *filepath, GLenum type) {
  int success;
  char infoLog[512];

  GLuint ret = glCreateShader(type);
  std::string shaderSrc = loadShaderSrc(filepath);
  const GLchar *shader = shaderSrc.c_str();
  glShaderSource(ret, 1, &shader, NULL);
  glCompileShader(ret);

  glGetShaderiv(ret, GL_COMPILE_STATUS, &success);
  if (!success) {
    glGetShaderInfoLog(ret, 512, NULL, infoLog);
    std::cout << "Error compiling shader: " << infoLog << std::endl;
  }
  return ret;
}

// ---------------------------------------------------------------------------
// Cached uniform location lookup — avoids per-frame glGetUniformLocation
// ---------------------------------------------------------------------------
GLint Shader::loc(const std::string &name) {
  auto it = mUniformCache.find(name);
  if (it != mUniformCache.end())
    return it->second;
  GLint l = glGetUniformLocation(id, name.c_str());
  mUniformCache[name] = l;
  return l;
}

void Shader::setMat4(const std::string &name, glm::mat4 value) {
  glUniformMatrix4fv(loc(name), 1, GL_FALSE, glm::value_ptr(value));
}
void Shader::setInt(const std::string &name, int value) {
  glUniform1i(loc(name), value);
}
void Shader::setFloat(const std::string &name, float value) {
  glUniform1f(loc(name), value);
}

void Shader::setBool(const std::string &name, bool value) {
  glUniform1i(loc(name), (int)value);
}

void Shader::setVec4(const std::string &name, const glm::vec4 &v) {
  glUniform4f(loc(name), v.x, v.y, v.z, v.w);
}

void Shader::setVec3(const char *name, const glm::vec3 &v) {
  glUniform3fv(loc(name), 1, glm::value_ptr(v));
}
void Shader::setVec3(const std::string &name, const glm::vec3 &v) {
  glUniform3fv(loc(name), 1, glm::value_ptr(v));
}

void Shader::setMat3(const std::string &name, const glm::mat3 &value) {
  glUniformMatrix3fv(loc(name), 1, GL_FALSE, glm::value_ptr(value));
}