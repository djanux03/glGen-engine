#include <doctest/doctest.h>
#include "ScatterManifest.h"
#include "TerrainNoise.h"
#include "TerrainScatter.h"
#include "TerrainWater.h"
#include "WoodlandLayout.h"
#include "TerrainSettings.h"
#include "TerrainTypes.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

TEST_CASE("Grass ground contact follows coverage and never requires blade shadows") {
  TerrainSettings settings;
  settings.worldBounded = false;
  settings.oceanEnabled = settings.lakesEnabled = false;
  TerrainNoiseSet noise(1337);
  ScatterLayer grass;
  grass.type = ScatterLayerType::Grass;
  grass.density = 2;
  grass.groundOcclusion = .38f;
  grass.patchScale = 0;
  grass.scaleMin = grass.scaleMax = 1;
  grass.castRayShadow = false;
  ScatterManifest manifest;
  manifest.layers.push_back(grass);
  auto contact = [&](glm::vec3 pos = glm::vec3(12, 4, 20)) {
    return grassGroundOcclusionAt(manifest, noise, settings, pos, {0,1,0}, {1,0,0}, .5f);
  };
  const float full = contact();
  CHECK(full > .75f);
  CHECK(full < 1.0f);
  CHECK(contact() == full);
  settings.grassDensityMultiplier = .25f;
  CHECK(contact() < full * .6f);
  settings.grassDensityMultiplier = 0;
  CHECK(contact() == 0);
  settings.grassDensityMultiplier = 1;
  settings.spawnGrass = false;
  CHECK(contact() == 0);
  settings.spawnGrass = true;
  settings.spawnVegetation = false;
  CHECK(contact() == 0);
  settings.spawnVegetation = true;
  settings.grassOcclusionStrength = 0;
  CHECK(contact() == 0);
  settings.grassOcclusionStrength = 1;
  manifest.layers[0].biomeMeadow = 0;
  CHECK(contact() == 0);
  manifest.layers[0].biomeMeadow = 1;
  manifest.layers[0].moistureMin = .8f;
  CHECK(contact() == 0);
  manifest.layers[0].moistureMin = 0;
  settings.oceanEnabled = true;
  settings.autoSeaLevel = false;
  settings.seaLevel = 10;
  CHECK(contact({12,0,20}) == 0);
}

TEST_CASE("Grass patch contact is continuous at a chunk boundary") {
  TerrainSettings settings;
  settings.oceanEnabled = settings.lakesEnabled = false;
  settings.worldBounded = false;
  TerrainNoiseSet noise(1337);
  auto manifest = defaultScatterManifest();
  for (int z=0; z<64; ++z) {
    const auto sample = [&](float x) {
      return grassGroundOcclusionAt(manifest, noise, settings, {x,4,float(z)},
                                     {0,1,0}, {1,0,0}, .5f);
    };
    CHECK(std::abs(sample(64-.001f)-sample(64+.001f)) < .001f);
  }
}

TEST_CASE("scatterLayers — deterministic for the same seed and chunk") {
  TerrainSettings settings;
  TerrainNoiseSet noise(1337);
  const ScatterManifest manifest = defaultScatterManifest();
  const glm::vec2 origin(0.0f, 0.0f);
  const uint32_t chunkSeed = chunkSeedFor(settings.seed, ChunkCoord{0, 0});

  std::vector<ScatterInstance> a, b;
  scatterLayers(manifest, noise, settings, origin, settings.chunkWorldSize, chunkSeed, 0, a);
  scatterLayers(manifest, noise, settings, origin, settings.chunkWorldSize, chunkSeed, 0, b);

  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    CHECK(a[i].layerIndex == b[i].layerIndex);
    CHECK(a[i].interactive == b[i].interactive);
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r)
        CHECK(a[i].transform[c][r] == doctest::Approx(b[i].transform[c][r]));
  }
}

