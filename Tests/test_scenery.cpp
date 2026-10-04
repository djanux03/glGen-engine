#include "../Engine/Scene/ScenePersistence.h"
#include "../VulkanRHI/runtime/ScenerySettings.h"
#include "ScatterManifest.h"
#include "TerrainNoise.h"
#include "WoodlandLayout.h"
#include "TerrainChunkMesher.h"
#include "TerrainScatter.h"
#include "TerrainQuery.h"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include "TerrainWater.h"
#include "TerrainChunkManager.h"
#include <chrono>
#include <thread>
#include <array>
#include <limits>

TEST_CASE("Scenery terrain settings round trip and partial definitions") {
  TerrainSettings original;
  original.seed = 729;
  original.forestCoverage = .7f;
  original.worldRadius = 2400;
  original.collisionUpdatesPerFrame = 7;
  scenery::Json saved;
  scenery::terrain(saved, original, false);
  TerrainSettings restored;
  scenery::terrain(saved, restored, true);
  scenery::Json again;
  scenery::terrain(again, restored, false);
  CHECK(saved == again);
  scenery::Json partial = {{"forestCoverage", .6}, {"seed", "invalid"}};
  scenery::terrain(partial, restored, true);
  CHECK(restored.seed == 729);
  CHECK(restored.worldRadius == 2400);
  CHECK(restored.forestCoverage == doctest::Approx(.6));
}
TEST_CASE("Snow reception is optional and survives scatter persistence") {
  auto m = defaultScatterManifest();
  for (auto &l : m.layers)
    CHECK_FALSE(l.receivesSnow);
  m.layers[0].receivesSnow = true;
  auto json = scatterManifestToJson(m);
  ScatterManifest restored;
  REQUIRE(scatterManifestFromJson(json, restored));
  CHECK(restored.layers[0].receivesSnow);
  json["layers"][0].erase("receivesSnow");
  REQUIRE(scatterManifestFromJson(json, restored));
  CHECK_FALSE(restored.layers[0].receivesSnow);
}
TEST_CASE("Shipped winter definition preserves deterministic scatter and "
          "shared assets") {
  auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
              "assets/scenery/bleak_winter.json";
  std::ifstream file(path);
  scenery::Json j;
  file >> j;
  REQUIRE(j["version"] == 1);
  TerrainSettings s;
  scenery::terrain(j["terrain"], s, true);
  ScatterManifest m;
  REQUIRE(scatterManifestFromJson(j["scatter"], m));
  ScatterManifest summer;
  REQUIRE(loadScatterManifest(
      (path.parent_path().parent_path().parent_path() / "terrain_scatter.json")
          .string(),
      summer));
  REQUIRE(m.layers.size() == summer.layers.size());
  for (size_t i = 0; i < m.layers.size(); ++i) {
    CHECK(m.layers[i].meshPath == summer.layers[i].meshPath);
    if (m.layers[i].type == ScatterLayerType::Grass)
      CHECK_FALSE(m.layers[i].receivesSnow);
  }
  TerrainNoiseSet noise(s.seed);
  std::vector<ScatterInstance> a, b;
  scatterLayers(m, noise, s, {0, 0}, s.chunkWorldSize, 173, 0, a);
  scatterLayers(m, noise, s, {0, 0}, s.chunkWorldSize, 173, 0, b);
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    CHECK(a[i].layerIndex == b[i].layerIndex);
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r)
        CHECK(a[i].transform[c][r] == b[i].transform[c][r]);
  }
}

TEST_CASE("The Long Dark terrain height and camera placement") {
  auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
              "assets/scenery/the_long_dark.json";
  std::ifstream file(path);
  scenery::Json j;
  file >> j;
  REQUIRE(j["version"] == 1);
  TerrainSettings s;
  scenery::terrain(j["terrain"], s, true);
  TerrainNoiseSet noise(s.seed);
  TerrainQuery q(s, noise, nullptr);
  float h00 = q.heightAt(glm::vec2(0.0f, 0.0f));
  float hCam = q.heightAt(glm::vec2(6.0f, 8.0f));
  // Terrain around the spawn / editor origin should remain at a sensible elevation (under 80m),
  // so the editor camera and player start safely above the surface.
  CHECK(h00 < 80.0f);
  CHECK(hCam < 80.0f);
  CHECK(s.mountainCoverage <= 0.40f);
}

