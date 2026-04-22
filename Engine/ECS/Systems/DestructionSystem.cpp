#include "ECS/Systems/DestructionSystem.h"

#include "Assets/AssetManager.h"
#include "Assets/FBXModel.h"
#include "Assets/OBJModel.h"
#include "Assets/UFBXModel.h"
#include "ECS/Components.h"
#include "ECS/Systems/PhysicsSystem.h"
#include "Terrain/TerrainSystem.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <string>

namespace {

constexpr const char *kShardAssetPrefix = "__destruct_shard_";

static void addShardFace(std::vector<OBJModel::VertexData> &verts,
                         const glm::vec3 &a, const glm::vec3 &b,
                         const glm::vec3 &c, const glm::vec3 &d) {
  const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
  verts.push_back({a, {0.0f, 0.0f}, n});
  verts.push_back({b, {1.0f, 0.0f}, n});
  verts.push_back({c, {1.0f, 1.0f}, n});
  verts.push_back({a, {0.0f, 0.0f}, n});
  verts.push_back({c, {1.0f, 1.0f}, n});
  verts.push_back({d, {0.0f, 1.0f}, n});
}

static std::vector<OBJModel::VertexData>
makeJaggedBoxShard(const glm::vec3 &halfSize, std::mt19937 &rng) {
  std::uniform_real_distribution<float> jitter(-0.22f, 0.22f);
  std::vector<glm::vec3> p = {
      {-halfSize.x, -halfSize.y, -halfSize.z},
      {halfSize.x, -halfSize.y, -halfSize.z},
      {halfSize.x, halfSize.y, -halfSize.z},
      {-halfSize.x, halfSize.y, -halfSize.z},
      {-halfSize.x, -halfSize.y, halfSize.z},
      {halfSize.x, -halfSize.y, halfSize.z},
      {halfSize.x, halfSize.y, halfSize.z},
      {-halfSize.x, halfSize.y, halfSize.z},
  };

  for (auto &v : p) {
    v.x += halfSize.x * jitter(rng);
    v.y += halfSize.y * jitter(rng);
    v.z += halfSize.z * jitter(rng);
  }

  std::vector<OBJModel::VertexData> verts;
  verts.reserve(36);
  addShardFace(verts, p[0], p[1], p[2], p[3]);
  addShardFace(verts, p[5], p[4], p[7], p[6]);
  addShardFace(verts, p[4], p[0], p[3], p[7]);
  addShardFace(verts, p[1], p[5], p[6], p[2]);
  addShardFace(verts, p[3], p[2], p[6], p[7]);
  addShardFace(verts, p[4], p[5], p[1], p[0]);
  return verts;
}

static bool sourceBounds(Registry &reg, Scene::EntityId entity,
                         glm::vec3 &minB, glm::vec3 &maxB) {
  if (!reg.has<MeshComponent>(entity))
    return false;

  const auto &mesh = reg.get<MeshComponent>(entity);
  if (mesh.objModel && mesh.objModel->getGlobalBounds(minB, maxB))
    return true;
  if (mesh.gltfModel && mesh.gltfModel->getGlobalBounds(minB, maxB))
    return true;
  if (mesh.ufbxModel && mesh.ufbxModel->getGlobalBounds(minB, maxB))
    return true;
  return false;
}

static MaterialOverrideComponent
makeShardMaterial(Registry &reg, Scene::EntityId source, int index) {
  if (reg.has<MaterialOverrideComponent>(source))
    return reg.get<MaterialOverrideComponent>(source);

  MaterialOverrideComponent mo;
  mo.material.id = "DestructionShard_" + std::to_string(source) + "_" +
                   std::to_string(index);
  const float warm = 0.48f + 0.04f * (float)(index % 3);
  mo.material.baseColor = glm::vec4(warm, warm * 0.78f, warm * 0.55f, 1.0f);
  mo.material.roughness = 0.86f;
  mo.material.metallic = 0.0f;
  mo.material.ao = 1.0f;
  return mo;
}

static bool isPendingDestroy(Registry &reg, Scene::EntityId entity) {
  return reg.has<LifecycleComponent>(entity) &&
         reg.get<LifecycleComponent>(entity).state ==
             EntityLifecycleState::PendingDestroy;
}

static bool isShardRuntimeAsset(const MeshComponent &mesh) {
  return mesh.type == MeshComponent::AssetType::OBJ &&
         !mesh.assetId.empty() &&
         mesh.assetId.rfind(kShardAssetPrefix, 0) == 0;
}

static void stripShardPhysics(Registry &reg, Scene::EntityId entity,
                              PhysicsSystem *physics) {
  if (reg.has<RigidbodyComponent>(entity)) {
    const auto &rb = reg.get<RigidbodyComponent>(entity);
    if (physics)
      physics->removeBody(rb.bodyID);
    reg.removeComponent<RigidbodyComponent>(entity);
  }
  if (reg.has<ColliderComponent>(entity))
    reg.removeComponent<ColliderComponent>(entity);
}

static void queueShardDestroy(Scene &scene, Registry &reg,
                              Scene::EntityId entity, PhysicsSystem *physics) {
  if (entity == 0 || isPendingDestroy(reg, entity))
    return;

  if (reg.has<MeshComponent>(entity)) {
    auto &mesh = reg.get<MeshComponent>(entity);
    mesh.visible = false;
    mesh.castsShadow = false;
  }
  if (reg.has<DestructionShardComponent>(entity)) {
    auto &shard = reg.get<DestructionShardComponent>(entity);
    shard.settled = true;
    shard.velocity = glm::vec3(0.0f);
    shard.angularVelocity = glm::vec3(0.0f);
  }

  stripShardPhysics(reg, entity, physics);
  scene.deleteEntity(entity);
}

} // namespace