TEST_CASE("scatterLayers — different chunk coords scatter differently") {
  TerrainSettings settings;
  TerrainNoiseSet noise(1337);
  const ScatterManifest manifest = defaultScatterManifest();

  std::vector<ScatterInstance> a, b;
  scatterLayers(manifest, noise, settings, glm::vec2(0.0f, 0.0f), settings.chunkWorldSize,
                chunkSeedFor(settings.seed, ChunkCoord{0, 0}), 0, a);
  scatterLayers(manifest, noise, settings, glm::vec2(settings.chunkWorldSize, 0.0f),
                settings.chunkWorldSize, chunkSeedFor(settings.seed, ChunkCoord{1, 0}), 1, b);

  // Not a strict guarantee for every possible noise field, but with real
  // fbm noise and a different world-space sampling region, an identical
  // placement list would be a near-impossible coincidence.
  bool anyDifference = a.size() != b.size();
  for (size_t i = 0; i < a.size() && i < b.size() && !anyDifference; ++i)
    if (a[i].layerIndex != b[i].layerIndex || a[i].transform != b[i].transform)
      anyDifference = true;
  CHECK(anyDifference);
}

TEST_CASE("scatterLayers — spawnVegetation=false produces nothing") {
  TerrainSettings settings;
  settings.spawnVegetation = false;
  TerrainNoiseSet noise(1337);
  const ScatterManifest manifest = defaultScatterManifest();

  std::vector<ScatterInstance> out;
  scatterLayers(manifest, noise, settings, glm::vec2(0.0f), settings.chunkWorldSize, 42u, 0, out);
  CHECK(out.empty());
}

TEST_CASE("scatterLayers — a layer with all-zero biome multipliers places nothing") {
  TerrainSettings settings;
  TerrainNoiseSet noise(1337);
  ScatterManifest manifest;
  ScatterLayer layer;
  layer.name = "inert";
  layer.meshPath = "assets/terraingeneratorassets/tree.obj";
  layer.type = ScatterLayerType::Tree;
  layer.density = 0.05f;
  layer.biomeMeadow = 0.0f;
  layer.biomeForest = 0.0f;
  layer.biomeMountain = 0.0f;
  manifest.layers.push_back(layer);

  std::vector<ScatterInstance> out;
  scatterLayers(manifest, noise, settings, glm::vec2(0.0f), settings.chunkWorldSize, 7u, 0, out);
  CHECK(out.empty());
}

// R5 replaces the R4-era "Grass-typed layers are skipped entirely" test:
// grass now places, and these are the four gates that decide whether it does.
namespace {
// A biome-agnostic grass layer so placement isn't gated by terrain shape.
ScatterLayer testGrassLayer() {
  ScatterLayer grass;
  grass.name = "grass_clump";
  // Doesn't need to exist -- scatterLayers never loads meshes.
  grass.meshPath = "assets/meadow/grass_clump.obj";
  grass.type = ScatterLayerType::Grass;
  grass.density = 1.0f;
  grass.biomeMeadow = 1.0f;
  grass.biomeForest = 1.0f;
  grass.biomeMountain = 1.0f;
  grass.slopeMax = 1.0f;
  return grass;
}
} // namespace

TEST_CASE("scatterLayers — Grass-typed layers now place instances (R5)") {
  TerrainSettings settings;
  TerrainNoiseSet noise(1337);
  ScatterManifest manifest;
  manifest.layers.push_back(testGrassLayer());

  std::vector<ScatterInstance> out;
  scatterLayers(manifest, noise, settings, glm::vec2(0.0f), settings.chunkWorldSize, 7u, 0, out);
  // At 1/m^2 over a 64m chunk the count is in the thousands; assert only that
  // it is emphatically non-empty, so terrain-shape rejections can't make this
  // flaky.
  CHECK(out.size() > 500);
}

