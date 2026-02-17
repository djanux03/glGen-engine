#pragma once

#include <glad/glad.h>

class GLStateCache {
public:
  static GLStateCache &instance();

  void useProgram(GLuint program);
  void bindTexture2D(GLuint unit, GLuint tex);
  void bindVertexArray(GLuint vao);

  void setBlend(bool enabled);
  void setBlendFunc(GLenum src, GLenum dst);
  void setDepthMask(bool enabled);
  void setCullFace(GLenum mode);
  void setPolygonMode(GLenum mode);

private:
  GLStateCache() = default;

  GLuint mProgram = 0;
  GLuint mVAO = 0;
  GLenum mBlendSrc = GL_ONE;
  GLenum mBlendDst = GL_ZERO;
  bool mBlendEnabled = false;
  bool mDepthMask = true;
  GLenum mCullFace = GL_BACK;
  GLenum mPolygonMode = GL_FILL;
  GLuint mTex2D[32] = {};
};

