#pragma once
// AssetRecipe.h — the unit of authorship in the AI-first pipeline.
//
// A recipe is a ~1 KB JSON document naming a generator and its parameters. It
// is the thing that gets version-controlled, diffed and reviewed; the mesh is
// a build product regenerated from it on demand (AI_ASSET_PIPELINE_PLAN.md
// §2). That inversion is what makes AI-authored content reproducible:
// "regenerate every asset from source" is a loop over these files.
//
//   {
//     "id": "tree/oak_windswept",
//     "generator": "tree.v1",
//     "seed": 91733,
//     "params": { "height": 8.4, "branchLevels": 4 },
//     "material": { "bark": {...}, "foliage": {...} },
//     "provenance": { "prompt": "gnarled windswept oak", "author": "claude" }
//   }

#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace gen {

struct AssetRecipe {
  std::string id;        // friendly label, e.g. "tree/oak_windswept"
  std::string generator; // registry name, e.g. "tree.v1"
  uint32_t seed = 0;
  nlohmann::json params = nlohmann::json::object();
  // Per-material-slot texture recipes, keyed by the slot name the generator
  // uses ("bark", "foliage", ...). Each value is itself {generator, params}.
  nlohmann::json material = nlohmann::json::object();
  // Free-form record of where this came from (prompt, author, date).
  // Deliberately EXCLUDED from the content hash -- editing a comment must not
  // invalidate a cached mesh.
  nlohmann::json provenance = nlohmann::json::object();

  std::string sourceFile; // set when loaded from disk; not part of the hash
};

// Stable 64-bit hash (hex) of everything that determines what the asset LOOKS
// LIKE: generator, seed, params, material. `id`, `provenance` and
// `sourceFile` are excluded, so renaming or re-annotating a recipe does not
// force a regeneration.
std::string recipeHash(const AssetRecipe &recipe);

// The AssetManager id this recipe registers under. The rule is:
//
//   NAMED recipe (id set)  -> "gen://<generator>/<id>"    STABLE identity
//   ANONYMOUS recipe       -> "gen://<generator>/<hash>"  content-addressed
//
// Both halves are needed, for opposite reasons. A named recipe is an authored
// asset that entities reference and that must survive editing: if its id moved
// with its content hash, saving a tweak to oak.json would mint a NEW asset and
// every tree already placed in the scene would keep the old geometry (or
// vanish), which defeats hot reload entirely. An anonymous recipe -- an AI
// asking for "a rock with these parameters" -- has no identity to preserve, so
// hashing it deduplicates repeat requests automatically.
std::string assetIdFor(const AssetRecipe &recipe);

// Parsing. Returns false with `error` set on malformed JSON or a missing
// required field ("generator").
bool parseRecipe(const nlohmann::json &j, AssetRecipe &out, std::string &error);
bool loadRecipeFile(const std::string &path, AssetRecipe &out,
                    std::string &error);

nlohmann::json recipeToJson(const AssetRecipe &recipe);
bool writeRecipeFile(const std::string &path, const AssetRecipe &recipe);

// Loads every *.json in `directory` (non-recursive). Files that fail to parse
// are reported in `errors` and skipped rather than aborting the whole load --
// one bad recipe should not take the project down.
std::vector<AssetRecipe> loadRecipeDirectory(const std::string &directory,
                                             std::vector<std::string> &errors);

} // namespace gen
