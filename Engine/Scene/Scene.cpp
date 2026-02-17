#include "AssetManager.h"
#include "Scene.h"

#include "ECS/Components.h"
#include "FBXModel.h"
#include "OBJModel.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace {
using json = nlohmann::json;

std::string lifecycleToString(EntityLifecycleState s) {
  switch (s) {
  case EntityLifecycleState::Alive:
    return "Alive";
  case EntityLifecycleState::Disabled:
    return "Disabled";
  case EntityLifecycleState::PendingDestroy:
    return "PendingDestroy";
  }
  return "Alive";
}

EntityLifecycleState lifecycleFromString(const std::string &s) {
  if (s == "Disabled")
    return EntityLifecycleState::Disabled;
  if (s == "PendingDestroy")
    return EntityLifecycleState::PendingDestroy;
  return EntityLifecycleState::Alive;
}

void eraseChild(std::vector<uint32_t> &children, uint32_t child) {
  children.erase(std::remove(children.begin(), children.end(), child),
                 children.end());
}
} // namespace

Scene::Scene() = default;
Scene::~Scene() = default;

OBJModel *Scene::getOrLoadOBJ(const std::string &objPath) {
  if (!mAssets)
    return nullptr;
  auto h = mAssets->loadOBJ(objPath);
  if (!h.valid())
    return nullptr;
  return mAssets->getOBJ(h);
}

FBXModel *Scene::getOrLoadFBX(const std::string &path) {
  if (!mAssets)
    return nullptr;
  auto h = mAssets->loadGLTF(path);
  if (!h.valid())
    return nullptr;
  return mAssets->getGLTF(h);
}

Scene::EntityId Scene::spawnFromFile(const std::string &path) {
  if (path.empty())
    return 0;

  namespace fs = std::filesystem;

  std::string ext = fs::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });

  MeshComponent mesh;
  if (ext == ".obj") {
    if (!mAssets)
      return 0;
    auto h = mAssets->loadOBJ(path);
    OBJModel *model = mAssets->getOBJ(h);
    if (!h.valid() || !model)
      return 0;
    mesh = MeshComponent(model);
    mesh.objHandle = h;
  } else if (ext == ".fbx" || ext == ".gltf" || ext == ".glb") {
    if (!mAssets)
      return 0;
    auto h = mAssets->loadGLTF(path);
    FBXModel *model = mAssets->getGLTF(h);
    if (!h.valid() || !model)
      return 0;
    mesh = MeshComponent(model);
    mesh.gltfHandle = h;
  } else {
    return 0;
  }

  EntityId id = mRegistry.create();
  mRegistry.emplace<TransformComponent>(id);
  auto &mc = mRegistry.emplace<MeshComponent>(id, mesh);
  mc.assetId = path;
  mRegistry.emplace<BoundsComponent>(id, BoundsComponent{2.0f});
  mRegistry.emplace<LifecycleComponent>(id);
  mRegistry.emplace<HierarchyComponent>(id);

  // Extract filename from path for entity name
  std::string name = fs::path(path).stem().string();
  if (name.empty())
    name = "Entity";
  mRegistry.emplace<NameComponent>(id, name);

  return id;
}

Scene::EntityId Scene::createEmptyEntity(const std::string &name) {
  EntityId id = mRegistry.create();
  mRegistry.emplace<TransformComponent>(id);
  mRegistry.emplace<NameComponent>(id, name.empty() ? "Empty" : name);
  mRegistry.emplace<LifecycleComponent>(id);
  mRegistry.emplace<HierarchyComponent>(id);
  return id;
}

void Scene::disableEntity(EntityId id) {
  if (!mRegistry.has<LifecycleComponent>(id))
    mRegistry.emplace<LifecycleComponent>(id);
  auto &lc = mRegistry.get<LifecycleComponent>(id);
  if (lc.state == EntityLifecycleState::Alive)
    lc.state = EntityLifecycleState::Disabled;
}

void Scene::enableEntity(EntityId id) {
  if (!mRegistry.has<LifecycleComponent>(id))
    mRegistry.emplace<LifecycleComponent>(id);
  auto &lc = mRegistry.get<LifecycleComponent>(id);
  if (lc.state == EntityLifecycleState::Disabled)
    lc.state = EntityLifecycleState::Alive;
}

bool Scene::setParent(EntityId child, EntityId parent) {
  if (child == 0 || parent == 0 || child == parent)
    return false;
  if (!mRegistry.has<HierarchyComponent>(child) ||
      !mRegistry.has<HierarchyComponent>(parent))
    return false;

  clearParent(child);

  auto &childH = mRegistry.get<HierarchyComponent>(child);
  auto &parentH = mRegistry.get<HierarchyComponent>(parent);
  childH.parent = parent;
  parentH.children.push_back(child);
  return true;
}

