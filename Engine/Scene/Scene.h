#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>

// Forward declarations (keep compile times low)
class Shader;
class OBJModel;

class Scene
{
public:
    using EntityId = std::uint32_t;

    struct Transform
    {
        glm::vec3 pos    = {0.0f, 0.0f, 0.0f};
        glm::vec3 rotDeg = {0.0f, 0.0f, 0.0f}; // stored for later; not used by OBJModel::draw(pos, scale)
        glm::vec3 scale  = {1.0f, 1.0f, 1.0f};
    };

    struct Entity
    {
        EntityId    id = 0;
        std::string name;

        OBJModel*   model = nullptr; // points into the asset cache

        Transform   tr;

        bool        visible     = true;
        bool        castsShadow = true;
    };

public:
    Scene();
    ~Scene();

    // ---- Asset loading (cached) ----
    // Loads an OBJ once per unique path; returns a stable pointer owned by the Scene.
    OBJModel* getOrLoadOBJ(const std::string& objPath);

    // ---- Entity management ----
    EntityId createEntity(const std::string& name);
    EntityId createEntityOBJ(const std::string& name,
                             const std::string& objPath);

    bool destroyEntity(EntityId id);

    Entity*       get(EntityId id);
    const Entity* get(EntityId id) const;

    // Useful for UI lists
    std::vector<Entity>&       entities()       { return mEntities; }
    const std::vector<Entity>& entities() const { return mEntities; }

    // ---- Rendering helpers ----
    // Uses your existing OBJModel API: draw(shader, pos, scale) / drawDepth(shader, pos, scale)
    void drawAll(Shader& shader) const;
    void drawAllDepth(Shader& depthShader) const;

private:
    EntityId nextId_();

private:
    // Asset cache
    std::unordered_map<std::string, std::unique_ptr<OBJModel>> mOBJCache;

    // Entities
    std::vector<Entity> mEntities;

    // Fast lookup: id -> index in mEntities
    std::unordered_map<EntityId, std::size_t> mIndexById;

    EntityId mNextId = 1;
};
