#include "GLStateCache.h"

GLStateCache &GLStateCache::instance() {
  static GLStateCache s;
  return s;
}

void GLStateCache::useProgram(GLuint program) {
  if (mProgram == program)
    return;
  glUseProgram(program);
  mProgram = program;
}

void GLStateCache::bindTexture2D(GLuint unit, GLuint tex) {
  if (unit >= 32)
    return;
  if (mTex2D[unit] == tex)
    return;
  glActiveTexture(GL_TEXTURE0 + unit);
  glBindTexture(GL_TEXTURE_2D, tex);
  mTex2D[unit] = tex;
}

void GLStateCache::bindVertexArray(GLuint vao) {
  if (mVAO == vao)
    return;
  glBindVertexArray(vao);
  mVAO = vao;
}

void GLStateCache::setBlend(bool enabled) {
  if (mBlendEnabled == enabled)
    return;
  if (enabled)
    glEnable(GL_BLEND);
  else
    glDisable(GL_BLEND);
  mBlendEnabled = enabled;
}

void GLStateCache::setBlendFunc(GLenum src, GLenum dst) {
  if (mBlendSrc == src && mBlendDst == dst)
    return;
  glBlendFunc(src, dst);
  mBlendSrc = src;
  mBlendDst = dst;
}

void GLStateCache::setDepthMask(bool enabled) {
  if (mDepthMask == enabled)
    return;
  glDepthMask(enabled ? GL_TRUE : GL_FALSE);
  mDepthMask = enabled;
}

void GLStateCache::setCullFace(GLenum mode) {
  if (mCullFace == mode)
    return;
  glCullFace(mode);
  mCullFace = mode;
}

void GLStateCache::setPolygonMode(GLenum mode) {
  if (mPolygonMode == mode)
    return;
  glPolygonMode(GL_FRONT_AND_BACK, mode);
  mPolygonMode = mode;
}