void Scene::clearParent(EntityId child) {
  if (!mRegistry.has<HierarchyComponent>(child))
    return;
  auto &childH = mRegistry.get<HierarchyComponent>(child);
  if (childH.parent != 0 && mRegistry.has<HierarchyComponent>(childH.parent)) {
    auto &oldParent = mRegistry.get<HierarchyComponent>(childH.parent);
    eraseChild(oldParent.children, child);
  }
  childH.parent = 0;
}

void Scene::deleteEntity(EntityId id) {
  if (id == 0)
    return;
  if (!mRegistry.has<LifecycleComponent>(id))
    mRegistry.emplace<LifecycleComponent>(id);
  auto &lc = mRegistry.get<LifecycleComponent>(id);
  if (lc.state == EntityLifecycleState::PendingDestroy)
    return;
  lc.state = EntityLifecycleState::PendingDestroy;

  if (mRegistry.has<HierarchyComponent>(id)) {
    auto children = mRegistry.get<HierarchyComponent>(id).children;
    for (auto c : children)
      deleteEntity(c);
  }
}

void Scene::flushPendingDestroy() {
  std::vector<EntityId> toDestroy;
  for (auto e : mRegistry.view<LifecycleComponent>()) {
    auto &lc = mRegistry.get<LifecycleComponent>(e);
    if (lc.state == EntityLifecycleState::PendingDestroy)
      toDestroy.push_back(e);
  }

  for (EntityId e : toDestroy) {
    if (mRegistry.has<HierarchyComponent>(e)) {
      auto h = mRegistry.get<HierarchyComponent>(e);
      if (h.parent != 0 && mRegistry.has<HierarchyComponent>(h.parent)) {
        auto &p = mRegistry.get<HierarchyComponent>(h.parent);
        eraseChild(p.children, e);
      }
      for (auto c : h.children) {
        if (mRegistry.has<HierarchyComponent>(c)) {
          auto &ch = mRegistry.get<HierarchyComponent>(c);
          ch.parent = 0;
        }
      }
    }
    mRegistry.destroy(e);
  }
}

void Scene::clear() { mRegistry = Registry{}; }

bool Scene::saveToFile(const std::string &path) const {
  std::ofstream out(path);
  if (!out.is_open())
    return false;
  out << serializeToString();
  return true;
}

bool Scene::loadFromFile(const std::string &path) {
  std::ifstream in(path);
  if (!in.is_open())
    return false;

  std::stringstream ss;
  ss << in.rdbuf();
  return loadFromString(ss.str());
}

std::string Scene::serializeToString() const {
  json root;
  root["entities"] = json::array();

  auto &reg = const_cast<Registry &>(mRegistry);
  for (auto e : reg.view<TransformComponent>()) {
    json ent;
    ent["id"] = e;

    if (reg.has<NameComponent>(e))
      ent["name"] = reg.get<NameComponent>(e).name;

    const auto &tr = reg.get<TransformComponent>(e);
    ent["transform"] = {{"position", {tr.position.x, tr.position.y, tr.position.z}},
                        {"rotation", {tr.rotation.x, tr.rotation.y, tr.rotation.z}},
                        {"scale", {tr.scale.x, tr.scale.y, tr.scale.z}}};

    if (reg.has<LifecycleComponent>(e)) {
      ent["lifecycle"] = lifecycleToString(reg.get<LifecycleComponent>(e).state);
    }

    if (reg.has<HierarchyComponent>(e)) {
      const auto &h = reg.get<HierarchyComponent>(e);
      ent["hierarchy"] = {{"parent", h.parent}, {"children", h.children}};
    }

    if (reg.has<MeshComponent>(e)) {
      const auto &mc = reg.get<MeshComponent>(e);
      std::string type = "None";
      if (mc.type == MeshComponent::AssetType::OBJ)
        type = "OBJ";
      else if (mc.type == MeshComponent::AssetType::FBX)
        type = "FBX";
      ent["mesh"] = {{"type", type},
                     {"assetId", mc.assetId},
                     {"visible", mc.visible},
                     {"castsShadow", mc.castsShadow}};
    }

    if (reg.has<PhysicsComponent>(e)) {
      const auto &ph = reg.get<PhysicsComponent>(e);
      ent["physics"] = {{"velocity", {ph.velocity.x, ph.velocity.y, ph.velocity.z}},
                        {"gravity", ph.gravity},
                        {"onGround", ph.onGround}};
    }

    if (reg.has<CameraComponent>(e)) {
      const auto &cam = reg.get<CameraComponent>(e);
      ent["camera"] = {{"fov", cam.fov},
                       {"front", {cam.front.x, cam.front.y, cam.front.z}},
                       {"up", {cam.up.x, cam.up.y, cam.up.z}},
                       {"yaw", cam.yaw},
                       {"pitch", cam.pitch},
                       {"isPrimary", cam.isPrimary}};
    }

    root["entities"].push_back(std::move(ent));
  }

  return root.dump(2);
}

