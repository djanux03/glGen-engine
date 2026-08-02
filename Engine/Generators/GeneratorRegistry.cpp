#include "GeneratorRegistry.h"

#include "GeneratorSchema.h"
#include "MeshBuilder.h"
#include "MeshValidate.h"

#include <algorithm>

namespace gen {

GeneratorRegistry &GeneratorRegistry::instance() {
  static GeneratorRegistry registry;
  static bool populated = false;
  if (!populated) {
    // Set BEFORE registering: the built-ins call instance() themselves, and
    // without this the recursion would re-enter registration forever.
    populated = true;
    registerBuiltinGenerators();
  }
  return registry;
}

void GeneratorRegistry::registerGenerator(GeneratorInfo info) {
  if (info.name.empty() || !info.build)
    return;
  auto it = std::find_if(mGenerators.begin(), mGenerators.end(),
                         [&](const GeneratorInfo &g) { return g.name == info.name; });
  if (it != mGenerators.end())
    *it = std::move(info); // re-registration replaces, so tests can override
  else
    mGenerators.push_back(std::move(info));
}

const GeneratorInfo *GeneratorRegistry::find(const std::string &name) const {
  for (const auto &g : mGenerators)
    if (g.name == name)
      return &g;
  return nullptr;
}

std::vector<std::string> GeneratorRegistry::names() const {
  std::vector<std::string> out;
  out.reserve(mGenerators.size());
  for (const auto &g : mGenerators)
    out.push_back(g.name);
  std::sort(out.begin(), out.end());
  return out;
}

GenResult GeneratorRegistry::run(const std::string &generator,
                                 nlohmann::json params, uint32_t seed) const {
  GenResult result;
  const GeneratorInfo *info = find(generator);
  if (!info) {
    result.error = "unknown generator '" + generator + "'";
    return result;
  }

  if (!validateParams(info->schema, params, result.warnings, result.error))
    return result;

  const Params typed(params);
  std::unique_ptr<MeshData> mesh =
      info->build(typed, seed, result.warnings, result.error);

  if (!mesh) {
    if (result.error.empty())
      result.error = "generator '" + generator + "' returned no mesh";
    return result;
  }
  if (!result.error.empty())
    return result;

  for (const auto &sm : mesh->submeshes) {
    // Non-indexed submeshes are a legal MeshData shape (the file parsers emit
    // them), so count both forms rather than assuming indices exist.
    result.triangles +=
        (sm.indices.empty() ? sm.vertices.size() : sm.indices.size()) / 3;
  }
  if (result.triangles == 0) {
    result.error = "generator '" + generator + "' produced no geometry";
    return result;
  }

  // Validation lives HERE rather than in each generator so every caller --
  // recipes, Lua, the command port, MCP -- inherits it, and so a new generator
  // cannot forget to do it (AI_ASSET_PIPELINE_PLAN.md Phase 6).
  MeshChecks checks;
  checks.polyBudget = info->polyBudget;
  // import.gltf can legitimately produce anything; the caller chose the file
  // and the recenter mode, and second-guessing that is noise.
  checks.expectPivotAtBase = generator.rfind("import.", 0) != 0;
  const std::vector<std::string> issues = validateMesh(*mesh, checks);
  result.warnings.insert(result.warnings.end(), issues.begin(), issues.end());

  // Non-finite geometry is the one thing that must not reach the renderer: it
  // corrupts the acceleration-structure build and crashes far from the cause.
  if (meshHasNonFiniteData(*mesh)) {
    result.error = "generator '" + generator +
                   "' produced NaN or infinite vertex data; refusing to "
                   "register it";
    return result;
  }

  if (mesh->sourcePath.empty())
    mesh->sourcePath = "gen://" + generator;
  result.mesh = std::move(mesh);
  return result;
}

} // namespace gen
