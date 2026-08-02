#include "ScatterManifest.h"

#include "json.hpp"

#include <filesystem>
#include <fstream>

namespace {
using json = nlohmann::json;

void loadString(const json &j, const char *key, std::string &out) {
  if (j.contains(key) && j[key].is_string())
    out = j[key].get<std::string>();
}
void loadFloat(const json &j, const char *key, float &out) {
  if (j.contains(key) && j[key].is_number())
    out = j[key].get<float>();
}
void loadBool(const json &j, const char *key, bool &out) {
  if (j.contains(key) && j[key].is_boolean())
    out = j[key].get<bool>();
}
void loadVec3(const json &j, const char *key, glm::vec3 &out) {
  if (!j.contains(key) || !j[key].is_array() || j[key].size() < 3)
    return;
  const json &a = j[key];
  for (int i = 0; i < 3; ++i)
    if (a[i].is_number())
      out[i] = a[i].get<float>();
}

ScatterLayerType parseLayerType(const std::string &s) {
  if (s == "rock") return ScatterLayerType::Rock;
  if (s == "grass") return ScatterLayerType::Grass;
  return ScatterLayerType::Tree;
}
std::string layerTypeToString(ScatterLayerType t) {
  switch (t) {
  case ScatterLayerType::Rock: return "rock";
  case ScatterLayerType::Grass: return "grass";
  default: return "tree";
  }
}

ScatterCollisionType parseCollisionType(const std::string &s) {
  if (s == "capsule") return ScatterCollisionType::Capsule;
  if (s == "convex-or-sphere") return ScatterCollisionType::ConvexOrSphere;
  return ScatterCollisionType::None;
}
std::string collisionTypeToString(ScatterCollisionType t) {
  switch (t) {
  case ScatterCollisionType::Capsule: return "capsule";
  case ScatterCollisionType::ConvexOrSphere: return "convex-or-sphere";
  default: return "none";
  }
}

void loadLayer(const json &j, ScatterLayer &layer) {
  loadString(j, "name", layer.name);
  loadString(j, "mesh", layer.meshPath);
  if (j.contains("type") && j["type"].is_string())
    layer.type = parseLayerType(j["type"].get<std::string>());
  loadFloat(j, "density", layer.density);

  if (j.contains("biomes") && j["biomes"].is_object()) {
    const json &b = j["biomes"];
    loadFloat(b, "meadow", layer.biomeMeadow);
    loadFloat(b, "forest", layer.biomeForest);
    loadFloat(b, "mountain", layer.biomeMountain);
  }

  if (j.contains("clustering") && j["clustering"].is_object()) {
    const json &c = j["clustering"];
    loadBool(c, "stands", layer.clustering.stands);
    loadFloat(c, "standRadius", layer.clustering.standRadius);
    loadFloat(c, "clearingChance", layer.clustering.clearingChance);
    loadBool(c, "outcrops", layer.clustering.outcrops);
  }

  loadFloat(j, "minSpacing", layer.minSpacing);

  if (j.contains("scale") && j["scale"].is_array() && j["scale"].size() >= 2) {
    if (j["scale"][0].is_number()) layer.scaleMin = j["scale"][0].get<float>();
    if (j["scale"][1].is_number()) layer.scaleMax = j["scale"][1].get<float>();
  }
  if (j.contains("heightScale") && j["heightScale"].is_array() &&
      j["heightScale"].size() >= 2) {
    if (j["heightScale"][0].is_number())
      layer.heightScaleMin = j["heightScale"][0].get<float>();
    if (j["heightScale"][1].is_number())
      layer.heightScaleMax = j["heightScale"][1].get<float>();
  }
  loadFloat(j, "leanMaxDeg", layer.leanMaxDeg);
  loadBool(j, "randomYaw", layer.randomYaw);
  loadFloat(j, "alignToNormal", layer.alignToNormal);
  loadFloat(j, "slopeMax", layer.slopeMax);
  loadFloat(j, "moistureMin", layer.moistureMin);
  loadFloat(j, "sinkIntoGround", layer.sinkIntoGround);

  if (j.contains("collision") && j["collision"].is_string())
    layer.collision = parseCollisionType(j["collision"].get<std::string>());
  loadBool(j, "castRayShadow", layer.castRayShadow);
  loadBool(j, "interactive", layer.interactive);

  if (j.contains("meshUpAxisFixDeg") && j["meshUpAxisFixDeg"].is_array() &&
      j["meshUpAxisFixDeg"].size() >= 3) {
    const json &fix = j["meshUpAxisFixDeg"];
    if (fix[0].is_number()) layer.meshUpAxisFixDeg.x = fix[0].get<float>();
    if (fix[1].is_number()) layer.meshUpAxisFixDeg.y = fix[1].get<float>();
    if (fix[2].is_number()) layer.meshUpAxisFixDeg.z = fix[2].get<float>();
  }

  loadVec3(j, "tint", layer.tint);

  loadFloat(j, "maxDrawDistance", layer.maxDrawDistance);
  loadFloat(j, "densityFalloffStart", layer.densityFalloffStart);
  loadBool(j, "wind", layer.wind);
  loadFloat(j, "windStrength", layer.windStrength);
  loadFloat(j, "windSpeed", layer.windSpeed);
  loadBool(j, "alphaCutout", layer.alphaCutout);
  loadFloat(j, "cullCellSize", layer.cullCellSize);
  loadFloat(j, "patchScale", layer.patchScale);
  loadFloat(j, "patchThreshold", layer.patchThreshold);
  loadFloat(j, "groundOcclusion", layer.groundOcclusion);
  if (j.contains("foliageSssStrength"))
    loadFloat(j, "foliageSssStrength", layer.foliageSssStrength);
  else
    loadFloat(j, "rimStrength", layer.foliageSssStrength);
}

json layerToJson(const ScatterLayer &layer) {
  json j;
  j["name"] = layer.name;
  j["mesh"] = layer.meshPath;
  j["type"] = layerTypeToString(layer.type);
  j["density"] = layer.density;
  j["biomes"] = {{"meadow", layer.biomeMeadow},
                {"forest", layer.biomeForest},
                {"mountain", layer.biomeMountain}};
  j["clustering"] = {{"stands", layer.clustering.stands},
                     {"standRadius", layer.clustering.standRadius},
                     {"clearingChance", layer.clustering.clearingChance},
                     {"outcrops", layer.clustering.outcrops}};
  j["minSpacing"] = layer.minSpacing;
  j["scale"] = {layer.scaleMin, layer.scaleMax};
  j["heightScale"] = {layer.heightScaleMin, layer.heightScaleMax};
  j["leanMaxDeg"] = layer.leanMaxDeg;
  j["randomYaw"] = layer.randomYaw;
  j["alignToNormal"] = layer.alignToNormal;
  j["slopeMax"] = layer.slopeMax;
  j["moistureMin"] = layer.moistureMin;
  j["sinkIntoGround"] = layer.sinkIntoGround;
  j["collision"] = collisionTypeToString(layer.collision);
  j["castRayShadow"] = layer.castRayShadow;
  j["interactive"] = layer.interactive;
  j["meshUpAxisFixDeg"] = {layer.meshUpAxisFixDeg.x, layer.meshUpAxisFixDeg.y,
                          layer.meshUpAxisFixDeg.z};
  j["tint"] = {layer.tint.x, layer.tint.y, layer.tint.z};
  j["maxDrawDistance"] = layer.maxDrawDistance;
  j["densityFalloffStart"] = layer.densityFalloffStart;
  j["wind"] = layer.wind;
  j["windStrength"] = layer.windStrength;
  j["windSpeed"] = layer.windSpeed;
  j["alphaCutout"] = layer.alphaCutout;
  j["cullCellSize"] = layer.cullCellSize;
  j["patchScale"] = layer.patchScale;
  j["patchThreshold"] = layer.patchThreshold;
  j["groundOcclusion"] = layer.groundOcclusion;
  j["foliageSssStrength"] = layer.foliageSssStrength;
  return j;
}

} // namespace

