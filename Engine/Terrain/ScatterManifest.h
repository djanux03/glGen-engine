#pragma once
// ScatterManifest.h — R4 of MEADOW_TERRAIN_REVAMP_PLAN.md §6a: a data-driven
// description of what gets scattered across the terrain, replacing the old
// fixed VegSpecies enum + procedurally-built primitive meshes
// (VegetationPrimitives.cpp, deleted). Each layer points at a real mesh
// file (.obj/.fbx/.gltf, loaded through the existing AssetManager parsers --
// see VkTerrainSubsystem.cpp) and carries the rules TerrainScatter.cpp uses
// to place it: a density, a per-biome multiplier gate (this is the ONE
// mechanism that gives meadow/forest/mountain their distinct vegetation
// identity -- see the plan's §6a comment on the `biomes` field), slope/
// moisture rejection, clustering behavior, and collision/render hints.
//
// R5 lights up Grass-typed layers, which R4 parsed but skipped. Grass
// differs from trees/rocks in kind, not just density: it is placed by the
// tens of thousands per chunk, so it needs its own culling granularity
// (cullCellSize), its own draw distance, sparse ray-traced shadow geometry,
// and vertex wind. Those knobs live on ScatterLayer alongside the existing
// placement rules rather than in a separate grass system -- placement,
// biome gating and clustering are identical machinery.

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

enum class ScatterLayerType { Tree, Rock, Grass };

enum class ScatterCollisionType {
  None,
  Capsule,      // trees: promoted to a physically-collidable marker entity
  ConvexOrSphere // rocks: static Sphere collider (no arbitrary convex hulls
                 // yet -- ColliderComponent only has Box/Sphere/Capsule)
};

// Optional render-only detail levels. Placement and collision always use the
// base mesh; a camera move switches geometry without regenerating scatter.
struct ScatterMeshLod {
  std::string meshPath;
  float distance = 0.0f;
};

struct ScatterClustering {
  // Tree stands: a low-frequency mask gates candidates so trees cluster
  // into groves with clearings, rather than an even sprinkle.
  bool stands = false;
  float standRadius = 25.0f;
  float clearingChance = 0.25f;
  // Rock outcrops: anchor + satellite placement, biased toward the
  // ground field's rockNoise/slope signal (natural boulder fields).
  bool outcrops = false;
};

// Fixed landmarks share the normal instanced rendering and chunk lifetime.
// Coordinates are world XZ; Y follows the generated terrain.
struct ScatterPlacement {
  glm::vec2 worldXZ{0};
  float yawDegrees = 0;
  float scale = 1;
};

// One scattered layer. Field names/defaults mirror the plan's §6a JSON
// example 1:1 so a hand-written terrain_scatter.json reads naturally.
struct ScatterLayer {
  std::string name;
  std::string meshPath; // resolved relative to the project root if not absolute
  // Optional casting-only mesh, sharing local coordinates with the base mesh.
  // Grass normally uses baked patch contact shading and leaves this empty.
  std::string shadowMeshPath;
  ScatterLayerType type = ScatterLayerType::Tree;

  std::vector<ScatterPlacement> fixedPlacements;
  float density = 0.01f; // instances / m^2 at full biome weight (1.0)

  // Per-biome density multipliers -- dotted with the biome weights at each
  // candidate position (plan §6a: "This is how the three biomes get their
  // vegetation identity from ONE system"). 1.0 each = biome-agnostic.
  float biomeMeadow = 1.0f;
  float biomeForest = 1.0f;
  float biomeMountain = 1.0f;

  ScatterClustering clustering;

  // Minimum distance (meters) between two accepted instances of this layer.
  // 0 = off (pure jittered-grid spacing, R4's behavior). Non-zero turns the
  // jittered grid into a proper dart-throwing sampler: candidates are still
  // generated on the grid, but one is rejected if any already-accepted
  // instance of the same layer lies within this radius (TerrainScatter.cpp
  // keeps a per-layer spatial hash for the O(1) lookup). This is what makes
  // "how close together can trees stand" a directly authorable number
  // instead of an emergent side effect of density.
  //
  // Enforced within one chunk only -- a cross-chunk neighbor query would
  // need placements from chunks that may not be built yet, which would break
  // the "each chunk scatters independently and deterministically" contract
  // the worker pool relies on. Visible consequence: spacing can be violated
  // in a thin band along chunk borders. At the default 64m chunk size and
  // single-digit spacing values this is rare enough not to read.
  float minSpacing = 0.0f;