TEST_CASE("Woodland swamp pins landforms, clearings and shared water authority") {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  std::ifstream file(root / "assets/scenery/woodland_swamp.json");
  scenery::Json j;
  file >> j;
  TerrainSettings s;
  scenery::terrain(j["terrain"], s, true);
  REQUIRE(s.authoredWoodland);
  REQUIRE(j["ownsWorldShape"] == true);
  TerrainNoiseSet a(s.seed), b(98765);
  TerrainQuery qa(s, a, nullptr), qb(s, b, nullptr);
  CHECK(qa.heightAt({0, -80}) < -1.0f);
  CHECK(qa.heightAt({-145, -78}) < 0.0f);
  CHECK(qa.heightAt({58, -205}) < 0.0f);
  CHECK(qa.heightAt({0, 12}) > 1.0f);
  CHECK(qa.heightAt({170, -100}) > 5.0f);
  CHECK(waterSurfaceAt(a, s, {0, -80}, nullptr, nullptr) == 0.0f);
  const auto clearing = sampleBiomeWeights(sampleMacro(a, {0, 12}, s), s, 3);
  const auto forest = sampleBiomeWeights(sampleMacro(a, {170, -100}, s), s, 8);
  CHECK(clearing.forest < .15f);
  CHECK(forest.forest > .4f);
  for (int z = -280; z <= 128; z += 8)
    for (int x = -240; x <= 256; x += 8) {
      const glm::vec2 p(x, z);
      CHECK(qa.heightAt(p) == qb.heightAt(p));
      CHECK(std::isfinite(qa.heightAt(p)));
    }
  scenery::Json saved;
  scenery::terrain(saved, s, false);
  CHECK(saved["authoredWoodland"] == true);

  ScatterManifest manifest;
  REQUIRE(scatterManifestFromJson(j["scatter"], manifest));
  std::vector<ScatterInstance> first, second;
  scatterLayers(manifest, a, s, {-192, -128}, s.chunkWorldSize, 173, 0, first);
  scatterLayers(manifest, a, s, {-192, -128}, s.chunkWorldSize, 173, 0, second);
  REQUIRE(first.size() > 100);
  REQUIRE(first.size() == second.size());
  for (size_t i = 0; i < first.size(); ++i) {
    const auto &v = first[i];
    for (int col = 0; col < 4; ++col)
      for (int row = 0; row < 4; ++row)
        CHECK(v.transform[col][row] == second[i].transform[col][row]);
    const glm::vec2 p(v.transform[3].x, v.transform[3].z);
    CHECK(qa.heightAt(p) >= s.seaLevel + s.shoreScatterMargin);
    if (manifest.layers[v.layerIndex].type == ScatterLayerType::Grass)
      CHECK_FALSE(manifest.layers[v.layerIndex].castRayShadow);
  }
}

TEST_CASE("Woodland trees survive coarse terrain detail on the opposite bank") {
  TerrainSettings s;
  s.authoredWoodland = true;
  s.worldBounded = false;
  s.autoSeaLevel = false;
  s.lakesEnabled = false;
  s.spawnGrass = false;
  s.chunkResolution = 17;
  s.viewDistanceChunks = 3;
  s.workerThreads = 1;
  s.treeDensityMultiplier = 1;
  ScatterManifest m;
  ScatterLayer trees;
  trees.type = ScatterLayerType::Tree;
  trees.density = .02f;
  trees.biomeForest = trees.biomeMeadow = 1;
  trees.moistureMin = 0;
  trees.slopeMax = 1;
  trees.clustering.stands = false;
  m.layers.push_back(trees);
  TerrainChunkManager manager;
  manager.init(s, m);
  bool oppositeBankFound = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!oppositeBankFound && std::chrono::steady_clock::now() < deadline) {
    manager.streamUpdate({170, 20, 100});
    for (const auto &upload : manager.takePendingUploads())
      if (upload.coord == ChunkCoord{4, 1}) {
        CHECK(upload.lod > 0);
        CHECK_FALSE(upload.scatter.empty());
        oppositeBankFound = true;
      }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(oppositeBankFound);
}