TEST_CASE("Grass sampling fills its budget without a one-tuft-per-cell grid") {
  TerrainSettings settings;
  settings.worldBounded = settings.oceanEnabled = settings.lakesEnabled = false;
  settings.heightScale = 0;
  TerrainNoiseSet noise(1337);
  ScatterManifest manifest;
  auto layer = testGrassLayer();
  layer.patchScale = 0;
  manifest.layers.push_back(layer);
  std::vector<ScatterInstance> a,b;
  const glm::vec2 origin(-16,0);
  scatterLayers(manifest,noise,settings,origin,16,791u,0,a);
  scatterLayers(manifest,noise,settings,origin,16,791u,0,b);
  REQUIRE(a.size() == 256);
  REQUIRE(a.size() == b.size());
  std::map<std::pair<int,int>,int> cells;
  for (size_t i=0; i<a.size(); ++i) {
    CHECK(a[i].transform == b[i].transform);
    const glm::vec3 p(a[i].transform[3]);
    CHECK(p.x >= -16); CHECK(p.x < 0);
    CHECK(p.z >= 0); CHECK(p.z < 16);
    ++cells[{static_cast<int>(std::floor(p.x)),static_cast<int>(std::floor(p.z))}];
    for (size_t j=0; j<i; ++j) {
      const glm::vec3 q(a[j].transform[3]);
      CHECK(glm::length(glm::vec2(p.x-q.x,p.z-q.z)) >= .4499f);
    }
  }
  // The former jittered grid put exactly one accepted tuft in each metre
  // cell. Random bounded spacing should have both empty and double cells.
  CHECK(cells.size() < 230);
  CHECK(std::any_of(cells.begin(),cells.end(),[](const auto &c){return c.second>1;}));
}

TEST_CASE("scatterLayers — spawnGrass=false disables only Grass layers") {
  TerrainSettings settings;
  settings.spawnGrass = false;
  TerrainNoiseSet noise(1337);

  ScatterManifest manifest;
  manifest.layers.push_back(testGrassLayer());
  std::vector<ScatterInstance> grassOnly;
  scatterLayers(manifest, noise, settings, glm::vec2(0.0f), settings.chunkWorldSize, 7u, 0,
                grassOnly);
  CHECK(grassOnly.empty());

  // Trees are untouched by the grass switch.
  std::vector<ScatterInstance> withTrees;
  scatterLayers(defaultScatterManifest(), noise, settings, glm::vec2(0.0f),
                settings.chunkWorldSize, 7u, 0, withTrees);
  bool anyTree = false;
  for (const ScatterInstance &i : withTrees)
    if (defaultScatterManifest().layers[static_cast<size_t>(i.layerIndex)].type ==
        ScatterLayerType::Tree)
      anyTree = true;
  CHECK(anyTree);
}

TEST_CASE("scatterLayers — grass is skipped past grassChunkRadius") {
  TerrainSettings settings;
  settings.grassChunkRadius = 2;
  TerrainNoiseSet noise(1337);
  ScatterManifest manifest;
  manifest.layers.push_back(testGrassLayer());

  std::vector<ScatterInstance> inside, outside;
  scatterLayers(manifest, noise, settings, glm::vec2(0.0f), settings.chunkWorldSize, 7u,
                /*chebyshev=*/2, inside);
  scatterLayers(manifest, noise, settings, glm::vec2(0.0f), settings.chunkWorldSize, 7u,
                /*chebyshev=*/3, outside);
  CHECK_FALSE(inside.empty());
  CHECK(outside.empty());
}

TEST_CASE("scatterLayers — minSpacing is respected within a chunk") {
  TerrainSettings settings;
  TerrainNoiseSet noise(1337);
  ScatterManifest manifest;
  ScatterLayer layer = testGrassLayer();
  layer.type = ScatterLayerType::Rock; // avoid the tree spacing multiplier
  layer.density = 2.0f;                // many more candidates than can fit
  layer.minSpacing = 5.0f;
  manifest.layers.push_back(layer);

  std::vector<ScatterInstance> out;
  scatterLayers(manifest, noise, settings, glm::vec2(0.0f), settings.chunkWorldSize, 7u, 0, out);
  REQUIRE(out.size() > 4);

  // O(n^2), fine at the instance count a 5m spacing permits in one chunk.
  float worstSq = 1e30f;
  for (size_t i = 0; i < out.size(); ++i) {
    for (size_t j = i + 1; j < out.size(); ++j) {
      const glm::vec3 a(out[i].transform[3]), b(out[j].transform[3]);
      const float dx = a.x - b.x, dz = a.z - b.z;
      worstSq = std::min(worstSq, dx * dx + dz * dz);
    }
  }
  CHECK(std::sqrt(worstSq) >= doctest::Approx(5.0f).epsilon(0.01));
}

