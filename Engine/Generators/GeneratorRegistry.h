#pragma once
// GeneratorRegistry.h — the name -> generator lookup, and the one place that
// knows how to turn (generator, params, seed) into a MeshData.
//
// Generators are C++ and deterministic: same name + params + seed produces the
// same mesh, always. That is what makes a recipe a reproducible description of
// an asset rather than a hopeful one (AI_ASSET_PIPELINE_PLAN.md §2).
//
// A generator never sees raw caller input -- run() validates against the
// published schema first, so out-of-range values are clamped and malformed
// ones rejected before any geometry code runs.

#include "MeshData.h"
#include "json.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gen {

// Everything a generator reports back besides the mesh itself. Warnings are
// text, deliberately: they travel back to the caller (eventually an AI over
// MCP) alongside the rendered image, and text is what lets the model correct
// its own parameters instead of guessing from pixels.
struct GenResult {
  std::unique_ptr<MeshData> mesh;
  std::vector<std::string> warnings;
  std::string error; // non-empty = failure, `mesh` is null
  size_t triangles = 0;
  bool ok() const { return mesh != nullptr && error.empty(); }
};

struct GeneratorInfo {
  std::string name;        // "tree.v1" -- version is part of the identity
  std::string description; // becomes the MCP tool description
  nlohmann::json schema;   // see GeneratorSchema.h
  // Soft ceiling on output size. Exceeding it is a warning, not an error: a
  // deliberately dense hero prop is legitimate, an accidentally 400k-triangle
  // grass clump is not, and only the caller knows which this is. See the plan
  // §3.3 on why triangle count is a real constraint in this renderer (one
  // BLAS per unique mesh).
  size_t polyBudget = 0;
  // Produces the asset. Returns null with `error` set on failure.
  //
  // Returning a whole MeshData (rather than filling a MeshBuilder the registry
  // owns) is deliberate: it lets a generator attach procedurally generated
  // textures via MeshData::images, and lets import.gltf hand back a parsed
  // file wholesale. Geometry generators just construct a MeshBuilder locally.
  std::function<std::unique_ptr<MeshData>(const class Params &params,
                                          uint32_t seed,
                                          std::vector<std::string> &warnings,
                                          std::string &error)>
      build;
};

class GeneratorRegistry {
public:
  // Process-wide registry; the built-in generators self-register on first use.
  static GeneratorRegistry &instance();

  void registerGenerator(GeneratorInfo info);
  const GeneratorInfo *find(const std::string &name) const;
  std::vector<std::string> names() const;

  // Validates `params` against the generator's schema (filling defaults and
  // clamping in place) and runs it. `params` is taken by value because
  // validation rewrites it.
  GenResult run(const std::string &generator, nlohmann::json params,
                uint32_t seed) const;

private:
  GeneratorRegistry() = default;
  std::vector<GeneratorInfo> mGenerators;
};

// Registers every built-in generator. Idempotent; called automatically by
// GeneratorRegistry::instance().
void registerBuiltinGenerators();

} // namespace gen