TEST_CASE("Woodland layered grass stays bounded and preserves walking routes") {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  std::ifstream file(root / "assets/scenery/woodland_swamp.json");
  scenery::Json j; file >> j;
  TerrainSettings settings; scenery::terrain(j["terrain"], settings, true);
  ScatterManifest all, grass;
  REQUIRE(scatterManifestFromJson(j["scatter"], all));
  for (const auto &layer : all.layers)
    if (layer.type == ScatterLayerType::Grass) grass.layers.push_back(layer);
  TerrainNoiseSet noise(settings.seed);
  std::vector<ScatterInstance> cover;
  scatterLayers(grass,noise,settings,{-32,0},64,175,0,cover);
  CHECK(cover.size() > 100);
  // Short cover intentionally closes the formerly bare gaps. Bound the
  // complete layered density while retaining actual track/rock exclusions.
  CHECK(cover.size() < 64*64*4);
  CHECK(std::any_of(cover.begin(),cover.end(),[&](const auto &i){
    return grass.layers[i.layerIndex].name == "grass_low_sward";
  }));
  for (const auto &instance : cover) {
    const glm::vec2 p(instance.transform[3].x,instance.transform[3].z);
    const auto habitat = woodland::sample(p,settings.worldRadius);
    // Fine grass is allowed through the eroded shoulder, but the compacted
    // centre remains open. Large trees and rocks still use the .12 exclusion.
    CHECK(habitat.track < .88f);
    CHECK(habitat.rock <= .35f);
    if (grass.layers[instance.layerIndex].moistureMin > .7f)
      CHECK(habitat.moisture >= .78f);
  }
}

TEST_CASE("Fixed landmarks belong to exactly one chunk and survive JSON round trips") {
  auto j = scatterManifestToJson(defaultScatterManifest());
  j["layers"] = scenery::Json::array({{{"name","landmark"},{"mesh","landmark.gltf"},{"type","rock"},
      {"density",0},{"fixedPlacements",scenery::Json::array({
      {64,32,90,2}, {3,4,0,-1}, {"bad",0,0,1}})}}});
  ScatterManifest m, restored;
  REQUIRE(scatterManifestFromJson(j,m));
  REQUIRE(m.layers[0].fixedPlacements.size()==1);
  REQUIRE(scatterManifestFromJson(scatterManifestToJson(m),restored));
  CHECK(scatterManifestToJson(m)==scatterManifestToJson(restored));
  TerrainSettings settings; settings.spawnVegetation=true;
  TerrainNoiseSet noise(75);
  std::vector<ScatterInstance> left,right;
  scatterLayers(m,noise,settings,{0,0},64,1,0,left);
  scatterLayers(m,noise,settings,{64,0},64,991,0,right);
  CHECK(left.empty()); REQUIRE(right.size()==1);
  CHECK(right[0].transform[3].x==64);
  CHECK(right[0].transform[3].z==32);
  CHECK(glm::length(glm::vec3(right[0].transform[0]))==doctest::Approx(2));
}

TEST_CASE("Authored surface masks stay continuous across independently built chunks") {
  TerrainSettings settings; settings.authoredWoodland=true; settings.worldBounded=false;
  settings.autoSeaLevel=false;settings.lakesEnabled=false;
  TerrainNoiseSet noise(123);
  std::vector<float> leftH,rightH;
  std::vector<TerrainGroundFields> leftF,rightF;
  sampleHeightGrid(noise,settings,{-64,0},64,33,nullptr,leftH,&leftF);
  sampleHeightGrid(noise,settings,{0,0},64,33,nullptr,rightH,&rightF);
  for(int i=0;i<33;++i){
    CHECK(leftF[i*33+32].authoredTrack==rightF[i*33].authoredTrack);
    CHECK(leftF[i*33+32].authoredRock==rightF[i*33].authoredRock);
    CHECK(leftH[i*33+32]==rightH[i*33]);
  }
  auto mesh=buildTerrainChunkMesh(rightH,rightF,33,64,.4f,4,0);
  CHECK(mesh.submeshes[0].vertices[6*33].uv.x==doctest::Approx(1));
}