ScatterManifest defaultScatterManifest() {
  ScatterManifest m;
  m.version = kScatterManifestVersion;

  // tree.obj is authored with its trunk along local X (bounding box:
  // X-extent ~11.2, Y-extent ~5.7, Z-extent ~5.5) instead of Y-up --
  // without this, every placed tree lies on its side. Post-fix the mesh is
  // ~11.2 units tall, so the scale/heightScale ranges below read directly as
  // "x11.2 meters".
  const glm::vec3 kTreeUpFix(0.0f, 0.0f, 90.0f);

  // --- Mature conifers: the forest's silhouette ---
  // Grown by UNIFORM scale (1.25-2.05 -> roughly 14-23m tall, 7-12m wide),
  // NOT by vertical stretch. tree.obj's authored height/width ratio is 1.98;
  // an earlier revision reached the same heights via heightScale and pushed
  // that ratio to 2.7-3.8, which reads as a distorted asset rather than as a
  // bigger tree. heightScale is left near 1.0 here and used only for slight
  // per-instance unevenness -- see ScatterLayer::heightScaleMin.
  // minSpacing keeps trunks from interpenetrating at these canopy widths.
  ScatterLayer pine;
  pine.name = "pine_canopy";
  pine.meshPath = "assets/terraingeneratorassets/tree.obj";
  pine.type = ScatterLayerType::Tree;
  pine.density = 0.05f;
  pine.biomeMeadow = 0.06f;  // rare lone trees
  pine.biomeForest = 1.0f;   // dense
  pine.biomeMountain = 0.15f; // thins toward the treeline (already in wMountain)
  pine.clustering.stands = true;
  pine.clustering.standRadius = 25.0f;
  pine.clustering.clearingChance = 0.25f;
  pine.minSpacing = 6.0f;
  pine.scaleMin = 1.25f;
  pine.scaleMax = 2.05f;
  // Just enough spread to break up a uniform canopy line; small enough that
  // no individual tree reads as stretched.
  pine.heightScaleMin = 0.94f;
  pine.heightScaleMax = 1.12f;
  pine.leanMaxDeg = 4.0f;
  pine.randomYaw = true;
  pine.alignToNormal = 0.15f;
  pine.slopeMax = 0.45f;
  pine.moistureMin = 0.3f;
  pine.collision = ScatterCollisionType::Capsule;
  pine.castRayShadow = true;
  pine.interactive = true;
  pine.wind = true;
  pine.windStrength = 0.55f; // meters of crown sway -- large plant, small ANGLE
  pine.windSpeed = 0.35f;
  pine.groundOcclusion = 0.25f; // subtle -- trees get real RT shadows too
  pine.tint = glm::vec3(0.92f, 1.0f, 0.88f); // cooler/darker than the saplings
  pine.meshUpAxisFixDeg = kTreeUpFix;
  m.layers.push_back(pine);

  // --- Saplings/undergrowth: fills the space beneath the canopy ---
  // Same mesh at roughly a quarter the size, with a warmer tint and a much
  // looser lean. The two layers read as different plants through SIZE, tint
  // and posture rather than through distorted proportions -- both keep the
  // asset's authored shape. Denser and much more tightly spaced than the
  // canopy layer, and NOT interactive/collidable: walking through
  // shoulder-high scrub should not stop the player.
  ScatterLayer sapling;
  sapling.name = "pine_young";
  sapling.meshPath = "assets/terraingeneratorassets/tree.obj";
  sapling.type = ScatterLayerType::Tree;
  sapling.density = 0.07f;
  sapling.biomeMeadow = 0.10f;
  sapling.biomeForest = 0.85f;
  sapling.biomeMountain = 0.25f;
  sapling.clustering.stands = true;
  sapling.clustering.standRadius = 18.0f;
  sapling.clustering.clearingChance = 0.35f;
  sapling.minSpacing = 2.4f;
  sapling.scaleMin = 0.30f;
  sapling.scaleMax = 0.65f;
  sapling.heightScaleMin = 0.90f;
  sapling.heightScaleMax = 1.08f;
  sapling.leanMaxDeg = 11.0f;
  sapling.randomYaw = true;
  sapling.alignToNormal = 0.35f;
  sapling.slopeMax = 0.6f;
  sapling.moistureMin = 0.25f;
  sapling.collision = ScatterCollisionType::None;
  sapling.castRayShadow = true;
  sapling.interactive = false;
  sapling.wind = true;
  sapling.windStrength = 0.30f;
  sapling.windSpeed = 0.8f;
  sapling.groundOcclusion = 0.35f;
  sapling.tint = glm::vec3(1.08f, 1.04f, 0.86f);
  sapling.meshUpAxisFixDeg = kTreeUpFix;
  m.layers.push_back(sapling);

  ScatterLayer boulder;
  boulder.name = "boulder";
  boulder.meshPath = "assets/terraingeneratorassets/rock.obj";
  boulder.type = ScatterLayerType::Rock;
  boulder.density = 0.004f;
  boulder.biomeMeadow = 0.3f;
  boulder.biomeForest = 0.3f;
  boulder.biomeMountain = 1.0f;
  boulder.clustering.outcrops = true;
  boulder.scaleMin = 0.4f;
  boulder.scaleMax = 2.2f;
  boulder.randomYaw = true;
  boulder.alignToNormal = 0.8f;
  boulder.sinkIntoGround = 0.15f;
  boulder.collision = ScatterCollisionType::ConvexOrSphere;
  boulder.castRayShadow = true;
  m.layers.push_back(boulder);

  // --- Ground cover ---
  // grass.obj is a real geometric clump (77 verts / 128 tris), not an alpha
  // card, so it needs no cutout and looks correct from any angle -- the
  // vegetation pipeline already rasterizes with VK_CULL_MODE_NONE. Its MTL
  // points at a Forest.psd this engine cannot load, hence the explicit tints
  // below.
  //
  // Like tree.obj (same source .blend), it is authored with its up-axis
  // along local X, not Y: slicing the mesh along X gives a clean
  // narrow-at-the-root, fanning-to-the-tips profile, while slicing along Y
  // or Z gives that same fan seen side-on. Without kTreeUpFix every clump
  // lies flat on the ground. Post-fix the mesh is ~0.72 units tall.
  //
  // The four settings that make grass affordable, all of them mandatory:
  //   castRayShadow=false  keeps ~40k instances/chunk out of the TLAS
  //   maxDrawDistance      cuts drawing well before the terrain far plane
  //   cullCellSize         gives the distance test sub-chunk resolution
  //   patchScale           leaves bare ground, so density buys coverage
  //                        variety instead of a uniform carpet
  ScatterLayer grass;
  grass.name = "grass_meadow";
  grass.meshPath = "assets/terraingeneratorassets/grass.obj";
  grass.type = ScatterLayerType::Grass;
  // instances/m^2 -- ~3 orders of magnitude above the tree layers. At 1.2 the
  // ~0.7m-wide clumps sit ~0.9m apart, so they very nearly touch: dense
  // enough to read as continuous cover, while keeping one 64m chunk at
  // ~5k instances rather than the ~6.5k a fully-closed carpet would need.
  grass.density = 1.2f;
  grass.biomeMeadow = 1.0f;
  grass.biomeForest = 0.45f; // thins under canopy
  grass.biomeMountain = 0.12f;
  // grass.obj is ~0.72m wide / 0.57m tall unscaled, so these put a clump at
  // roughly 0.3-0.6m across and 0.2-0.6m tall -- ankle-to-shin height. Scaled
  // much above 1.0 the clump's individual blades become readable as
  // metre-long shards rather than grass.
  grass.scaleMin = 0.45f;
  grass.scaleMax = 0.85f;
  grass.heightScaleMin = 0.8f;
  grass.heightScaleMax = 1.3f;
  grass.leanMaxDeg = 14.0f;
  grass.randomYaw = true;
  grass.alignToNormal = 0.7f; // lies along the slope it grows on
  grass.slopeMax = 0.72f;
  grass.sinkIntoGround = 0.06f; // hides the clump's flat base
  grass.collision = ScatterCollisionType::None;
  grass.castRayShadow = false;
  grass.interactive = false;
  grass.tint = glm::vec3(0.42f, 0.66f, 0.26f);
  grass.maxDrawDistance = 95.0f;
  grass.densityFalloffStart = 55.0f;
  grass.wind = true;
  grass.windStrength = 0.13f;
  grass.windSpeed = 1.6f;
  grass.cullCellSize = 8.0f;
  grass.patchScale = 1.0f;
  grass.patchThreshold = 0.34f;
  grass.groundOcclusion = 0.62f;
  grass.meshUpAxisFixDeg = kTreeUpFix;
  m.layers.push_back(grass);

  // Taller, sparser, yellower tufts scattered over the short cover, with an
  // independent patch mask (different frequency) so the two layers' patches
  // don't coincide. Same silhouette-variety argument as the two pine layers.
  ScatterLayer tuft;
  tuft.name = "grass_tuft";
  tuft.meshPath = "assets/terraingeneratorassets/grass.obj";
  tuft.type = ScatterLayerType::Grass;
  tuft.density = 0.45f;
  tuft.biomeMeadow = 0.8f;
  tuft.biomeForest = 1.0f;
  tuft.biomeMountain = 0.3f;
  // Knee-to-thigh height: taller than the ground cover, but still grass.
  tuft.scaleMin = 0.6f;
  tuft.scaleMax = 1.0f;
  tuft.heightScaleMin = 1.3f;
  tuft.heightScaleMax = 2.0f;
  tuft.leanMaxDeg = 20.0f;
  tuft.randomYaw = true;
  tuft.alignToNormal = 0.5f;
  tuft.slopeMax = 0.8f;
  tuft.sinkIntoGround = 0.1f;
  tuft.collision = ScatterCollisionType::None;
  tuft.castRayShadow = false;
  tuft.interactive = false;
  tuft.tint = glm::vec3(0.55f, 0.62f, 0.24f);
  tuft.maxDrawDistance = 110.0f;
  tuft.densityFalloffStart = 70.0f;
  tuft.wind = true;
  tuft.windStrength = 0.26f;
  tuft.windSpeed = 1.25f;
  tuft.cullCellSize = 10.0f;
  tuft.patchScale = 2.3f;
  tuft.patchThreshold = 0.46f;
  tuft.groundOcclusion = 0.55f;
  // Must match grass_meadow's -- both layers share grass.obj, and the
  // corrective rotation is a property of the file (see loadScatterMeshData).
  tuft.meshUpAxisFixDeg = kTreeUpFix;
  m.layers.push_back(tuft);

  return m;
}