std::vector<Scene::EntityId> DestructionSystem::fractureEntity(
    Scene &scene, AssetManager &assets, Scene::EntityId entity,
    const glm::vec3 &hitPosition, const glm::vec3 &impulseDirection,
    PhysicsSystem *physics) {
  std::vector<Scene::EntityId> spawned;
  Registry &reg = scene.registry();
  if (entity == 0 || !reg.has<TransformComponent>(entity) ||
      !reg.has<DestructibleComponent>(entity))
    return spawned;

  auto &destructible = reg.get<DestructibleComponent>(entity);
  if (!destructible.enabled || destructible.fractured)
    return spawned;

  const bool treeSource = reg.has<TreeComponent>(entity);

  const TransformComponent sourceTr = reg.get<TransformComponent>(entity);
  glm::vec3 minB(-0.5f), maxB(0.5f);
  if (!sourceBounds(reg, entity, minB, maxB)) {
    float radius = 1.0f;
    if (reg.has<BoundsComponent>(entity))
      radius = std::max(0.1f, reg.get<BoundsComponent>(entity).radius);
    minB = glm::vec3(-radius * 0.5f);
    maxB = glm::vec3(radius * 0.5f);
  }

  const glm::vec3 localSize =
      glm::max(maxB - minB, glm::vec3(0.15f));
  const glm::vec3 worldSize =
      glm::max(glm::abs(sourceTr.scale) * localSize, glm::vec3(0.15f));
  const int maxShardCount = treeSource ? 12 : 64;
  const int shardCount = std::clamp(destructible.shardCount, 1, maxShardCount);
  const float basePiece =
      std::max(0.08f, std::cbrt((worldSize.x * worldSize.y * worldSize.z) /
                                (float)shardCount) *
                          destructible.shardScale);

  std::mt19937 rng(entity * 9781u + 1337u);
  std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
  std::uniform_real_distribution<float> pos01(0.0f, 1.0f);
  std::uniform_real_distribution<float> scaleJitter(0.55f, 1.35f);
  std::uniform_real_distribution<float> rotJitter(-55.0f, 55.0f);

  glm::vec3 impulseDir = impulseDirection;
  if (glm::length(impulseDir) < 0.001f)
    impulseDir = glm::vec3(0.0f, 1.0f, 0.0f);
  impulseDir = glm::normalize(impulseDir);

  if (physics && reg.has<RigidbodyComponent>(entity)) {
    auto &rb = reg.get<RigidbodyComponent>(entity);
    physics->removeBody(rb.bodyID);
  }

  destructible.fractured = true;
  if (destructible.hideOriginal && reg.has<MeshComponent>(entity))
    reg.get<MeshComponent>(entity).visible = false;
  if (reg.has<RigidbodyComponent>(entity))
    reg.removeComponent<RigidbodyComponent>(entity);
  if (reg.has<ColliderComponent>(entity))
    reg.removeComponent<ColliderComponent>(entity);
  if (destructible.hideOriginal && reg.has<LifecycleComponent>(entity))
    reg.get<LifecycleComponent>(entity).state = EntityLifecycleState::Disabled;

  const glm::mat4 sourceM = sourceTr.getMatrix();
  for (int i = 0; i < shardCount; ++i) {
    glm::vec3 local(
        minB.x + pos01(rng) * localSize.x,
        minB.y + pos01(rng) * localSize.y,
        minB.z + pos01(rng) * localSize.z);
    glm::vec3 worldPos = glm::vec3(sourceM * glm::vec4(local, 1.0f));
    glm::vec3 shardHalfSize(
        basePiece * scaleJitter(rng) * 0.55f,
        basePiece * scaleJitter(rng) * 0.55f,
        basePiece * scaleJitter(rng) * 0.55f);
    shardHalfSize = glm::min(shardHalfSize, worldSize * 0.45f);
    shardHalfSize = glm::max(shardHalfSize, glm::vec3(0.04f));

    auto model = std::make_unique<OBJModel>();
    const std::string assetId = "__destruct_shard_" + std::to_string(entity) +
                                "_" + std::to_string(i);
    model->loadFromVertices(makeJaggedBoxShard(shardHalfSize, rng), assetId);
    OBJHandle handle = assets.registerRuntimeOBJ(assetId, std::move(model));
    OBJModel *obj = assets.getOBJ(handle);
    if (!handle.valid() || !obj)
      continue;

    Scene::EntityId shard = scene.createEmptyEntity(
        "Shard_" + std::to_string(entity) + "_" + std::to_string(i));
    reg.emplace<TransientComponent>(shard);
    auto &tr = reg.get<TransformComponent>(shard);
    tr.position = worldPos;
    tr.rotation =
        sourceTr.rotation + glm::vec3(rotJitter(rng), rotJitter(rng),
                                     rotJitter(rng));
    tr.scale = glm::vec3(1.0f);

    auto &mesh = reg.emplace<MeshComponent>(shard, obj, true, true);
    mesh.assetId = assetId;
    mesh.objHandle = handle;
    mesh.type = MeshComponent::AssetType::OBJ;

    auto &bounds = reg.emplace<BoundsComponent>(shard);
    bounds.radius = glm::length(shardHalfSize);

    reg.emplace<MaterialOverrideComponent>(shard) =
        makeShardMaterial(reg, entity, i);

    auto &rb = reg.emplace<RigidbodyComponent>(shard);
    rb.type = RigidbodyComponent::Type::Dynamic;
    rb.mass = std::max(0.05f, destructible.shardScale * 0.45f);
    rb.friction = 0.65f;
    rb.restitution = 0.12f;

    auto &col = reg.emplace<ColliderComponent>(shard);
    col.shape = ColliderComponent::Shape::Box;
    col.dimensions = shardHalfSize * 2.0f;

    glm::vec3 outward = worldPos - hitPosition;
    if (glm::length(outward) < 0.001f)
      outward = glm::vec3(unit(rng), 0.25f + std::abs(unit(rng)), unit(rng));
    outward = glm::normalize(outward);
    const float force = destructible.explosionForce * scaleJitter(rng);
    rb.pendingImpulse =
        (outward * 0.75f + impulseDir * 0.35f +
         glm::vec3(0.0f, destructible.upwardImpulse * 0.08f, 0.0f)) *
        force;

    auto &preview = reg.emplace<DestructionShardComponent>(shard);
    preview.velocity =
        (rb.pendingImpulse / std::max(0.1f, rb.mass)) * 0.18f;
    preview.angularVelocity =
        glm::vec3(unit(rng), unit(rng), unit(rng)) * 220.0f;
    preview.editorPreview = (physics == nullptr);
    preview.lifetime = physics ? (treeSource ? 5.0f : 8.0f) : 10.0f;
    preview.shadowLifetime = treeSource ? 0.25f : 0.75f;
    preview.cleanupDelay = treeSource ? 0.8f : 1.2f;
    if (!physics)
      rb.pendingImpulse = glm::vec3(0.0f);

    spawned.push_back(shard);
  }

  return spawned;
}

