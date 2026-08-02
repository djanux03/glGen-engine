#pragma once
// AssetLibrary.h — recipes in, live engine assets out.
//
// Owns the loop that turns an AssetRecipe into something the renderer can
// draw: run the generator, generate and attach any procedural textures, and
// register the result with the AssetManager under the recipe's asset id. Also
// owns hot reload -- editing a recipe file on disk regenerates the mesh and
// swaps it in place under the SAME asset id, so entities already referencing
// it just change shape.
//
// That last property is the entire authoring loop for the pipeline: edit
// JSON, see the tree change. It works because named recipes have stable ids
// (see AssetRecipe.h) and AssetManager::replaceMeshData bumps a content
// version the render system polls (see AI_ASSET_PIPELINE_PLAN.md Phase 0).

#include "AssetRecipe.h"
#include "MeshData.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class AssetManager;

namespace gen {

struct ResolveResult {
  std::string assetId;
  std::vector<std::string> warnings;
  std::string error;
  size_t triangles = 0;
  bool regenerated = false; // false = served from the already-registered mesh
  bool ok() const { return error.empty() && !assetId.empty(); }
};

class AssetLibrary {
public:
  // `recipeDir` is watched by pollHotReload(); pass empty to disable file
  // watching (resolve() still works for in-memory recipes).
  void initialize(AssetManager &assets, std::string recipeDir);

  // Generates and registers `recipe` if it is not already registered with
  // identical content, otherwise returns the existing asset id unchanged.
  ResolveResult resolve(const AssetRecipe &recipe);

  // Loads and resolves every *.json in the recipe directory. Parse failures
  // and generator errors land in `errors`; one bad recipe never aborts the
  // rest. Returns the number successfully resolved.
  int loadAll(std::vector<std::string> &errors);

  // Re-reads recipe files whose modification time changed, regenerates them,
  // and swaps the geometry in place. Returns human-readable messages (the
  // same shape AssetManager::pollHotReload uses, so the editor console can
  // print both alike). Cheap to call every frame.
  std::vector<std::string> pollHotReload();

  // Deterministic facts about a registered asset. These are what a regression
  // check compares: a triangle count or a bounding box that moved names the
  // change exactly, where an image diff only says "something looks different".
  struct AssetInfo {
    bool valid = false;
    size_t triangles = 0;
    size_t vertices = 0;
    size_t submeshes = 0;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
  };
  AssetInfo info(const std::string &assetId) const;

  const AssetRecipe *findByRecipeId(const std::string &recipeId) const;
  // Asset id currently registered for a friendly recipe id, or empty.
  std::string assetIdOfRecipe(const std::string &recipeId) const;
  std::vector<std::string> recipeIds() const;
  size_t size() const { return mEntries.size(); }

private:
  struct Entry {
    AssetRecipe recipe;
    std::string contentHash;
    std::filesystem::file_time_type watchedTime{};
    size_t triangles = 0;
  };

  // Generates the mesh + textures for `recipe`. Separate from resolve() so
  // both the initial registration and a hot-reload regeneration share it.
  bool generate_(const AssetRecipe &recipe, ResolveResult &result,
                 std::unique_ptr<MeshData> &outMesh);

  AssetManager *mAssets = nullptr;
  std::string mRecipeDir;
  // assetId -> entry
  std::unordered_map<std::string, Entry> mEntries;
  // friendly recipe id -> assetId
  std::unordered_map<std::string, std::string> mByRecipeId;
};

} // namespace gen
