#include "AssetRecipe.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace gen {
namespace {

using json = nlohmann::json;

// FNV-1a 64. Not cryptographic -- this is a cache key, and a collision would
// mean two different recipes sharing a mesh, which the 64-bit width makes
// vanishingly unlikely at any realistic project size.
uint64_t fnv1a(const std::string &s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string toHex(uint64_t v) {
  static const char *digits = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[i] = digits[v & 0xF];
    v >>= 4;
  }
  return out;
}

} // namespace

std::string recipeHash(const AssetRecipe &recipe) {
  // nlohmann's default object type keeps keys sorted, so dump() is already
  // canonical -- the same logical recipe always hashes identically regardless
  // of the key order it was written in.
  json canonical{{"generator", recipe.generator},
                 {"seed", recipe.seed},
                 {"params", recipe.params},
                 {"material", recipe.material}};
  return toHex(fnv1a(canonical.dump()));
}

std::string assetIdFor(const AssetRecipe &recipe) {
  // See the header for why named and anonymous recipes are addressed
  // differently.
  return "gen://" + recipe.generator + "/" +
         (recipe.id.empty() ? recipeHash(recipe) : recipe.id);
}

bool parseRecipe(const json &j, AssetRecipe &out, std::string &error) {
  if (!j.is_object()) {
    error = "recipe must be a JSON object";
    return false;
  }
  if (!j.contains("generator") || !j["generator"].is_string()) {
    error = "recipe is missing a string \"generator\" field";
    return false;
  }

  out.generator = j["generator"].get<std::string>();
  out.id = j.value("id", std::string{});

  if (j.contains("seed")) {
    if (!j["seed"].is_number()) {
      error = "recipe \"seed\" must be a number";
      return false;
    }
    out.seed = j["seed"].get<uint32_t>();
  }

  auto takeObject = [&](const char *key, json &dst) -> bool {
    if (!j.contains(key) || j[key].is_null()) {
      dst = json::object();
      return true;
    }
    if (!j[key].is_object()) {
      error = std::string("recipe \"") + key + "\" must be an object";
      return false;
    }
    dst = j[key];
    return true;
  };
  if (!takeObject("params", out.params))
    return false;
  if (!takeObject("material", out.material))
    return false;
  if (!takeObject("provenance", out.provenance))
    return false;

  return true;
}

bool loadRecipeFile(const std::string &path, AssetRecipe &out,
                    std::string &error) {
  std::ifstream in(path);
  if (!in.is_open()) {
    error = "cannot open recipe '" + path + "'";
    return false;
  }
  json j;
  try {
    in >> j;
  } catch (const std::exception &e) {
    error = "invalid JSON in '" + path + "': " + e.what();
    return false;
  }
  if (!parseRecipe(j, out, error)) {
    error = "in '" + path + "': " + error;
    return false;
  }
  out.sourceFile = path;
  return true;
}

json recipeToJson(const AssetRecipe &recipe) {
  json j;
  if (!recipe.id.empty())
    j["id"] = recipe.id;
  j["generator"] = recipe.generator;
  j["seed"] = recipe.seed;
  j["params"] = recipe.params;
  if (!recipe.material.empty())
    j["material"] = recipe.material;
  if (!recipe.provenance.empty())
    j["provenance"] = recipe.provenance;
  return j;
}

bool writeRecipeFile(const std::string &path, const AssetRecipe &recipe) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path p(path);
  if (p.has_parent_path())
    fs::create_directories(p.parent_path(), ec);
  std::ofstream out(path);
  if (!out.is_open())
    return false;
  out << recipeToJson(recipe).dump(2) << "\n";
  return out.good();
}

std::vector<AssetRecipe> loadRecipeDirectory(const std::string &directory,
                                             std::vector<std::string> &errors) {
  namespace fs = std::filesystem;
  std::vector<AssetRecipe> out;
  std::error_code ec;
  if (!fs::is_directory(directory, ec))
    return out;

  // Sorted, so load order (and therefore any id collision resolution) is
  // deterministic rather than filesystem-dependent.
  std::vector<std::string> files;
  for (const auto &entry : fs::directory_iterator(directory, ec)) {
    if (!entry.is_regular_file())
      continue;
    if (entry.path().extension() != ".json")
      continue;
    files.push_back(entry.path().string());
  }
  std::sort(files.begin(), files.end());

  for (const std::string &file : files) {
    AssetRecipe recipe;
    std::string error;
    if (loadRecipeFile(file, recipe, error))
      out.push_back(std::move(recipe));
    else
      errors.push_back(error); // skip, don't abort the whole project load
  }
  return out;
}

} // namespace gen
