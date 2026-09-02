#pragma once
// TextureGen.h — procedural PBR maps, generated straight into memory.
//
// The output goes into MeshData::images as embedded payloads keyed by the same
// string the material's tex*Path fields use. VulkanRenderer::buildMeshFromData
// checks findImage(path) BEFORE touching the filesystem, so a generated
// texture never has to exist as a file and needs no renderer changes at all.
//
// What is generated:
//   albedo    sRGB, RGBA
//   roughness linear, single-channel replicated to RGB
//   ao        linear, optional
//   normal    linear RGB, derived from each generator's deterministic height
//   alpha     honored by the renderer when MaterialAsset::alphaCutoff > 0

#include "MeshData.h"
#include "json.hpp"

#include <string>
#include <vector>

namespace gen {

struct TextureSet {
  std::vector<MeshImage> images;
  std::string albedoKey;
  std::string roughnessKey;
  std::string aoKey;
  std::string normalKey;
  bool valid() const { return !images.empty(); }
};

// Names of every available texture generator ("tex.bark", "tex.rock", ...).
std::vector<std::string> textureGeneratorNames();
// Parameter schema for one, or null if unknown. Same shape and purpose as a
// mesh generator's schema (see GeneratorSchema.h).
const nlohmann::json *textureGeneratorSchema(const std::string &name);

// Renders a texture set. `keyPrefix` namespaces the image keys so two
// materials on the same mesh cannot collide (pass the submesh/material name).
// Returns false with `error` set on an unknown generator or invalid params.
bool generateTextureSet(const std::string &generator,
                        nlohmann::json params, uint32_t seed,
                        const std::string &keyPrefix, TextureSet &out,
                        std::vector<std::string> &warnings, std::string &error);

// Points `material` at the generated maps and moves the payloads into `mesh`.
void applyTextureSet(MeshData &mesh, MaterialAsset &material, TextureSet &&set);

} // namespace gen
