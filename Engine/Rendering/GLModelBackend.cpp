#include "GLModelBackend.h"

#include "Assets/AssetManager.h"
#include "Assets/FBXModel.h"
#include "Assets/OBJModel.h"
#include "Assets/UFBXModel.h"
#include "Shader.h"

void InstallGLModelBackend(AssetManager &assets) {
  ModelGpuBackend backend;

  backend.createOBJ = [](const MeshData &data) -> void * {
    auto *model = new OBJModel();
    if (!model->loadFromData(data)) {
      delete model;
      return nullptr;
    }
    return model;
  };
  backend.reloadOBJ = [](void *model, const MeshData &data) {
    return static_cast<OBJModel *>(model)->loadFromData(data);
  };
  backend.destroyOBJ = [](void *model) {
    delete static_cast<OBJModel *>(model);
  };

  backend.createGLTF = [](const MeshData &data) -> void * {
    auto *model = new FBXModel();
    if (!model->loadFromData(data)) {
      delete model;
      return nullptr;
    }
    return model;
  };
  backend.reloadGLTF = [](void *model, const MeshData &data) {
    return static_cast<FBXModel *>(model)->loadFromData(data);
  };
  backend.destroyGLTF = [](void *model) {
    delete static_cast<FBXModel *>(model);
  };

  backend.createUFBX = [](const MeshData &data) -> void * {
    auto *model = new UFBXModel();
    if (!model->loadFromData(data)) {
      delete model;
      return nullptr;
    }
    return model;
  };
  backend.destroyUFBX = [](void *model) {
    delete static_cast<UFBXModel *>(model);
  };

  assets.setGpuBackend(std::move(backend));
  assets.setShaderReloader([](Shader *shader) { return shader->reload(); });
}
