#include <doctest/doctest.h>

#include "AssetManager.h"
#include "MeshParse.h"

#include <memory>
#include <string>

// AssetManager's synthetic-asset API (registerMeshData / replaceMeshData /
// findMeshData / assetContentVersion) is the entry point for procedurally
// generated content -- see AI_ASSET_PIPELINE_PLAN.md Phase 0.
//
// The file parsers are stubbed rather than linked: none of the code under test
// touches them, and pulling them in would drag tinyobjloader/tinygltf/ufbx
// into a test target that is otherwise loader-free. Any test that DID need a
// real parse would be testing the parsers, not this.
std::unique_ptr<MeshData> parseMeshOBJ(const std::string &) { return nullptr; }
std::unique_ptr<MeshData> parseMeshGLTF(const std::string &) { return nullptr; }
std::unique_ptr<MeshData> parseMeshFBX(const std::string &) { return nullptr; }
std::unique_ptr<MeshData> parseMeshFile(const std::string &) { return nullptr; }

namespace {

// A one-triangle mesh whose vertex X coordinate is `marker`, so a test can
// tell one generation of an asset from the next by inspecting the geometry.
std::unique_ptr<MeshData> makeMarkerMesh(float marker) {
  auto data = std::make_unique<MeshData>();
  MeshSubmeshData sm;
  sm.objectName = "marker";
  sm.materialName = "default";
  sm.material.baseColor = glm::vec4(1.0f);
  sm.vertices = {
      {glm::vec3(marker, 0.0f, 0.0f), glm::vec2(0.0f), glm::vec3(0, 1, 0)},
      {glm::vec3(marker, 0.0f, 1.0f), glm::vec2(0.0f), glm::vec3(0, 1, 0)},
      {glm::vec3(marker, 1.0f, 0.0f), glm::vec2(0.0f), glm::vec3(0, 1, 0)},
  };
  for (const auto &v : sm.vertices) {
    sm.aabbMin = glm::min(sm.aabbMin, v.pos);
    sm.aabbMax = glm::max(sm.aabbMax, v.pos);
    sm.hasBounds = true;
  }
  data->submeshes.push_back(std::move(sm));
  return data;
}

float markerOf(const MeshData *data) {
  REQUIRE(data != nullptr);
  REQUIRE_FALSE(data->submeshes.empty());
  REQUIRE_FALSE(data->submeshes[0].vertices.empty());
  return data->submeshes[0].vertices[0].pos.x;
}

} // namespace

TEST_CASE("AssetManager — registerMeshData exposes the CPU mesh") {
  AssetManager assets;
  OBJHandle h = assets.registerMeshData("gen://test/a", makeMarkerMesh(1.0f));

  REQUIRE(h.valid());
  // The whole point of registerMeshData over the removed registerRuntimeOBJRaw:
  // the CPU data survives, which is what VulkanRenderSystem resolves through.
  CHECK(markerOf(assets.getOBJData(h)) == doctest::Approx(1.0f));
  CHECK(assets.getOBJData(h)->sourcePath == "gen://test/a");
  CHECK(assets.stats().objLive == 1);
  CHECK(assets.stats().objRuntimeLive == 1);
}

TEST_CASE("AssetManager — rejects empty ids and null data") {
  AssetManager assets;
  CHECK_FALSE(assets.registerMeshData("", makeMarkerMesh(1.0f)).valid());
  CHECK_FALSE(assets.registerMeshData("gen://test/a", nullptr).valid());
  CHECK(assets.stats().objLive == 0);
}

TEST_CASE("AssetManager — findMeshData looks up without touching the disk") {
  AssetManager assets;
  CHECK_FALSE(assets.findMeshData("gen://test/missing").valid());

  assets.registerMeshData("gen://test/a", makeMarkerMesh(2.0f));
  OBJHandle found = assets.findMeshData("gen://test/a");
  REQUIRE(found.valid());
  CHECK(markerOf(assets.getOBJData(found)) == doctest::Approx(2.0f));
}

TEST_CASE("AssetManager — replaceMeshData swaps geometry and keeps handles valid") {
  AssetManager assets;
  OBJHandle h = assets.registerMeshData("gen://test/a", makeMarkerMesh(1.0f));
  const uint32_t v0 = assets.assetContentVersion("gen://test/a");

  REQUIRE(assets.replaceMeshData("gen://test/a", makeMarkerMesh(9.0f)));

  // The pre-existing handle must still resolve -- this is the distinction
  // between content version and handle generation. If replace bumped
  // AssetHandle::generation instead, every MeshComponent already holding this
  // handle would silently render nothing.
  CHECK(markerOf(assets.getOBJData(h)) == doctest::Approx(9.0f));
  CHECK(assets.assetContentVersion("gen://test/a") > v0);
  // Replacing must not leak a second record.
  CHECK(assets.stats().objLive == 1);
}

