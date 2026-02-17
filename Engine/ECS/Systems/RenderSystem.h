#pragma once
#include "Assets/OBJModel.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "Rendering/Shader.h"

class RenderSystem {
public:
  void update(Registry &registry, Shader &shader, bool shadowPass = false) {
    // Set pass-level uniforms ONCE (instead of per-object in OBJModel::draw)
    if (!shadowPass) {
      shader.setBool("uGlowPass", false);
      shader.setBool("uCloudPass", false);
      shader.setInt("texture1", 0);
    }

    // Multi-component view: only entities with BOTH Mesh + Transform
    for (auto entity : registry.view2<MeshComponent, TransformComponent>()) {
      auto &mesh = registry.get<MeshComponent>(entity);

      if (!mesh.visible)
        continue;
      if (shadowPass && !mesh.castsShadow)
        continue;
      if (!mesh.model)
        continue;

      auto &transform = registry.get<TransformComponent>(entity);

      if (shadowPass) {
        mesh.model->drawDepth(shader, transform.position, transform.rotation,
                              transform.scale);
      } else {
        mesh.model->draw(shader, transform.position, transform.rotation,
                         transform.scale);
      }
    }
  }
};
