#include "Scene.h"

#include "OBJModel.h"
#include "Shader.h"

#include <utility>   // std::move

Scene::Scene() = default;
Scene::~Scene() = default;

Scene::EntityId Scene::nextId_()
{
    return mNextId++;
}

OBJModel* Scene::getOrLoadOBJ(const std::string& objPath)
{
    auto it = mOBJCache.find(objPath);
    if (it != mOBJCache.end())
        return it->second.get();

    auto model = std::make_unique<OBJModel>();

    // Your code calls loadFromFile() without checking return; do the same here.
    // If loadFromFile returns bool in your implementation, you can add a check later.
    model->loadFromFile(objPath);

    OBJModel* ptr = model.get();
    mOBJCache.emplace(objPath, std::move(model));
    return ptr;
}

Scene::EntityId Scene::createEntity(const std::string& name)
{
    Entity e;
    e.id = nextId_();
    e.name = name;

    mIndexById[e.id] = mEntities.size();
    mEntities.push_back(std::move(e));
    return mEntities.back().id;
}

Scene::EntityId Scene::createEntityOBJ(const std::string& name,
                                       const std::string& objPath)
{
    EntityId id = createEntity(name);
    Entity* e = get(id);
    if (e)
        e->model = getOrLoadOBJ(objPath);
    return id;
}

bool Scene::destroyEntity(EntityId id)
{
    auto it = mIndexById.find(id);
    if (it == mIndexById.end())
        return false;

    std::size_t idx = it->second;
    std::size_t last = mEntities.size() - 1;

    if (idx != last)
    {
        std::swap(mEntities[idx], mEntities[last]);
        mIndexById[mEntities[idx].id] = idx;
    }

    mEntities.pop_back();
    mIndexById.erase(it);
    return true;
}

Scene::Entity* Scene::get(EntityId id)
{
    auto it = mIndexById.find(id);
    if (it == mIndexById.end())
        return nullptr;
    return &mEntities[it->second];
}

const Scene::Entity* Scene::get(EntityId id) const
{
    auto it = mIndexById.find(id);
    if (it == mIndexById.end())
        return nullptr;
    return &mEntities[it->second];
}

void Scene::drawAll(Shader& shader) const
{
    for (const auto& e : mEntities)
    {
        if (!e.visible || !e.model) continue;
e.model->draw(shader, e.tr.pos, e.tr.rotDeg, e.tr.scale);
    }
}

void Scene::drawAllDepth(Shader& depthShader) const
{
    for (const auto& e : mEntities)
    {
        if (!e.visible || !e.castsShadow || !e.model) continue;
        e.model->drawDepth(depthShader, e.tr.pos, e.tr.rotDeg, e.tr.scale);
    }
}