TEST_CASE("Scatter mesh LODs persist, sort, and reject unusable definitions") {
  auto j = scatterManifestToJson(defaultScatterManifest());
  j["layers"][0]["shadowMesh"] = "sparse_shadow.gltf";
  j["layers"][0]["meshLods"] = scenery::Json::array({
      {{"mesh", "far.gltf"}, {"distance", 140}},
      {{"mesh", "mid.gltf"}, {"distance", 40}},
      {{"mesh", ""}, {"distance", 10}},
      {{"mesh", "bad.gltf"}, {"distance", -5}},
      "invalid"});
  ScatterManifest m;
  REQUIRE(scatterManifestFromJson(j, m));
  CHECK(m.layers[0].shadowMeshPath == "sparse_shadow.gltf");
  REQUIRE(m.layers[0].meshLods.size() == 2);
  CHECK(m.layers[0].meshLods[0].meshPath == "mid.gltf");
  CHECK(m.layers[0].meshLods[1].distance == 140);
  auto saved = scatterManifestToJson(m);
  ScatterManifest restored;
  REQUIRE(scatterManifestFromJson(saved, restored));
  CHECK(scatterManifestToJson(restored) == saved);
  j["layers"][0].erase("meshLods");
  j["layers"][0].erase("shadowMesh");
  REQUIRE(scatterManifestFromJson(j, restored));
  CHECK(restored.layers[0].meshLods.empty());
  CHECK(restored.layers[0].shadowMeshPath.empty());
}

TEST_CASE("Woodland LOD assets retain dense placements with bounded geometry cost") {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  std::ifstream file(root / "assets/scenery/woodland_swamp.json");
  scenery::Json j;
  file >> j;
  ScatterManifest m;
  REQUIRE(scatterManifestFromJson(j["scatter"], m));
  auto triangles = [&](const std::string &path) {
    std::ifstream meshFile(root / path);
    REQUIRE(meshFile.good());
    scenery::Json mesh;
    meshFile >> mesh;
    size_t count = 0;
    for (const auto &part : mesh["meshes"][0]["primitives"])
      count += mesh["accessors"][part["indices"].get<size_t>()]["count"].get<size_t>() / 3;
    return count;
  };
  auto bounds = [&](const std::string &path) {
    std::ifstream meshFile(root / path);
    scenery::Json mesh; meshFile >> mesh;
    std::array<double,6> result;
    for (size_t j=0;j<3;++j) {
      result[j]=std::numeric_limits<double>::infinity();
      result[j+3]=-result[j];
    }
    for (const auto &part : mesh["meshes"][0]["primitives"]) {
      const auto &positions=mesh["accessors"][part["attributes"]["POSITION"].get<size_t>()];
      for(size_t j=0;j<3;++j) {
        result[j]=std::min(result[j],positions["min"][j].get<double>());
        result[j+3]=std::max(result[j+3],positions["max"][j].get<double>());
      }
    }
    return result;
  };
  for (const auto &layer : m.layers) {
    if (layer.meshLods.empty()) continue;
    REQUIRE(layer.meshLods.size() == 2);
    const auto full = triangles(layer.meshPath);
    if (!layer.shadowMeshPath.empty()) CHECK(triangles(layer.shadowMeshPath) < full / 5);
    if (layer.meshPath.find("assets/grass/")==0) {
      // Import recentres from these bounds. A mismatched sparse subset moved
      // grass/shadow roots sideways and made the patches jump at LOD changes.
      const auto shared=bounds(layer.meshPath);
      if (!layer.shadowMeshPath.empty()) CHECK(bounds(layer.shadowMeshPath)==shared);
      for(const auto &lod:layer.meshLods) CHECK(bounds(lod.meshPath)==shared);
    }
    const auto middle = triangles(layer.meshLods[0].meshPath);
    const auto far = triangles(layer.meshLods[1].meshPath);
    CHECK(middle < full / 3);
    CHECK(far < full / 10);
    CHECK(layer.meshLods[0].distance < layer.meshLods[1].distance);
    CHECK(layer.meshLods[1].distance < layer.maxDrawDistance);
  }
}

