#pragma once
// TextureGen.h — procedural PBR maps, generated straight into memory.
//
// The output goes into MeshData::images as embedded payloads keyed by the same
// string the material's tex*Path fields use. VulkanRenderer::buildMeshFromData
// checks findImage(path) BEFORE touching the filesystem, so a generated
// texture never has to exist as a file and needs no renderer changes at all.
//
// What is generated, and what is not:
//   albedo    sRGB, RGBA
//   roughness linear, single-channel replicated to RGB
//   ao        linear, optional
//
// Two deliberate omissions, both dictated by what the scene mesh pipeline
// actually samples:
//
//   NO normal maps. mesh.frag reads texDiffusePath, texRoughnessPath,
//   texMetallicPath and texAOPath; it has no normal-map binding (only the
//   terrain material slots do). Generating one would burn memory and a
//   bindless slot for something nothing reads.
//
//   Albedo alpha is WRITTEN BUT NOT USED YET. VulkanRenderer::DrawItem carries
//   no alpha-cutoff and materialFlags has no alpha-test bit, so
//   MaterialAsset::alphaCutoff is silently ignored and an alpha-masked quad
//   renders as an opaque rectangle. Generators therefore put foliage
//   silhouettes in GEOMETRY (MeshBuilder::addLeafCard, and grass blades'
//   geometric taper), not in alpha. The alpha channel is still generated
//   because it is correct data and costs nothing extra -- it starts working
//   the day mesh.frag gains a cutout path.
//
// If either gap is closed in the renderer, this is the file to revisit.

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