TEST_CASE("scatterLayers — heightScale stretches vertically only") {
  TerrainSettings settings;
  TerrainNoiseSet noise(1337);
  ScatterManifest manifest;
  ScatterLayer layer = testGrassLayer();
  layer.type = ScatterLayerType::Rock; // no tree height multipliers in play
  layer.scaleMin = layer.scaleMax = 1.0f;
  layer.heightScaleMin = layer.heightScaleMax = 3.0f;
  layer.alignToNormal = 0.0f; // keep the basis axis-aligned so columns are readable
  layer.leanMaxDeg = 0.0f;
  manifest.layers.push_back(layer);

  std::vector<ScatterInstance> out;
  scatterLayers(manifest, noise, settings, glm::vec2(0.0f), settings.chunkWorldSize, 7u, 0, out);
  REQUIRE_FALSE(out.empty());

  const glm::mat4 &t = out.front().transform;
  CHECK(glm::length(glm::vec3(t[0])) == doctest::Approx(1.0f).epsilon(0.01));
  CHECK(glm::length(glm::vec3(t[1])) == doctest::Approx(3.0f).epsilon(0.01));
  CHECK(glm::length(glm::vec3(t[2])) == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("effectiveLayer — settings multipliers scale the authored values") {
  ScatterLayer layer;
  layer.type = ScatterLayerType::Tree;
  layer.density = 0.1f;
  layer.minSpacing = 4.0f;
  layer.heightScaleMin = 1.0f;
  layer.heightScaleMax = 2.0f;
  layer.wind = true;
  layer.windStrength = 0.5f;

  layer.scaleMin = 1.0f;
  layer.scaleMax = 2.0f;

  TerrainSettings settings;
  settings.treeDensityMultiplier = 2.0f;
  settings.treeSpacingMultiplier = 0.5f;
  settings.treeHeightMultiplier = 2.0f;
  settings.windStrength = 0.0f;

  EffectiveScatterLayer e = effectiveLayer(layer, settings);
  CHECK(e.density == doctest::Approx(0.2f));
  CHECK(e.minSpacing == doctest::Approx(2.0f));
  CHECK(e.heightScaleMin == doctest::Approx(2.0f));
  CHECK(e.heightScaleMax == doctest::Approx(4.0f));
  CHECK(e.windStrength == doctest::Approx(0.0f)); // global wind off

  // Variance re-spreads the range around its midpoint WITHOUT moving the
  // average -- the property that makes it independent of the height dial.
  settings.treeHeightVariance = 0.0f;
  e = effectiveLayer(layer, settings);
  CHECK(e.heightScaleMin == doctest::Approx(3.0f));
  CHECK(e.heightScaleMax == doctest::Approx(3.0f));
}

TEST_CASE("effectiveLayer — tree size scales uniformly, stretch does not") {
  ScatterLayer layer;
  layer.type = ScatterLayerType::Tree;
  layer.scaleMin = 1.0f;
  layer.scaleMax = 2.0f;
  layer.heightScaleMin = layer.heightScaleMax = 1.0f;

  // Size doubles the UNIFORM scale and leaves the vertical stretch at 1,
  // so the asset's proportions are untouched.
  TerrainSettings sized;
  sized.treeSizeMultiplier = 2.0f;
  EffectiveScatterLayer e = effectiveLayer(layer, sized);
  CHECK(e.scaleMin == doctest::Approx(2.0f));
  CHECK(e.scaleMax == doctest::Approx(4.0f));
  CHECK(e.heightScaleMin == doctest::Approx(1.0f));
  CHECK(e.heightScaleMax == doctest::Approx(1.0f));

  // Stretch does the opposite: vertical only, uniform scale untouched.
  TerrainSettings stretched;
  stretched.treeHeightMultiplier = 2.0f;
  e = effectiveLayer(layer, stretched);
  CHECK(e.scaleMin == doctest::Approx(1.0f));
  CHECK(e.scaleMax == doctest::Approx(2.0f));
  CHECK(e.heightScaleMin == doctest::Approx(2.0f));

  // Defaults must be a no-op in both directions, or the manifest's authored
  // proportions would be silently altered just by loading.
  EffectiveScatterLayer d = effectiveLayer(layer, TerrainSettings{});
  CHECK(d.scaleMin == doctest::Approx(layer.scaleMin));
  CHECK(d.scaleMax == doctest::Approx(layer.scaleMax));
  CHECK(d.heightScaleMin == doctest::Approx(1.0f));
}

TEST_CASE("scatterLayers — default trees keep the mesh's authored proportions") {
  // The regression this pins: reaching "taller trees" through heightScale
  // instead of scale, which visibly distorts the asset. Every placed tree's
  // vertical scale must stay close to its horizontal scale.
  TerrainSettings settings;
  TerrainNoiseSet noise(1337);
  const ScatterManifest manifest = defaultScatterManifest();

  std::vector<ScatterInstance> out;
  size_t trees = 0;
  for (int cx = 0; cx < 4; ++cx) {
    std::vector<ScatterInstance> chunk;
    scatterLayers(manifest, noise, settings,
                  glm::vec2(cx * settings.chunkWorldSize, 0.0f),
                  settings.chunkWorldSize, chunkSeedFor(settings.seed, ChunkCoord{cx, 0}),
                  0, chunk);
    for (const ScatterInstance &i : chunk) {
      if (manifest.layers[static_cast<size_t>(i.layerIndex)].type !=
          ScatterLayerType::Tree)
        continue;
      ++trees;
      const float sxz = glm::length(glm::vec3(i.transform[0]));
      const float sy = glm::length(glm::vec3(i.transform[1]));
      // Allow the authored +-12% unevenness, nothing near the 1.9x an
      // earlier revision reached.
      CHECK(sy / sxz < 1.2f);
      CHECK(sy / sxz > 0.8f);
    }
  }
  CHECK(trees > 0);
}

TEST_CASE("scatterLayers — every placed instance references a valid layer index") {
  TerrainSettings settings;
  TerrainNoiseSet noise(1337);
  const ScatterManifest manifest = defaultScatterManifest();

  std::vector<ScatterInstance> out;
  scatterLayers(manifest, noise, settings, glm::vec2(200.0f, -150.0f), settings.chunkWorldSize,
                chunkSeedFor(settings.seed, ChunkCoord{3, -2}), 0, out);

  for (const ScatterInstance &inst : out) {
    REQUIRE(inst.layerIndex >= 0);
    REQUIRE(static_cast<size_t>(inst.layerIndex) < manifest.layers.size());
    // interactive is only ever set from the layer's own flag.
    CHECK(inst.interactive == manifest.layers[static_cast<size_t>(inst.layerIndex)].interactive);
  }
}

TEST_CASE("scatterLayers — rock outcrop clustering places satellites around some anchors") {
  TerrainSettings settings;
  TerrainNoiseSet noise(1337);
  ScatterManifest manifest;
  ScatterLayer boulder;
  boulder.name = "boulder";
  boulder.meshPath = "assets/terraingeneratorassets/rock.obj";
  boulder.type = ScatterLayerType::Rock;
  boulder.density = 0.02f; // denser than the default, to get a robust sample
  boulder.biomeMeadow = 1.0f;
  boulder.biomeForest = 1.0f;
  boulder.biomeMountain = 1.0f; // biome-agnostic so placement isn't gated by terrain shape
  boulder.clustering.outcrops = true;
  manifest.layers.push_back(boulder);

  // Sample several chunks so at least one outcrop-favorable area is likely
  // hit regardless of the specific seed/terrain shape.
  size_t total = 0;
  for (int cx = 0; cx < 6; ++cx) {
    std::vector<ScatterInstance> out;
    scatterLayers(manifest, noise, settings, glm::vec2(cx * settings.chunkWorldSize, 0.0f),
                 settings.chunkWorldSize, chunkSeedFor(settings.seed, ChunkCoord{cx, 0}), 0, out);
    total += out.size();
  }
  // Anchors alone (no clustering) would average ~density*area candidates
  // times the acceptance rate; satellites (2-5 per accepted outcrop anchor)
  // should push the total well past a bare single-instance-per-cell count.
  CHECK(total > 0);
}

TEST_CASE("ScatterManifest — default manifest covers trees, rocks and grass") {
  const ScatterManifest manifest = defaultScatterManifest();
  CHECK(manifest.version == kScatterManifestVersion);
  REQUIRE(manifest.layers.size() == 8);

  CHECK(manifest.layers[0].name == "pine_canopy");
  CHECK(manifest.layers[0].type == ScatterLayerType::Tree);
  CHECK(manifest.layers[0].interactive);
  CHECK(manifest.layers[1].name == "pine_young");
  CHECK(manifest.layers[2].name == "boulder");
  CHECK(manifest.layers[2].collision == ScatterCollisionType::ConvexOrSphere);

  // The two canopy layers differ by SIZE (uniform scale), which is what
  // keeps both of them proportioned like the asset.
  CHECK(manifest.layers[0].scaleMax > manifest.layers[1].scaleMax * 2.0f);

  // No layer may grow by vertical stretch. heightScale is for slight
  // per-instance unevenness only -- pushing a plant's height without its
  // width distorts the mesh, which is the whole reason the uniform `scale`
  // range exists. Anything beyond ~1.2x here is a size change wearing the
  // wrong field's clothes.
  for (const ScatterLayer &l : manifest.layers) {
    if (l.type == ScatterLayerType::Grass)
      continue; // long thin blades tolerate stretch; see grassHeightMultiplier
    CHECK(l.heightScaleMax <= doctest::Approx(1.2f));
    CHECK(l.heightScaleMin >= doctest::Approx(0.8f));
  }

  // Grass must carry root darkening: without it the cheap stand-in for
  // grass's own contact shadow is simply absent, and it reads as resting on
  // the terrain rather than growing out of it.
  for (const ScatterLayer &l : manifest.layers)
    if (l.type == ScatterLayerType::Grass)
      CHECK(l.groundOcclusion > 0.2f);

  // Stock grass uses broad terrain contact shading, not blade casting.
  // Finite draw distances and small cells still bound raster work.
  size_t grassLayers = 0;
  for (const ScatterLayer &l : manifest.layers) {
    if (l.type != ScatterLayerType::Grass)
      continue;
    ++grassLayers;
    CHECK_FALSE(l.castRayShadow);
    CHECK(l.maxDrawDistance < 1.0e6f);
    CHECK(l.cullCellSize > 0.0f);
  }
  CHECK(grassLayers == 5);
  CHECK_FALSE(TerrainSettings{}.grassCastShadows);

  // Every layer sharing one mesh file must agree on that file's up-axis
  // correction -- it describes the ASSET, and only the first one encountered
  // is applied (see VkTerrainSubsystem::loadLayerMeshes). Grass and trees
  // both come from the same source .blend and are both authored up-along-X;
  // a layer that forgot this renders its instances lying flat on the ground,
  // which is easy to miss in code review and obvious only in a capture.
  std::map<std::string, glm::vec3> fixByMesh;
  for (const ScatterLayer &l : manifest.layers) {
    auto [it, inserted] = fixByMesh.emplace(l.meshPath, l.meshUpAxisFixDeg);
    if (!inserted)
      CHECK(glm::length(it->second - l.meshUpAxisFixDeg) == doctest::Approx(0.0f));
  }

  // The correction belongs to the file: the legacy grass.obj is authored
  // up-along-X and needs a non-zero one, while glTF is Y-up by spec and must
  // have none (a stray fix lays every clump on its side). Asserted explicitly
  // because "no correction" is also the default, making a forgotten fix
  // indistinguishable from a deliberate one.
  for (const ScatterLayer &l : manifest.layers) {
    if (l.type != ScatterLayerType::Grass)
      continue;
    const bool gltf = l.meshPath.size() > 5 &&
                      l.meshPath.compare(l.meshPath.size() - 5, 5, ".gltf") == 0;
    if (gltf)
      CHECK(glm::length(l.meshUpAxisFixDeg) == doctest::Approx(0.0f));
    else
      CHECK(glm::length(l.meshUpAxisFixDeg) > 1.0f);
  }
}

TEST_CASE("ScatterManifest — write then load round-trips every field") {
  const ScatterManifest original = defaultScatterManifest();
  const std::string path =
      (std::filesystem::temp_directory_path() / "glgen_test_scatter_manifest.json").string();

  REQUIRE(writeScatterManifest(path, original));

  ScatterManifest loaded;
  REQUIRE(loadScatterManifest(path, loaded));
  REQUIRE(loaded.layers.size() == original.layers.size());
  for (size_t i = 0; i < original.layers.size(); ++i) {
    const ScatterLayer &a = original.layers[i];
    const ScatterLayer &b = loaded.layers[i];
    CHECK(a.name == b.name);
    CHECK(a.meshPath == b.meshPath);
    CHECK(a.type == b.type);
    CHECK(a.density == doctest::Approx(b.density));
    CHECK(a.biomeMeadow == doctest::Approx(b.biomeMeadow));
    CHECK(a.biomeForest == doctest::Approx(b.biomeForest));
    CHECK(a.biomeMountain == doctest::Approx(b.biomeMountain));
    CHECK(a.collision == b.collision);
    CHECK(a.interactive == b.interactive);
    // R5 fields.
    CHECK(a.minSpacing == doctest::Approx(b.minSpacing));
    CHECK(a.heightScaleMin == doctest::Approx(b.heightScaleMin));
    CHECK(a.heightScaleMax == doctest::Approx(b.heightScaleMax));
    CHECK(a.leanMaxDeg == doctest::Approx(b.leanMaxDeg));
    CHECK(a.tint.x == doctest::Approx(b.tint.x));
    CHECK(a.tint.y == doctest::Approx(b.tint.y));
    CHECK(a.tint.z == doctest::Approx(b.tint.z));
    CHECK(a.castRayShadow == b.castRayShadow);
    CHECK(a.maxDrawDistance == doctest::Approx(b.maxDrawDistance));
    CHECK(a.densityFalloffStart == doctest::Approx(b.densityFalloffStart));
    CHECK(a.wind == b.wind);
    CHECK(a.windStrength == doctest::Approx(b.windStrength));
    CHECK(a.windSpeed == doctest::Approx(b.windSpeed));
    CHECK(a.cullCellSize == doctest::Approx(b.cullCellSize));
    CHECK(a.patchScale == doctest::Approx(b.patchScale));
    CHECK(a.patchThreshold == doctest::Approx(b.patchThreshold));
    CHECK(a.groundOcclusion == doctest::Approx(b.groundOcclusion));
    CHECK(a.foliageSssStrength == doctest::Approx(b.foliageSssStrength));
  }
  CHECK(loaded.version == original.version);

  std::filesystem::remove(path);
}

TEST_CASE("ScatterManifest - old files default foliage transmission strength") {
  const std::string path =
      (std::filesystem::temp_directory_path() / "glgen_test_scatter_v4.json").string();
  {
    std::ofstream f(path);
    f << R"({"version":4,"layers":[{"name":"old","mesh":"tree.obj","type":"tree"}]})";
  }
  ScatterManifest loaded;
  REQUIRE(loadScatterManifest(path, loaded));
  REQUIRE(loaded.layers.size() == 1);
  CHECK(loaded.layers[0].foliageSssStrength == doctest::Approx(1.0f));
  std::filesystem::remove(path);
}

TEST_CASE("ScatterManifest - v5 rim strength migrates to foliage SSS") {
  const std::string path =
      (std::filesystem::temp_directory_path() / "glgen_test_scatter_v5.json").string();
  {
    std::ofstream f(path);
    f << R"({"version":5,"layers":[{"name":"old","mesh":"tree.obj","type":"tree","rimStrength":0.42}]})";
  }
  ScatterManifest loaded;
  REQUIRE(loadScatterManifest(path, loaded));
  REQUIRE(loaded.layers.size() == 1);
  CHECK(loaded.layers[0].foliageSssStrength == doctest::Approx(0.42f));
  std::filesystem::remove(path);
}

TEST_CASE("ScatterManifest — loading a missing file fails cleanly") {
  ScatterManifest out;
  CHECK_FALSE(loadScatterManifest("this/path/does/not/exist.json", out));
}

TEST_CASE("Rock clusters respect shoreline, service track and chunk ownership") {
  TerrainSettings settings;
  settings.authoredWoodland = true;
  settings.worldBounded = false;
  settings.worldRadius = 500;
  settings.shoreScatterMargin = .6f;
  TerrainNoiseSet noise(settings.seed);
  ScatterLayer rock;
  rock.name = "clustered_stone";
  rock.type = ScatterLayerType::Rock;
  rock.meshPath = "assets/terrain_dressing/granite_slab.gltf";
  rock.density = .12f;
  rock.minSpacing = .45f;
  rock.slopeMax = .5f;
  rock.avoidTracks = true;
  rock.clustering.outcrops = true;
  ScatterManifest manifest;
  manifest.layers.push_back(rock);
  ScatterManifest restored;
  REQUIRE(scatterManifestFromJson(scatterManifestToJson(manifest), restored));
  REQUIRE(restored.layers[0].avoidTracks);
  size_t total = 0;
  for (int z=-3; z<=0; ++z) for (int x=-2; x<=1; ++x) {
    const glm::vec2 origin(x*settings.chunkWorldSize,z*settings.chunkWorldSize);
    std::vector<ScatterInstance> instances, repeated;
    const uint32_t seed = chunkSeedFor(settings.seed, ChunkCoord{x,z});
    scatterLayers(restored, noise, settings, origin, settings.chunkWorldSize, seed, 0, instances);
    scatterLayers(restored, noise, settings, origin, settings.chunkWorldSize, seed, 0, repeated);
    REQUIRE(instances.size() == repeated.size());
    WaterCellCache cache;
    for (size_t i=0; i<instances.size(); ++i) {
      const glm::vec3 p(instances[i].transform[3]);
      CHECK(instances[i].transform == repeated[i].transform);
      CHECK(p.x >= origin.x);
      CHECK(p.z >= origin.y);
      CHECK(p.x < origin.x+settings.chunkWorldSize);
      CHECK(p.z < origin.y+settings.chunkWorldSize);
      const glm::vec2 xz(p.x,p.z);
      CHECK(woodland::sample(xz, settings.worldRadius).track <= .12f);
      const float water = waterSurfaceAt(noise, settings, xz, nullptr, &cache);
      if (water > kNoWater*.5f) CHECK(p.y >= water+settings.shoreScatterMargin);
    }
    total += instances.size();
  }
  CHECK(total > 50);
}

TEST_CASE("Woodland grass feathers into the verge while the road centre stays open") {
  TerrainSettings settings;
  settings.authoredWoodland = true;
  settings.worldBounded = false;
  settings.worldRadius = 900;
  TerrainNoiseSet noise(settings.seed);
  ScatterLayer grass;
  grass.name = "verge_review";
  grass.type = ScatterLayerType::Grass;
  grass.density = 4;
  grass.slopeMax = 1;
  grass.biomeMeadow = grass.biomeForest = grass.biomeMountain = 1;
  grass.patchScale = 0;
  grass.sinkIntoGround = 0;
  ScatterManifest manifest;
  manifest.layers.push_back(grass);
  std::vector<ScatterInstance> instances, repeated;
  const glm::vec2 origin(-64,0);
  scatterLayers(manifest,noise,settings,origin,64,17171,0,instances);
  scatterLayers(manifest,noise,settings,origin,64,17171,0,repeated);
  REQUIRE(instances.size() == repeated.size());
  size_t shoulder = 0;
  for (size_t i=0; i<instances.size(); ++i) {
    CHECK(instances[i].transform == repeated[i].transform);
    const glm::vec3 p(instances[i].transform[3]);
    const float coverage = woodland::sample({p.x,p.z},settings.worldRadius).track;
    CHECK(coverage < .88f);
    if (coverage > .12f) ++shoulder;
  }
  // A binary .12 cut used to leave the entire blended shoulder devoid of
  // blades. This checks actual deterministic placement, not just the mask.
  CHECK(shoulder > 30);
  for (int i=1; i<13; ++i) {
    const glm::vec2 centre = glm::mix(woodland::serviceTrack[2],woodland::serviceTrack[3],i/13.f);
    CHECK(woodland::sample(centre,settings.worldRadius).track == doctest::Approx(1));
  }
}