TEST_CASE("Saved woodland settings adopt detail assets while preserving authoring") {
  scenery::Json definition = {{"scatter", {{"layers", scenery::Json::array({
      {{"name", "trees"}, {"mesh", "tree.gltf"}, {"density", .02},
       {"cullCellSize", 16}, {"meshLods", scenery::Json::array({
          {{"mesh", "tree_far.gltf"}, {"distance", 90}}})}}})}}}};
  scenery::Json saved = definition;
  auto &old = saved["scatter"]["layers"][0];
  old.erase("meshLods"); old["density"] = .06; old["cullCellSize"] = 0;
  auto restored = definition;
  scenery::mergeSavedProfile(restored, saved);
  CHECK(restored["scatter"]["layers"][0]["density"] == .06);
  CHECK(restored["scatter"]["layers"][0]["cullCellSize"] == 16);
  CHECK(restored["scatter"]["layers"][0]["meshLods"] ==
        definition["scatter"]["layers"][0]["meshLods"]);
  old["meshLods"] = scenery::Json::array();
  restored = definition; scenery::mergeSavedProfile(restored, saved);
  CHECK(restored["scatter"]["layers"][0]["meshLods"].empty());
  old.erase("meshLods"); old["mesh"] = "custom_tree.gltf";
  restored = definition; scenery::mergeSavedProfile(restored, saved);
  CHECK_FALSE(restored["scatter"]["layers"][0].contains("meshLods"));
}

TEST_CASE("Legacy streamed scene proxies are excluded without discarding authored nodes") {
  using scenePersistence::isLegacyTerrainProxy;
  scenery::Json marker = {{"id", 14}, {"name", "TerrainChunk_-12_4"},
      {"transform", {{"position", {0,0,0}}}},
      {"hierarchy", {{"parent", 0}, {"children", scenery::Json::array()}}}};
  CHECK(isLegacyTerrainProxy(marker));
  marker["mesh"] = {{"type", "None"}, {"assetId", "TerrainChunk_-12_4"}};
  CHECK(isLegacyTerrainProxy(marker));
  marker["mesh"] = {{"type", "GLTF"}, {"assetId", "my_terrain.gltf"}};
  CHECK_FALSE(isLegacyTerrainProxy(marker));
  marker.erase("mesh"); marker["name"] = "TerrainChunk_garden";
  CHECK_FALSE(isLegacyTerrainProxy(marker));
  scenery::Json rock = {{"name", "CollidableRock"},
      {"rigidbody", {{"type", "Static"}}}, {"collider", {{"shape", "Sphere"}}}};
  CHECK(isLegacyTerrainProxy(rock));
  auto authored = rock; authored["script"] = {{"scriptPath", "gameplay.lua"}};
  CHECK_FALSE(isLegacyTerrainProxy(authored));
  authored = rock; authored["hierarchy"] = {{"parent", 8}};
  CHECK_FALSE(isLegacyTerrainProxy(authored));
  authored = rock; authored["rigidbody"]["type"] = "Dynamic";
  CHECK_FALSE(isLegacyTerrainProxy(authored));
  authored = rock; authored["name"] = "Garden rock";
  CHECK_FALSE(isLegacyTerrainProxy(authored));
  rock["name"] = "InteractiveTree"; rock["collider"]["shape"] = "Capsule";
  CHECK(isLegacyTerrainProxy(rock));
}
