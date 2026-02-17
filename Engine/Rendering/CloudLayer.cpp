#include "CloudLayer.h"
#include "Shader.h"
#include <glad/glad.h>

bool CloudLayer::loadFromFile(const std::string &objPath) {
  return mModel.loadFromFile(objPath);
}

void CloudLayer::shutdown() { mModel.shutdown(); }

void CloudLayer::draw(Shader &shader, const glm::vec3 &position,
                      const glm::vec3 &scale, float alpha) {
  // Transparent pass setup: draw after opaque, enable blending, disable depth
  // writes. [web:48]
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA,
              GL_ONE_MINUS_SRC_ALPHA); // standard alpha blend [web:48]
  glDepthMask(GL_FALSE); // don't write depth for transparency [web:48]

  // Tell shader we're drawing cloud cards/mesh (unlit + alpha control)
  shader.setBool("uCloudMeshPass", true);
  shader.setFloat("uCloudMeshAlpha", alpha);

  // Draw using the existing OBJModel pipeline
  mModel.draw(shader, position, glm::vec3(0.0f), scale);

  shader.setBool("uCloudMeshPass", false);

  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
}
