// import.gltf — a file on disk, expressed as a recipe.
//
// This is how Option A (text-to-3D services, asset stores, anything modelled
// by hand) folds into the same system as the procedural generators
// (AI_ASSET_PIPELINE_PLAN.md §3.5): an imported model is just another recipe,
// so scatter layers, the asset library and the eventual MCP tools do not need
// to care where geometry came from.
//
// The fetch/conditioning half deliberately lives OUTSIDE the engine (a
// separate offline CLI). This generator only loads what is already on disk --
// no network, no HTTP dependency in the engine.

#include "../GeneratorRegistry.h"
#include "../GeneratorSchema.h"
#include "../MeshDecimate.h"
#include "MeshParse.h"

#include <algorithm>
#include <filesystem>

namespace gen {

void registerImportGenerator() {
  GeneratorInfo info;
  info.name = "import.gltf";
  info.description =
      "Loads an existing model file (.obj, .gltf, .glb, .fbx) as an asset, "
      "with optional pivot and up-axis correction. Use this for hand-modelled "
      "or externally generated assets so they participate in the same recipe "
      "system as procedural ones. Does not download anything -- the file must "
      "already be on disk.";
  info.polyBudget = 0; // caller-supplied geometry; no meaningful ceiling
  info.schema =
      SchemaBuilder()
          .string("path", "",
                  "Path to the model file, absolute or relative to the "
                  "project root.")
          .enumString("recenter", "baseY", {"none", "baseY", "center"},
                      "Pivot correction. 'baseY' puts the origin at the centre "
                      "of the model's footprint with its base at y=0 (right "
                      "for props placed on ground); 'center' fully centres it "
                      "(right for physics bodies that tumble).")
          .color("rotateDeg", glm::vec3(0.0f),
                 "Corrective rotation in degrees [x,y,z], baked into the "
                 "vertices. For assets authored with a non-Y-up convention.")
          .number("scale", 1.0f, 0.0001f, 1000.0f,
                  "Uniform scale applied to the vertices, for files authored "
                  "in the wrong units.")
          .number("normalizeHeight", 0.0f, 0.0f, 200.0f,
                  "If non-zero, rescale so the model's HEIGHT (Y extent) is "
                  "this many metres. Overrides 'scale'. Right for things that "
                  "stand upright -- a tree, a person, a lamp post.")
          .number("normalizeSize", 0.0f, 0.0f, 200.0f,
                  "If non-zero, rescale so the model's LONGEST dimension is "
                  "this many metres. Overrides 'scale' and 'normalizeHeight'. "
                  "Use this when the orientation is unknown or the object is "
                  "not upright: normalizing the height of something lying "
                  "flat (a wrench, a plank) scales its thin axis and makes it "
                  "enormous.")
          .integer("maxTriangles", 0, 0, 500000,
                   "If non-zero, decimate to at most this many triangles. "
                   "Downloaded and store-bought meshes routinely arrive at "
                   "30k-200k; every unique mesh here costs a ray-tracing "
                   "acceleration structure, so anything scattered must be "
                   "reduced. Suggested: 4000 for a tree, 1500 for a rock, "
                   "20000 for a hero prop seen up close.")
          .schema();

  info.build = [](const Params &p, uint32_t,
                  std::vector<std::string> &warnings,
                  std::string &error) -> std::unique_ptr<MeshData> {
    const std::string path = p.str("path");
    if (path.empty()) {
      error = "import.gltf requires a 'path'";
      return nullptr;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
      error = "import.gltf: no such file '" + path + "'";
      return nullptr;
    }

    std::unique_ptr<MeshData> mesh = parseMeshFile(path);
    if (!mesh) {
      error = "import.gltf: failed to parse '" + path +
              "' (supported: .obj, .gltf, .glb, .fbx)";
      return nullptr;
    }

    // Rotate BEFORE recentering: the base-at-y=0 shift has to apply to the
    // corrected orientation, or a model rotated upright afterwards ends up
    // pivoted around what used to be its side. Same ordering rule as
    // AssetManager::rotateOBJ/recenterOBJ.
    const glm::vec3 rotate = p.color("rotateDeg", glm::vec3(0.0f));
    if (glm::dot(rotate, rotate) > 1e-8f)
      mesh->rotateEulerDeg(rotate);

    // The normalize* options win over 'scale': they work without knowing what
    // units the file was authored in, which is the usual case for downloaded
    // assets. normalizeSize wins over normalizeHeight, being the safer of the
    // two when orientation is unknown.
    float scale = p.num("scale", 1.0f);
    const float targetHeight = p.num("normalizeHeight", 0.0f);
    const float targetSize = p.num("normalizeSize", 0.0f);
    if (targetSize > 0.0f || targetHeight > 0.0f) {
      glm::vec3 mn, mx;
      if (mesh->getGlobalBounds(mn, mx)) {
        const glm::vec3 extent = mx - mn;
        const bool useLongest = targetSize > 0.0f;
        const float measured =
            useLongest ? std::max({extent.x, extent.y, extent.z}) : extent.y;
        const float target = useLongest ? targetSize : targetHeight;
        if (measured > 1e-6f) {
          scale = target / measured;
          warnings.push_back(
              std::string("normalized ") + (useLongest ? "longest axis " : "height ") +
              std::to_string(measured) + " -> " + std::to_string(target) +
              " m (scale " + std::to_string(scale) + ")");
          // A model whose height is a small fraction of its length is lying
          // down; normalizing its height would have scaled the thin axis.
          if (!useLongest) {
            const float longest = std::max({extent.x, extent.y, extent.z});
            if (longest > extent.y * 3.0f)
              warnings.push_back(
                  "this model is much longer than it is tall, so "
                  "normalizeHeight scaled its thin axis -- the result is "
                  "roughly " + std::to_string(longest * scale) +
                  " m long. Use normalizeSize instead if that is not intended.");
          }
        } else {
          warnings.push_back("normalize ignored: the model has no extent");
        }
      }
    }
    if (std::fabs(scale - 1.0f) > 1e-6f) {
      for (auto &sm : mesh->submeshes) {
        for (auto &v : sm.vertices)
          v.pos *= scale;
        if (sm.hasBounds) {
          sm.aabbMin *= scale;
          sm.aabbMax *= scale;
        }
      }
      for (auto &ob : mesh->objectBounds) {
        if (ob.second.hasBounds) {
          ob.second.aabbMin *= scale;
          ob.second.aabbMax *= scale;
        }
      }
    }

    // Decimate BEFORE recentering: collapses move vertices, so bounds
    // computed first would be stale by the time the pivot is placed.
    const int maxTriangles = p.integer("maxTriangles", 0);
    if (maxTriangles > 0) {
      const DecimateResult reduced =
          decimateMesh(*mesh, static_cast<size_t>(maxTriangles));
      if (reduced.changed())
        warnings.push_back("decimated " +
                           std::to_string(reduced.trianglesBefore) + " -> " +
                           std::to_string(reduced.trianglesAfter) +
                           " triangles");
      for (const std::string &note : reduced.notes)
        warnings.push_back(note);
    }

    const std::string recenter = p.str("recenter", "baseY");
    if (recenter == "baseY")
      mesh->recenter(MeshData::Recenter::BaseY);
    else if (recenter == "center")
      mesh->recenter(MeshData::Recenter::Center);

    size_t tris = 0;
    for (const auto &sm : mesh->submeshes)
      tris += (sm.indices.empty() ? sm.vertices.size() : sm.indices.size()) / 3;
    if (tris > 20000)
      warnings.push_back(
          "imported mesh has " + std::to_string(tris) +
          " triangles; every unique mesh costs a ray-tracing acceleration "
          "structure, so set 'maxTriangles' before using this for anything "
          "scattered");

    return mesh;
  };

  GeneratorRegistry::instance().registerGenerator(std::move(info));
}

} // namespace gen