TEST_CASE("AssetManager — replaceMeshData fails on unregistered ids") {
  AssetManager assets;
  CHECK_FALSE(assets.replaceMeshData("gen://test/nope", makeMarkerMesh(1.0f)));
  CHECK_FALSE(assets.replaceMeshData("gen://test/nope", nullptr));
  CHECK(assets.stats().objLive == 0);
}

TEST_CASE("AssetManager — re-registering an existing id replaces in place") {
  AssetManager assets;
  OBJHandle first = assets.registerMeshData("gen://test/a", makeMarkerMesh(1.0f));
  OBJHandle second = assets.registerMeshData("gen://test/a", makeMarkerMesh(5.0f));

  CHECK(first.index == second.index);
  CHECK(first.generation == second.generation);
  CHECK(markerOf(assets.getOBJData(first)) == doctest::Approx(5.0f));
  CHECK(assets.stats().objLive == 1);
}

TEST_CASE("AssetManager — content version tracks changes, not lookups") {
  AssetManager assets;
  CHECK(assets.assetContentVersion("gen://test/missing") == 0);

  assets.registerMeshData("gen://test/a", makeMarkerMesh(1.0f));
  const uint32_t v0 = assets.assetContentVersion("gen://test/a");
  CHECK(v0 != 0); // 0 is reserved for "unknown asset"

  // Reading is not a change.
  CHECK(assets.assetContentVersion("gen://test/a") == v0);

  assets.replaceMeshData("gen://test/a", makeMarkerMesh(2.0f));
  const uint32_t v1 = assets.assetContentVersion("gen://test/a");
  CHECK(v1 != v0);

  // In-place CPU mutations must bump it too, or a render system that already
  // uploaded the mesh keeps drawing the un-recentered geometry.
  assets.recenterOBJ(assets.findMeshData("gen://test/a"),
                     MeshData::Recenter::Center);
  const uint32_t v2 = assets.assetContentVersion("gen://test/a");
  CHECK(v2 != v1);

  assets.rotateOBJ(assets.findMeshData("gen://test/a"), glm::vec3(0, 90, 0));
  CHECK(assets.assetContentVersion("gen://test/a") != v2);
}

TEST_CASE("AssetManager — released slots are recycled, live ones are not") {
  AssetManager assets;
  OBJHandle a = assets.registerMeshData("gen://test/a", makeMarkerMesh(1.0f));
  assets.registerMeshData("gen://test/b", makeMarkerMesh(2.0f));
  CHECK(assets.stats().objLive == 2);

  REQUIRE(assets.releaseOBJ("gen://test/a"));
  CHECK(assets.stats().objLive == 1);
  CHECK(assets.assetContentVersion("gen://test/a") == 0);
  CHECK_FALSE(assets.getOBJData(a)); // released handle must not resolve

  // The freed slot is reused. The recycling predicate has to test CPU data as
  // well as the GPU pointer: a synthetic asset has no GPU model, so a
  // gpu-only test would hand out "gen://test/b"'s live slot here.
  OBJHandle c = assets.registerMeshData("gen://test/c", makeMarkerMesh(3.0f));
  CHECK(c.index == a.index);
  CHECK(assets.stats().objLive == 2);
  // b must be untouched.
  CHECK(markerOf(assets.getOBJData(assets.findMeshData("gen://test/b"))) ==
        doctest::Approx(2.0f));
  CHECK(markerOf(assets.getOBJData(c)) == doctest::Approx(3.0f));
}

TEST_CASE("AssetManager — synthetic assets coexist with primitive ids") {
  AssetManager assets;
  OBJHandle cube = assets.loadOBJ("__primitive_cube");
  OBJHandle gen = assets.registerMeshData("gen://test/a", makeMarkerMesh(4.0f));

  REQUIRE(cube.valid());
  REQUIRE(gen.valid());
  CHECK(cube.index != gen.index);
  CHECK_FALSE(assets.getOBJData(cube)->submeshes.empty());
  CHECK(markerOf(assets.getOBJData(gen)) == doctest::Approx(4.0f));
  // Primitives are parsed, not runtime-registered.
  CHECK(assets.stats().objLive == 2);
  CHECK(assets.stats().objRuntimeLive == 1);
}