void DestructionSystem::preparePendingDestroy(Scene &scene, AssetManager &assets,
                                              PhysicsSystem *physics) {
  Registry &reg = scene.registry();
  for (auto entity : reg.viewAll<DestructionShardComponent, LifecycleComponent>()) {
    if (!isPendingDestroy(reg, entity))
      continue;

    stripShardPhysics(reg, entity, physics);

    if (!reg.has<MeshComponent>(entity))
      continue;

    auto &mesh = reg.get<MeshComponent>(entity);
    mesh.visible = false;
    mesh.castsShadow = false;
    if (isShardRuntimeAsset(mesh))
      assets.releaseOBJ(mesh.assetId);

    mesh.objModel = nullptr;
    mesh.objHandle = {};
    mesh.assetId.clear();
    mesh.type = MeshComponent::AssetType::None;
  }
}

void DestructionSystem::updateRuntime(Scene &scene, float dt,
                                      PhysicsSystem *physics,
                                      const TerrainSystem *terrain) {
  if (dt <= 0.0f)
    return;

  Registry &reg = scene.registry();
  std::vector<Scene::EntityId> toDestroy;
  for (auto entity : reg.viewAll<TransformComponent, DestructionShardComponent>()) {
    if (isPendingDestroy(reg, entity))
      continue;

    auto &shard = reg.get<DestructionShardComponent>(entity);
    if (shard.editorPreview)
      continue;

    auto &tr = reg.get<TransformComponent>(entity);
    shard.age += dt;

    float linearSpeed = glm::length(shard.velocity);
    float angularSpeed = glm::length(shard.angularVelocity);
    if (reg.has<RigidbodyComponent>(entity)) {
      const auto &rb = reg.get<RigidbodyComponent>(entity);
      shard.velocity = rb.linearVelocity;
      linearSpeed = glm::length(rb.linearVelocity);
      angularSpeed = 0.0f;
    }

    if (reg.has<MeshComponent>(entity) && shard.age >= shard.shadowLifetime) {
      reg.get<MeshComponent>(entity).castsShadow = false;
    }

    bool hasGround = false;
    float groundY = 0.0f;
    if (terrain && terrain->isEnabled() &&
        terrain->isChunkLoadedAt(tr.position.x, tr.position.z)) {
      groundY = terrain->getHeightAt(tr.position.x, tr.position.z) + 0.03f;
      hasGround = true;
    } else if (tr.position.y <= 0.0f) {
      groundY = 0.0f;
      hasGround = true;
    }

    const bool nearGround = hasGround && tr.position.y <= groundY + 0.08f;
    if (!shard.settled && nearGround && linearSpeed < 0.18f &&
        angularSpeed < 6.0f) {
      shard.settled = true;
      shard.settledTime = 0.0f;
      if (tr.position.y < groundY)
        tr.position.y = groundY;
    }

    if (shard.settled) {
      shard.settledTime += dt;
      shard.velocity = glm::vec3(0.0f);
      shard.angularVelocity = glm::vec3(0.0f);
      if (hasGround && tr.position.y < groundY)
        tr.position.y = groundY;
      stripShardPhysics(reg, entity, physics);

      if (shard.settledTime >= shard.cleanupDelay ||
          shard.age >= shard.lifetime) {
        toDestroy.push_back(entity);
      }
      continue;
    }

    if (shard.age >= shard.lifetime)
      toDestroy.push_back(entity);
  }

  for (auto entity : toDestroy)
    queueShardDestroy(scene, reg, entity, physics);
}

