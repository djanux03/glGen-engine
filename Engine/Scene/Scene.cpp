#include "Scene.h"

#include "OBJModel.h"

#include <utility> // std::move

Scene::Scene() = default;
Scene::~Scene() = default;

OBJModel *Scene::getOrLoadOBJ(const std::string &objPath) {
  auto it = mOBJCache.find(objPath);
  if (it != mOBJCache.end())
    return it->second.get();

  auto model = std::make_unique<OBJModel>();
  model->loadFromFile(objPath);

  OBJModel *ptr = model.get();
  mOBJCache.emplace(objPath, std::move(model));
  return ptr;
}