  // Uniform scale range, applied to all three axes.
  float scaleMin = 0.8f;
  float scaleMax = 1.4f;

  // EXTRA vertical scale, multiplied on top of the uniform scale above. 1.0
  // leaves the mesh's authored proportions alone; >1 stretches it upward
  // without widening it. Separating the two is what lets a forest read as
  // tall and columnar (mature conifers) rather than merely big: scaling a
  // tree up uniformly makes the canopy wider at the same rate as the trunk
  // is tall, which just looks like the camera got closer.
  float heightScaleMin = 1.0f;
  float heightScaleMax = 1.0f;

  // Maximum random tilt off vertical, degrees. Applied about a random
  // horizontal axis, independent of alignToNormal (which tilts toward the
  // ground normal -- a deterministic, terrain-driven lean). This one is pure
  // per-instance noise, so a stand on flat ground still doesn't read as a
  // grid of perfectly plumb posts.
  float leanMaxDeg = 0.0f;

  bool randomYaw = true;
  float alignToNormal = 0.0f; // 0=always upright, 1=fully aligned to terrain normal
  float slopeMax = 1.0f;      // reject candidates steeper than this (0..1, 1=vertical)
  float moistureMin = 0.0f;
  float sinkIntoGround = 0.0f; // meters sunk below the sampled ground height
  // Large rocks/deadwood should leave authored service tracks navigable.
  // Small gravel can opt out; fixed landmarks keep their explicit placement.
  bool avoidTracks = false;

  ScatterCollisionType collision = ScatterCollisionType::None;
  // false keeps this layer out of the TLAS (casting only; shadow reception
  // still comes from the shader). Stock grass disables casting and uses broad
  // ground contact shading; a separate master switch bounds custom grass too.
  bool castRayShadow = true;
  bool interactive = false; // trees only: eligible for capsule promotion

  // Per-layer albedo multiply, applied on top of the per-instance color
  // jitter (they share the same GPU varying -- see VegInstanceGpu). Exists
  // because a scatter mesh's own material may be unusable: grass.obj's MTL
  // references a Forest.psd this engine has no loader for, so it would
  // otherwise render at the default flat texture color. Authoring the tint
  // here keeps that a data decision instead of a hardcoded shader branch.
  glm::vec3 tint{1.0f};

  // One-time corrective rotation (degrees, X then Y then Z) baked into the
  // mesh's own vertex data at load time -- for source assets authored with
  // a non-Y-up convention (e.g. tree.obj's trunk running along local X
  // instead of Y). {0,0,0} = mesh is already Y-up, no correction applied.
  // Distinct from alignToNormal/randomYaw, which rotate each PLACED
  // instance -- this fixes the mesh's own local orientation once, so those
  // per-instance rotations can keep assuming local Y is up.
  glm::vec3 meshUpAxisFixDeg{0.0f};

  // --- R5: grass/foliage rendering ---
  // Beyond this distance from the camera (meters) the layer stops drawing.
  // Overrides the renderer's global vegDrawDistance for THIS layer only, so
  // grass can cut out at 90m while trees stay visible to the far plane.
  // 0 or >= 1e6 = fall back to the global setting.
  float maxDrawDistance = 1.0e6f;
  std::vector<ScatterMeshLod> meshLods;
  // Distance at which placement density starts thinning toward zero at
  // maxDrawDistance. Applied at scatter time against the camera position the
  // chunk was built for, so it is a coarse, per-chunk-build approximation --
  // it does not re-thin as the camera moves within an already-built chunk.
  float densityFalloffStart = 1.0e6f;

  // Vertex wind sway (meshInstanced.vert). Displacement is weighted by
  // height above the instance origin, so the base stays planted and the tips
  // move most. windStrength is in meters of tip displacement.
  bool wind = false;
  float windStrength = 0.0f;
  float windSpeed = 1.0f;

  bool alphaCutout = false;