bool loadScatterManifest(const std::string &jsonPath, ScatterManifest &out) {
  std::ifstream f(jsonPath);
  if (!f.is_open())
    return false;

  json j;
  try {
    f >> j;
  } catch (const std::exception &) {
    return false; // malformed JSON -- caller falls back to the default manifest
  }

  if (!j.contains("layers") || !j["layers"].is_array())
    return false;

  ScatterManifest manifest;
  // Absent "version" means a file written before versioning existed (R4).
  manifest.version = 1;
  if (j.contains("version") && j["version"].is_number_integer())
    manifest.version = j["version"].get<int>();

  for (const json &lj : j["layers"]) {
    if (!lj.is_object())
      continue;
    ScatterLayer layer;
    loadLayer(lj, layer);
    if (layer.meshPath.empty())
      continue; // a layer with no mesh can never place anything -- skip
    manifest.layers.push_back(std::move(layer));
  }
  if (manifest.layers.empty())
    return false;

  out = std::move(manifest);
  return true;
}

bool writeScatterManifest(const std::string &jsonPath, const ScatterManifest &manifest) {
  std::error_code ec;
  std::filesystem::path path(jsonPath);
  if (path.has_parent_path())
    std::filesystem::create_directories(path.parent_path(), ec);

  json j;
  j["version"] = manifest.version;
  json layers = json::array();
  for (const ScatterLayer &layer : manifest.layers)
    layers.push_back(layerToJson(layer));
  j["layers"] = layers;

  std::ofstream f(jsonPath);
  if (!f.is_open())
    return false;
  f << j.dump(2);
  return true;
}
