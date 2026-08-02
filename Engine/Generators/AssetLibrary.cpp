#include "AssetLibrary.h"

#include "AssetManager.h"
#include "GeneratorRegistry.h"
#include "TextureGen.h"

#include <algorithm>

namespace gen {
namespace {

// A per-material-slot seed derived from the recipe seed and the slot name, so
// bark and foliage on the same tree get decorrelated noise while both stay
// deterministic.
uint32_t slotSeed(uint32_t base, const std::string &slot) {
  uint32_t h = base ^ 0x9E3779B9u;
  for (unsigned char c : slot) {
    h ^= c;
    h *= 16777619u;
  }
  return h ? h : 1u;
}

} // namespace

void AssetLibrary::initialize(AssetManager &assets, std::string recipeDir) {
  mAssets = &assets;
  mRecipeDir = std::move(recipeDir);
}

bool AssetLibrary::generate_(const AssetRecipe &recipe, ResolveResult &result,
                             std::unique_ptr<MeshData> &outMesh) {
  GenResult gr =
      GeneratorRegistry::instance().run(recipe.generator, recipe.params, recipe.seed);
  result.warnings.insert(result.warnings.end(), gr.warnings.begin(),
                         gr.warnings.end());
  if (!gr.ok()) {
    result.error = gr.error;
    return false;
  }
  result.triangles = gr.triangles;

  // Procedural textures, keyed by the material id each generator gave its
  // submeshes ("bark", "foliage", "rock", ...). A recipe naming a slot the
  // mesh doesn't have is a warning, not an error -- it usually means a
  // parameter change moved the asset to a different canopy style.
  if (recipe.material.is_object() && !recipe.material.empty()) {
    std::vector<std::string> matched;
    for (auto &sm : gr.mesh->submeshes) {
      const auto it = recipe.material.find(sm.material.id);
      if (it == recipe.material.end() || !it->is_object())
        continue;
      const std::string texGen = it->value("generator", std::string{});
      if (texGen.empty()) {
        result.warnings.push_back("material slot '" + sm.material.id +
                                  "' has no \"generator\"");
        continue;
      }
      TextureSet set;
      std::string texError;
      const nlohmann::json texParams =
          it->contains("params") ? (*it)["params"] : nlohmann::json::object();
      if (!generateTextureSet(texGen, texParams,
                              slotSeed(recipe.seed, sm.material.id),
                              sm.material.id, set, result.warnings, texError)) {
        // A bad texture must not lose the geometry -- the mesh still renders
        // with its material's flat base colour.
        result.warnings.push_back("material slot '" + sm.material.id +
                                  "': " + texError + " (using flat colour)");
        continue;
      }
      applyTextureSet(*gr.mesh, sm.material, std::move(set));
      matched.push_back(sm.material.id);
    }
    for (auto it = recipe.material.begin(); it != recipe.material.end(); ++it) {
      if (std::find(matched.begin(), matched.end(), it.key()) == matched.end())
        result.warnings.push_back("material slot '" + it.key() +
                                  "' does not match any submesh of this "
                                  "generator's output and was ignored");
    }
  }

  outMesh = std::move(gr.mesh);
  return true;
}

ResolveResult AssetLibrary::resolve(const AssetRecipe &recipe) {
  ResolveResult result;
  if (!mAssets) {
    result.error = "AssetLibrary::initialize() was never called";
    return result;
  }
  if (recipe.generator.empty()) {
    result.error = "recipe has no generator";
    return result;
  }

  const std::string assetId = assetIdFor(recipe);
  const std::string hash = recipeHash(recipe);

  // Already registered with identical content: nothing to do. This is what
  // makes resolve() cheap to call repeatedly (every scatter layer, every
  // spawn) rather than something callers must cache around.
  const auto existing = mEntries.find(assetId);
  if (existing != mEntries.end() && existing->second.contentHash == hash) {
    result.assetId = assetId;
    result.triangles = existing->second.triangles;
    return result;
  }

  std::unique_ptr<MeshData> mesh;
  if (!generate_(recipe, result, mesh))
    return result;

  mesh->sourcePath = assetId;
  // registerMeshData replaces in place when the id already exists, keeping
  // outstanding handles valid and bumping the content version -- which is
  // precisely what a hot reload needs.
  if (!mAssets->registerMeshData(assetId, std::move(mesh)).valid()) {
    result.error = "AssetManager rejected '" + assetId + "'";
    return result;
  }

  Entry entry;
  entry.recipe = recipe;
  entry.contentHash = hash;
  entry.triangles = result.triangles;
  if (!recipe.sourceFile.empty()) {
    std::error_code ec;
    entry.watchedTime = std::filesystem::last_write_time(recipe.sourceFile, ec);
  }
  mEntries[assetId] = std::move(entry);
  if (!recipe.id.empty())
    mByRecipeId[recipe.id] = assetId;

  result.assetId = assetId;
  result.regenerated = true;
  return result;
}

int AssetLibrary::loadAll(std::vector<std::string> &errors) {
  if (mRecipeDir.empty())
    return 0;
  std::vector<AssetRecipe> recipes = loadRecipeDirectory(mRecipeDir, errors);
  int ok = 0;
  for (const AssetRecipe &recipe : recipes) {
    ResolveResult r = resolve(recipe);
    if (r.ok()) {
      ++ok;
      for (const std::string &w : r.warnings)
        errors.push_back("[warning] " + (recipe.id.empty() ? recipe.generator
                                                           : recipe.id) +
                         ": " + w);
    } else {
      errors.push_back((recipe.sourceFile.empty() ? recipe.generator
                                                  : recipe.sourceFile) +
                       ": " + r.error);
    }
  }
  return ok;
}

std::vector<std::string> AssetLibrary::pollHotReload() {
  std::vector<std::string> messages;
  if (mRecipeDir.empty())
    return messages;

  // Snapshot the file list first: resolving mutates mEntries, and iterating a
  // map while inserting into it is undefined.
  std::vector<std::pair<std::string, std::string>> watched; // assetId, file
  for (const auto &kv : mEntries)
    if (!kv.second.recipe.sourceFile.empty())
      watched.emplace_back(kv.first, kv.second.recipe.sourceFile);

  for (const auto &[assetId, file] : watched) {
    std::error_code ec;
    const auto now = std::filesystem::last_write_time(file, ec);
    if (ec)
      continue;
    auto it = mEntries.find(assetId);
    if (it == mEntries.end() || now == it->second.watchedTime)
      continue;
    it->second.watchedTime = now;

    AssetRecipe reloaded;
    std::string parseError;
    if (!loadRecipeFile(file, reloaded, parseError)) {
      messages.push_back("Recipe error: " + parseError);
      continue;
    }
    // Editing a named recipe's id, or its generator, re-homes it to a
    // different asset id. Say so, because the old asset stays registered and
    // anything already referencing it will not change.
    const std::string newAssetId = assetIdFor(reloaded);
    if (newAssetId != assetId)
      messages.push_back("Recipe '" + file + "' now resolves to '" +
                         newAssetId + "'; entities still referencing '" +
                         assetId + "' keep the old mesh");

    ResolveResult r = resolve(reloaded);
    if (!r.ok()) {
      messages.push_back("Recipe error: " + file + ": " + r.error);
      continue;
    }
    if (r.regenerated) {
      messages.push_back("Reloaded recipe: " + file + " -> " + r.assetId +
                         " (" + std::to_string(r.triangles) + " tris)");
      for (const std::string &w : r.warnings)
        messages.push_back("  warning: " + w);
    }
  }
  return messages;
}

AssetLibrary::AssetInfo AssetLibrary::info(const std::string &assetId) const {
  AssetInfo out;
  if (!mAssets)
    return out;
  const MeshData *data = mAssets->getOBJData(mAssets->findMeshData(assetId));
  if (!data)
    return out;

  out.valid = true;
  out.submeshes = data->submeshes.size();
  for (const auto &sm : data->submeshes) {
    out.vertices += sm.vertices.size();
    out.triangles +=
        (sm.indices.empty() ? sm.vertices.size() : sm.indices.size()) / 3;
  }
  data->getGlobalBounds(out.boundsMin, out.boundsMax);
  return out;
}

const AssetRecipe *AssetLibrary::findByRecipeId(const std::string &recipeId) const {
  const auto it = mByRecipeId.find(recipeId);
  if (it == mByRecipeId.end())
    return nullptr;
  const auto entry = mEntries.find(it->second);
  return entry == mEntries.end() ? nullptr : &entry->second.recipe;
}

std::string AssetLibrary::assetIdOfRecipe(const std::string &recipeId) const {
  const auto it = mByRecipeId.find(recipeId);
  return it == mByRecipeId.end() ? std::string{} : it->second;
}

std::vector<std::string> AssetLibrary::recipeIds() const {
  std::vector<std::string> out;
  out.reserve(mByRecipeId.size());
  for (const auto &kv : mByRecipeId)
    out.push_back(kv.first);
  std::sort(out.begin(), out.end());
  return out;
}

} // namespace gen
