#pragma once

#include "ECS/Registry.h"
#include <glm/glm.hpp>

class Shader;

// GL wireframe view of all ColliderComponents. Lives in the rendering layer
// so the physics simulation itself stays graphics-API-free. Debug meshes are
// created lazily on first draw (needs a current GL context).
class PhysicsDebugRenderer {
public:
  PhysicsDebugRenderer() = default;
  ~PhysicsDebugRenderer();

  void drawColliders(Registry &reg, const glm::mat4 &view,
                     const glm::mat4 &proj, Shader &shader);

private:
  class OBJModel *mDebugCube = nullptr;
  class OBJModel *mDebugSphere = nullptr;
};