bool Scene::loadFromString(const std::string &jsonText) {
  json root = json::parse(jsonText, nullptr, false);
  if (root.is_discarded())
    return false;
  if (!root.contains("entities") || !root["entities"].is_array())
    return false;

  clear();

  std::unordered_map<uint32_t, uint32_t> idMap;
  struct PendingParent {
    uint32_t child = 0;
    uint32_t oldParent = 0;
  };
  std::vector<PendingParent> pendingParents;

  for (const auto &ent : root["entities"]) {
    const uint32_t oldId = ent.value("id", 0u);
    EntityId id = mRegistry.create();
    idMap[oldId] = id;

    mRegistry.emplace<TransformComponent>(id);
    auto &tr = mRegistry.get<TransformComponent>(id);
    if (ent.contains("transform")) {
      const auto &t = ent["transform"];
      auto p = t.value("position", std::vector<float>{0, 0, 0});
      auto r = t.value("rotation", std::vector<float>{0, 0, 0});
      auto s = t.value("scale", std::vector<float>{1, 1, 1});
      if (p.size() == 3)
        tr.position = {p[0], p[1], p[2]};
      if (r.size() == 3)
        tr.rotation = {r[0], r[1], r[2]};
      if (s.size() == 3)
        tr.scale = {s[0], s[1], s[2]};
    }

    mRegistry.emplace<NameComponent>(id, ent.value("name", "Entity"));

    auto &lc = mRegistry.emplace<LifecycleComponent>(id);
    lc.state = lifecycleFromString(ent.value("lifecycle", "Alive"));

    auto &hc = mRegistry.emplace<HierarchyComponent>(id);
    if (ent.contains("hierarchy")) {
      hc.parent = ent["hierarchy"].value("parent", 0u);
      pendingParents.push_back({id, hc.parent});
      hc.parent = 0;
    }

    if (ent.contains("mesh")) {
      const auto &m = ent["mesh"];
      const std::string type = m.value("type", "None");
      const std::string assetId = m.value("assetId", "");
      const bool visible = m.value("visible", true);
      const bool castsShadow = m.value("castsShadow", true);

      if (!assetId.empty()) {
        if (type == "OBJ") {
          if (!mAssets)
            continue;
          auto h = mAssets->loadOBJ(assetId);
          if (OBJModel *obj = mAssets->getOBJ(h)) {
            auto &mc = mRegistry.emplace<MeshComponent>(id, obj, visible, castsShadow);
            mc.assetId = assetId;
            mc.objHandle = h;
          }
        } else if (type == "FBX") {
          if (!mAssets)
            continue;
          auto h = mAssets->loadGLTF(assetId);
          if (FBXModel *fbx = mAssets->getGLTF(h)) {
            auto &mc = mRegistry.emplace<MeshComponent>(id, fbx, visible, castsShadow);
            mc.assetId = assetId;
            mc.gltfHandle = h;
          }
        }
      }
    }

    if (ent.contains("physics")) {
      auto &ph = mRegistry.emplace<PhysicsComponent>(id);
      const auto &p = ent["physics"];
      auto vel = p.value("velocity", std::vector<float>{0, 0, 0});
      if (vel.size() == 3)
        ph.velocity = {vel[0], vel[1], vel[2]};
      ph.gravity = p.value("gravity", 0.01f);
      ph.onGround = p.value("onGround", false);
    }

    if (ent.contains("camera")) {
      auto &cam = mRegistry.emplace<CameraComponent>(id);
      const auto &c = ent["camera"];
      cam.fov = c.value("fov", 50.0f);
      auto front = c.value("front", std::vector<float>{0, 0, -1});
      auto up = c.value("up", std::vector<float>{0, 1, 0});
      if (front.size() == 3)
        cam.front = {front[0], front[1], front[2]};
      if (up.size() == 3)
        cam.up = {up[0], up[1], up[2]};
      cam.yaw = c.value("yaw", -90.0f);
      cam.pitch = c.value("pitch", 0.0f);
      cam.isPrimary = c.value("isPrimary", true);
    }
  }

  for (const auto &pp : pendingParents) {
    auto it = idMap.find(pp.oldParent);
    if (it != idMap.end())
      setParent(pp.child, it->second);
  }

  return true;
}