void DestructionSystem::updateEditorPreview(Scene &scene, float dt,
                                            const TerrainSystem *terrain) {
  if (dt <= 0.0f)
    return;

  const float step = std::min(dt, 1.0f / 30.0f);
  Registry &reg = scene.registry();
  for (auto entity : reg.viewAll<TransformComponent, DestructionShardComponent>()) {
    auto &preview = reg.get<DestructionShardComponent>(entity);
    if (!preview.editorPreview || preview.settled)
      continue;

    auto &tr = reg.get<TransformComponent>(entity);
    preview.age += step;
    preview.velocity.y -= 9.81f * step;
    tr.position += preview.velocity * step;
    tr.rotation += preview.angularVelocity * step;

    const float damping = std::pow(0.985f, step * 60.0f);
    preview.velocity.x *= damping;
    preview.velocity.z *= damping;
    preview.angularVelocity *= std::pow(0.965f, step * 60.0f);

    bool hasGround = false;
    float groundY = 0.0f;
    if (terrain && terrain->isEnabled() &&
        terrain->isChunkLoadedAt(tr.position.x, tr.position.z)) {
      groundY = terrain->getHeightAt(tr.position.x, tr.position.z) + 0.03f;
      hasGround = true;
    } else if (tr.position.y < 0.0f) {
      hasGround = true;
    }

    if (hasGround && tr.position.y <= groundY) {
      tr.position.y = groundY;
      if (std::abs(preview.velocity.y) > 1.2f) {
        preview.velocity.y = -preview.velocity.y * 0.22f;
        preview.velocity.x *= 0.72f;
        preview.velocity.z *= 0.72f;
        preview.angularVelocity *= 0.55f;
      } else {
        preview.velocity.y = 0.0f;
        preview.velocity.x *= 0.35f;
        preview.velocity.z *= 0.35f;
        preview.angularVelocity *= 0.35f;
      }
    }

    const float linearSpeed = glm::length(preview.velocity);
    const float angularSpeed = glm::length(preview.angularVelocity);
    if (preview.age >= preview.lifetime ||
        (hasGround && linearSpeed < 0.08f && angularSpeed < 2.0f)) {
      preview.velocity = glm::vec3(0.0f);
      preview.angularVelocity = glm::vec3(0.0f);
      preview.settled = true;
    }
  }
}
