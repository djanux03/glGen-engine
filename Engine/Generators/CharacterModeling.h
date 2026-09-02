#pragma once
// CharacterModeling.h -- deterministic implicit-surface character construction.
//
// The LLM never writes vertices.  It edits a compact, versioned operation list
// which is evaluated here into a watertight-ish, indexed MeshData.  Keeping the
// representation in EngineCore means MCP, Lua, recipes and future UI clients
// all produce exactly the same character from the same parameters and seed.

#include "MeshData.h"
#include "json.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gen::modeling {

// Builds a neutral-pose humanoid from `character.v1` parameters.  `operations`
// is deliberately semantic (reshape, add_garment, add_feature, add_hair,
// add_accessory, assign_material, mirror, smooth, remesh, simplify): malformed
// or unknown operations become warnings rather than arbitrary geometry input.
std::unique_ptr<MeshData> buildHumanoid(const nlohmann::json &params,
                                        uint32_t seed,
                                        std::vector<std::string> &warnings,
                                        std::string &error);

} // namespace gen::modeling
