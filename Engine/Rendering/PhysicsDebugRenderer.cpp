#include "PhysicsDebugRenderer.h"

#include "Assets/PrimitiveMeshGenerator.h"
#include "ECS/Components.h"
#include "GLStateCache.h"
#include "Shader.h"

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>

PhysicsDebugRenderer::~PhysicsDebugRenderer() {
  delete mDebugCube;
  delete mDebugSphere;
}

void PhysicsDebugRenderer::drawColliders(Registry &reg, const glm::mat4 &view,
                                         const glm::mat4 &proj,
                                         Shader &shader) {
  if (!mDebugCube)
    mDebugCube = PrimitiveMeshGenerator::createCube();
  if (!mDebugSphere)
    mDebugSphere = PrimitiveMeshGenerator::createSphere(16, 16);
  if (!mDebugCube || !mDebugSphere)
    return;

  GLStateCache::instance().setPolygonMode(GL_LINE);
  glDisable(GL_CULL_FACE);

  shader.activate();
  shader.setMat4("view", view);
  shader.setMat4("projection", proj);

  auto group = reg.view<ColliderComponent>();
  for (auto entity : group) {
    if (!reg.has<TransformComponent>(entity))
      continue;
    const auto &col = reg.get<ColliderComponent>(entity);
    const auto &transform = reg.get<TransformComponent>(entity);

    glm::vec4 color(1.0f); // Default white
    if (reg.has<RigidbodyComponent>(entity)) {
      auto type = reg.get<RigidbodyComponent>(entity).type;
      if (type == RigidbodyComponent::Type::Static) {
        color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f); // Red = Static
      } else if (type == RigidbodyComponent::Type::Kinematic) {
        color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f); // Green = Kinematic
      } else {
        color = glm::vec4(0.2f, 0.6f, 1.0f, 1.0f); // Blue = Dynamic
      }
    }

    shader.setVec4("uColor", color);

    glm::mat4 model = transform.getMatrix();
    model = glm::translate(model, col.offset);

    // Component dimensions are local-space. The entity transform handles scale,
    // rotation and pivot offset so the wireframe matches the Jolt shape.
    glm::vec3 drawScale(1.0f);
    OBJModel *drawModel = mDebugCube;

    if (col.shape == ColliderComponent::Shape::Box) {
      drawScale = col.dimensions; // Full dimensions, our cube is 1x1x1
      drawModel = mDebugCube;
    } else if (col.shape == ColliderComponent::Shape::Sphere) {
      drawScale =
          glm::vec3(col.dimensions.x *
                    2.0f); // Radius * 2 to get diameter, sphere is 1 diameter
      drawModel = mDebugSphere;
    } else if (col.shape == ColliderComponent::Shape::Capsule) {
      // Using sphere to approximate capsule for now, stretching it along Y
      drawScale = glm::vec3(col.dimensions.x * 2.0f,
                            col.dimensions.y + col.dimensions.x * 2.0f,
                            col.dimensions.x * 2.0f);
      drawModel = mDebugSphere;
    }

    model = glm::scale(model, drawScale);

    shader.setMat4("model", model);
    shader.setBool("uUseColor", true);

    // OBJModel::draw applies its own internal modeling if not bypassed.
    // Passing 0 for pos/rot/scale effectively bypasses it and uses our shader
    // uniform.
    drawModel->draw(shader, glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f));
  }

  GLStateCache::instance().setPolygonMode(GL_FILL);
  glEnable(GL_CULL_FACE);
}