  // Root darkening, 0..1: how much light is removed at the base of the
  // instance, fading to none at its tip. 0 = flat, unoccluded shading.
  //
  // Real grass is shadowed near the ground by its own neighbours and by the
  // blades above it. This also drives the terrain patch contact mask; the
  // vertical gradient approximates unresolved ambient occlusion -- one multiply -- and
  // it is what makes grass read as growing OUT of the terrain rather than
  // resting on top of it. Applies mostly to ambient, which is where the
  // real-world effect lives.
  float groundOcclusion = 0.0f;
  // Painterly silhouette/backlight multiplier. Kept in the manifest so
  // replacement Blender foliage can tune its crown response per layer.
  float foliageSssStrength = 1.0f;
  // Opt-in surface snow; unrelated scene objects never inherit winter coating.
  bool receivesSnow = false;

  // Side length (meters) of the sub-cell grid this layer's instances are
  // bucketed into for frustum/distance culling. 0 = one culling range per
  // chunk (the R4 behavior, right for trees: a few dozen per chunk, each
  // visible from far away).
  //
  // Grass needs this. Its whole draw distance (~90m) is barely larger than
  // one 64m chunk, so per-chunk ranges would mean the culler can only ever
  // answer "this entire chunk of 40,000 clumps is in or out" -- and standing
  // anywhere inside a chunk makes the answer "in". Bucketing into 8m cells
  // gives the existing per-range distance test enough resolution to actually
  // discard the far side of the chunk.
  float cullCellSize = 0.0f;

  // Low-frequency patch mask: grass grows in patches with bare ground
  // between, not as an even carpet. patchScale is the mask's frequency
  // multiplier (higher = smaller, busier patches); candidates whose mask
  // value falls below patchThreshold are rejected. 0 = no mask.
  // Distinct from clustering.stands (which is a tree-stand/clearing mask at
  // a much coarser scale and gates whole groves).
  float patchScale = 0.0f;
  float patchThreshold = 0.35f;
};

// Bumped when the DEFAULT layer set changes in a way an existing on-disk
// manifest should pick up (new layers, retuned defaults) rather than silently
// keep the old version of. loadScatterManifest() reports the file's version;
// see the migration note on ScatterManifest::version.
//
//   v2  grass layers, spacing/height/tint/wind fields
//   v3  grass.obj up-axis correction (v2 manifests render grass lying flat)
//   v4  trees grown by uniform scale instead of vertical stretch;
//       groundOcclusion added
//   v5  transitional painterly per-layer rimStrength
//   v6  vegetation transmission renamed to foliageSssStrength
inline constexpr int kScatterManifestVersion = 6;

struct ScatterManifest {
  // Version of the DEFAULTS this manifest was generated from. A file written
  // before versioning existed parses as 1. VkTerrainSubsystem uses this to
  // decide whether to regenerate a stale terrain_scatter.json (preserving the
  // user's copy as a .bak) -- otherwise the R4-era file already sitting in
  // every working tree would keep grass permanently switched off, with no
  // indication why.
  int version = kScatterManifestVersion;
  std::vector<ScatterLayer> layers;
};

// The manifest that demos with zero user setup. R5 grows it from 2 layers to
// 5, all built from the 3 meshes already in assets/terraingeneratorassets:
//
//   pine_canopy  tall columnar mature conifers, the forest's silhouette
//   pine_young   short, wider, warmer-tinted saplings filling in beneath
//   boulder      rock outcrops, mountain-biased
//   grass_meadow dense short ground cover, meadow-dominant
//   grass_tuft   taller sparser tufts breaking up the carpet, forest-biased
//
// Two pine layers off ONE mesh is deliberate: a forest of a single tree at
// one size range reads as copy-paste no matter how well it is scattered,
// and a second layer with a different height range, tint and spacing buys
// most of the variety of a second asset for none of the authoring cost.
ScatterManifest defaultScatterManifest();

// Loads a manifest from a JSON file (project-root terrain_scatter.json).
// Returns false (out untouched) if the file doesn't exist or fails to
// parse -- callers should fall back to defaultScatterManifest().
bool loadScatterManifest(const std::string &jsonPath, ScatterManifest &out);

// Writes `manifest` to `jsonPath` as pretty-printed JSON, creating parent
// directories if needed. Used once at first run (if terrain_scatter.json
// doesn't exist yet) so there's something for the user to edit.
bool writeScatterManifest(const std::string &jsonPath, const ScatterManifest &manifest);

#include "json.hpp"
nlohmann::json scatterManifestToJson(const ScatterManifest &manifest);
bool scatterManifestFromJson(const nlohmann::json &j, ScatterManifest &out);
