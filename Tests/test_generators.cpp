#include <doctest/doctest.h>

#include "AssetLibrary.h"
#include "AssetManager.h"
#include "AssetRecipe.h"
#include "GenRandom.h"
#include "GeneratorRegistry.h"
#include "GeneratorSchema.h"
#include "MeshBuilder.h"
#include "TextureGen.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace gen;
using json = nlohmann::json;

namespace {

size_t triCount(const MeshData &m) {
  size_t n = 0;
  for (const auto &sm : m.submeshes)
    n += (sm.indices.empty() ? sm.vertices.size() : sm.indices.size()) / 3;
  return n;
}

// Bitwise vertex comparison -- determinism means identical, not close.
bool identical(const MeshData &a, const MeshData &b) {
  if (a.submeshes.size() != b.submeshes.size())
    return false;
  for (size_t s = 0; s < a.submeshes.size(); ++s) {
    const auto &x = a.submeshes[s];
    const auto &y = b.submeshes[s];
    if (x.vertices.size() != y.vertices.size() ||
        x.indices.size() != y.indices.size())
      return false;
    for (size_t i = 0; i < x.vertices.size(); ++i) {
      if (x.vertices[i].pos != y.vertices[i].pos ||
          x.vertices[i].normal != y.vertices[i].normal ||
          x.vertices[i].uv != y.vertices[i].uv)
        return false;
    }
    if (x.indices != y.indices)
      return false;
  }
  return true;
}

AssetRecipe makeRecipe(const std::string &generator, json params = json::object(),
                       uint32_t seed = 1234) {
  AssetRecipe r;
  r.generator = generator;
  r.params = std::move(params);
  r.seed = seed;
  return r;
}

} // namespace

// ── GenRandom ──────────────────────────────────────────────────────────────

TEST_CASE("GenRandom — same seed reproduces the same stream") {
  GenRandom a(42), b(42), c(43);
  bool diverged = false;
  for (int i = 0; i < 64; ++i) {
    const float x = a.unit(), y = b.unit(), z = c.unit();
    CHECK(x == y);
    if (x != z)
      diverged = true;
    CHECK(x >= 0.0f);
    CHECK(x < 1.0f);
  }
  CHECK(diverged); // a different seed must actually produce a different stream
}

TEST_CASE("GenRandom — inSphere stays inside the unit sphere") {
  GenRandom rng(7);
  for (int i = 0; i < 200; ++i) {
    const glm::vec3 v = rng.inSphere();
    CHECK(glm::dot(v, v) <= 1.0f + 1e-5f);
  }
}

// ── MeshBuilder ────────────────────────────────────────────────────────────

TEST_CASE("MeshBuilder — box is indexed with correct counts and bounds") {
  MeshBuilder b;
  b.addBox(glm::vec3(-1.0f), glm::vec3(1.0f));
  CHECK(b.triangleCount() == 12);
  CHECK(b.vertexCount() == 24); // per-face vertices, for hard normals

  auto mesh = b.build();
  REQUIRE(mesh->submeshes.size() == 1);
  const auto &sm = mesh->submeshes[0];
  CHECK_FALSE(sm.indices.empty()); // indexed, unlike MeshPrimitives' soup
  CHECK(sm.hasBounds);
  CHECK(sm.aabbMin.x == doctest::Approx(-1.0f));
  CHECK(sm.aabbMax.y == doctest::Approx(1.0f));

  // Every index must be in range, or the renderer reads garbage vertices.
  for (uint32_t i : sm.indices)
    CHECK(i < sm.vertices.size());
}

TEST_CASE("MeshBuilder — transform stack composes and unwinds") {
  MeshBuilder b;
  b.pushTransform(glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)));
  b.pushTransform(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 5.0f, 0.0f)));
  b.addBox(glm::vec3(-0.5f), glm::vec3(0.5f));
  b.popTransform();
  b.popTransform();
  b.addBox(glm::vec3(-0.5f), glm::vec3(0.5f));

  auto mesh = b.build();
  glm::vec3 mn, mx;
  REQUIRE(mesh->getGlobalBounds(mn, mx));
  // First box sits at (10,5,0), second at the origin.
  CHECK(mx.x == doctest::Approx(10.5f));
  CHECK(mx.y == doctest::Approx(5.5f));
  CHECK(mn.x == doctest::Approx(-0.5f));
}

TEST_CASE("MeshBuilder — popping past the base transform is harmless") {
  MeshBuilder b;
  // The identity at the bottom of the stack must survive over-popping, or
  // every later emit reads a dangling transform.
  b.popTransform();
  b.popTransform();
  b.addBox(glm::vec3(-1.0f), glm::vec3(1.0f));
  auto mesh = b.build();
  glm::vec3 mn, mx;
  REQUIRE(mesh->getGlobalBounds(mn, mx));
  CHECK(mn.x == doctest::Approx(-1.0f));
}

TEST_CASE("MeshBuilder — normals are unit length across primitives") {
  MeshBuilder b;
  b.addTaperedCylinder(glm::vec3(0.0f), 1.0f, glm::vec3(0.0f, 4.0f, 0.0f), 0.2f, 8);
  b.addIcosphere(glm::vec3(5.0f, 0.0f, 0.0f), 1.0f, 2);
  b.addCard(0.2f, 1.0f, 45.0f, 3);
  b.addRevolve({{0.0f, 0.0f}, {1.0f, 0.5f}, {0.7f, 1.5f}, {0.0f, 2.0f}}, 8);
  auto mesh = b.build();
  for (const auto &sm : mesh->submeshes)
    for (const auto &v : sm.vertices)
      CHECK(glm::length(v.normal) == doctest::Approx(1.0f).epsilon(0.01f));
}

TEST_CASE("MeshBuilder — separate submeshes keep separate materials") {
  MaterialAsset bark;
  bark.id = "bark";
  bark.baseColor = glm::vec4(0.3f, 0.2f, 0.1f, 1.0f);
  MaterialAsset leaf;
  leaf.id = "foliage";
  leaf.baseColor = glm::vec4(0.1f, 0.5f, 0.1f, 1.0f);

  MeshBuilder b;
  b.beginSubmesh("Bark", bark);
  b.addBox(glm::vec3(-1.0f), glm::vec3(1.0f));
  b.beginSubmesh("Foliage", leaf);
  b.addBox(glm::vec3(-2.0f), glm::vec3(2.0f));

  auto mesh = b.build();
  REQUIRE(mesh->submeshes.size() == 2);
  CHECK(mesh->submeshes[0].material.id == "bark");
  CHECK(mesh->submeshes[1].material.id == "foliage");
  // Empty submeshes must be dropped, not emitted as zero-triangle draws.
  MeshBuilder c;
  c.beginSubmesh("Empty", bark);
  CHECK(c.build()->submeshes.empty());
}

// ── Schema validation ──────────────────────────────────────────────────────

TEST_CASE("Schema — defaults fill in for absent parameters") {
  const json schema = SchemaBuilder()
                          .number("height", 8.0f, 1.0f, 20.0f, "h")
                          .enumString("style", "a", {"a", "b"}, "s")
                          .schema();
  json params = json::object();
  std::vector<std::string> warnings;
  std::string error;
  REQUIRE(validateParams(schema, params, warnings, error));
  CHECK(params["height"].get<float>() == doctest::Approx(8.0f));
  CHECK(params["style"].get<std::string>() == "a");
  CHECK(warnings.empty());
}

TEST_CASE("Schema — out-of-range numbers clamp and warn rather than fail") {
  const json schema =
      SchemaBuilder().number("height", 8.0f, 1.0f, 20.0f, "h").schema();
  json params;
  params["height"] = 500.0;
  std::vector<std::string> warnings;
  std::string error;

  // Clamping keeps the caller productive: they get a usable asset plus text
  // saying what was adjusted, which is what lets an AI self-correct.
  REQUIRE(validateParams(schema, params, warnings, error));
  CHECK(params["height"].get<float>() == doctest::Approx(20.0f));
  REQUIRE(warnings.size() == 1);
  CHECK(warnings[0].find("clamped") != std::string::npos);
}

TEST_CASE("Schema — wrong types and bad enum values are hard errors") {
  const json schema = SchemaBuilder()
                          .number("height", 8.0f, 1.0f, 20.0f, "h")
                          .enumString("style", "a", {"a", "b"}, "s")
                          .schema();
  {
    json params;
    params["height"] = "tall";
    std::vector<std::string> warnings;
    std::string error;
    CHECK_FALSE(validateParams(schema, params, warnings, error));
    CHECK(error.find("must be a number") != std::string::npos);
  }
  {
    // Not clamped to a default: silently substituting a different discrete
    // choice would change what the asset IS with no signal to the caller.
    json params;
    params["style"] = "c";
    std::vector<std::string> warnings;
    std::string error;
    CHECK_FALSE(validateParams(schema, params, warnings, error));
    CHECK(error.find("not one of") != std::string::npos);
  }
}

TEST_CASE("Schema — unknown parameters warn but do not fail") {
  const json schema =
      SchemaBuilder().number("height", 8.0f, 1.0f, 20.0f, "h").schema();
  json params;
  params["heigth"] = 5.0; // typo
  std::vector<std::string> warnings;
  std::string error;
  REQUIRE(validateParams(schema, params, warnings, error));
  REQUIRE(warnings.size() == 1);
  CHECK(warnings[0].find("heigth") != std::string::npos);
}

TEST_CASE("Schema — integers round and colors clamp per element") {
  const json schema = SchemaBuilder()
                          .integer("count", 3, 1, 10, "c")
                          .color("tint", glm::vec3(1.0f), "t")
                          .schema();
  json params;
  params["count"] = 4.7;
  params["tint"] = json::array({2.0, -1.0, 0.5});
  std::vector<std::string> warnings;
  std::string error;
  REQUIRE(validateParams(schema, params, warnings, error));
  CHECK(params["count"].get<int>() == 5);
  CHECK(params["tint"][0].get<float>() == doctest::Approx(1.0f));
  CHECK(params["tint"][1].get<float>() == doctest::Approx(0.0f));
  CHECK(params["tint"][2].get<float>() == doctest::Approx(0.5f));
}

// ── Recipes ────────────────────────────────────────────────────────────────

TEST_CASE("Recipe — hash covers appearance, ignores annotation") {
  AssetRecipe a = makeRecipe("rock.v1");
  a.params["radius"] = 1.0;
  AssetRecipe b = a;
  const std::string h0 = recipeHash(a);

  b.provenance["prompt"] = "a big rock";
  b.id = "rock/big";
  // Renaming or annotating must not invalidate a cached mesh.
  CHECK(recipeHash(b) == h0);

  b.params["radius"] = 1.5;
  CHECK(recipeHash(b) != h0);

  AssetRecipe c = a;
  c.seed = a.seed + 1;
  CHECK(recipeHash(c) != h0);
}

TEST_CASE("Recipe — named ids are stable, anonymous ones are content-addressed") {
  AssetRecipe named = makeRecipe("tree.v1");
  named.id = "tree/oak";
  const std::string id0 = assetIdFor(named);
  CHECK(id0 == "gen://tree.v1/tree/oak");

  // The whole point: editing a named recipe keeps its asset id, so entities
  // already referencing it pick up the new geometry instead of losing it.
  named.params["height"] = 12.0;
  CHECK(assetIdFor(named) == id0);

  AssetRecipe anon = makeRecipe("tree.v1");
  const std::string anonId = assetIdFor(anon);
  CHECK(anonId != id0);
  anon.params["height"] = 12.0;
  CHECK(assetIdFor(anon) != anonId); // content-addressed: content moved
}

TEST_CASE("Recipe — parse round-trips and rejects malformed input") {
  json j;
  j["generator"] = "rock.v1";
  j["id"] = "rock/a";
  j["seed"] = 99;
  j["params"]["radius"] = 2.0;

  AssetRecipe r;
  std::string error;
  REQUIRE(parseRecipe(j, r, error));
  CHECK(r.generator == "rock.v1");
  CHECK(r.id == "rock/a");
  CHECK(r.seed == 99u);
  CHECK(r.params["radius"].get<float>() == doctest::Approx(2.0f));

  AssetRecipe round;
  REQUIRE(parseRecipe(recipeToJson(r), round, error));
  CHECK(recipeHash(round) == recipeHash(r));

  json bad;
  bad["seed"] = 1;
  CHECK_FALSE(parseRecipe(bad, r, error));
  CHECK(error.find("generator") != std::string::npos);
}

// ── Generators ─────────────────────────────────────────────────────────────

TEST_CASE("Registry — built-ins are registered with schemas") {
  const auto names = GeneratorRegistry::instance().names();
  for (const char *expected :
       {"tree.v1", "rock.v1", "grass.v1", "kitbash.v1", "sculpt.v1",
        "sweep.v1", "revolve.v1", "panel.v1", "character.v1", "import.gltf"}) {
    CAPTURE(expected);
    CHECK(std::find(names.begin(), names.end(), expected) != names.end());
    const GeneratorInfo *info = GeneratorRegistry::instance().find(expected);
    REQUIRE(info != nullptr);
    // The schema doubles as the MCP tool description, so both must be present.
    CHECK_FALSE(info->description.empty());
    CHECK(info->schema.contains("properties"));
  }
}

TEST_CASE("Registry — unknown generator fails cleanly") {
  GenResult r = GeneratorRegistry::instance().run("nope.v9", json::object(), 1);
  CHECK_FALSE(r.ok());
  CHECK(r.error.find("unknown generator") != std::string::npos);
  CHECK(r.mesh == nullptr);
}

TEST_CASE("Generators — output is deterministic for a given seed") {
  for (const char *name : {"tree.v1", "rock.v1", "grass.v1"}) {
    CAPTURE(name);
    GenResult a = GeneratorRegistry::instance().run(name, json::object(), 555);
    GenResult b = GeneratorRegistry::instance().run(name, json::object(), 555);
    GenResult c = GeneratorRegistry::instance().run(name, json::object(), 556);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    REQUIRE(c.ok());
    // Determinism is the premise of the whole recipe model.
    CHECK(identical(*a.mesh, *b.mesh));
    CHECK_FALSE(identical(*a.mesh, *c.mesh));
  }
}

TEST_CASE("tree.v1 — emits bark and foliage submeshes, pivot at the base") {
  json params;
  params["height"] = 9.0;
  params["canopy"] = "conifer";
  GenResult r = GeneratorRegistry::instance().run("tree.v1", params, 11);
  REQUIRE(r.ok());
  REQUIRE(r.mesh->submeshes.size() >= 2);
  CHECK(r.mesh->submeshes[0].material.id == "bark");
  CHECK(r.mesh->submeshes[1].material.id == "foliage");

  glm::vec3 mn, mx;
  REQUIRE(r.mesh->getGlobalBounds(mn, mx));
  CHECK(mn.y == doctest::Approx(0.0f).epsilon(0.05f)); // sits on the ground
  CHECK(mx.y > 5.0f);
  CHECK(r.triangles > 0);
}

TEST_CASE("tree.v1 — 'bare' canopy produces no foliage") {
  json params;
  params["canopy"] = "bare";
  GenResult r = GeneratorRegistry::instance().run("tree.v1", params, 3);
  REQUIRE(r.ok());
  for (const auto &sm : r.mesh->submeshes)
    CHECK(sm.material.id != "foliage");
}

TEST_CASE("rock.v1 — sits on y=0 and respects the poly budget") {
  GenResult r = GeneratorRegistry::instance().run("rock.v1", json::object(), 21);
  REQUIRE(r.ok());
  glm::vec3 mn, mx;
  REQUIRE(r.mesh->getGlobalBounds(mn, mx));
  CHECK(mn.y == doctest::Approx(0.0f).epsilon(0.001f));
  CHECK(r.triangles <= 1500);
  CHECK(r.warnings.empty());
}

TEST_CASE("Registry — exceeding the poly budget warns but still returns a mesh") {
  json params;
  params["detail"] = 4; // 20 * 4^4 = 5120 triangles, over rock's 1500
  GenResult r = GeneratorRegistry::instance().run("rock.v1", params, 5);
  REQUIRE(r.ok()); // a warning, not a rejection
  CHECK(r.triangles > 1500);
  bool warned = false;
  for (const auto &w : r.warnings)
    if (w.find("budget") != std::string::npos)
      warned = true;
  CHECK(warned);
}

TEST_CASE("grass.v1 — stays within its very tight budget at defaults") {
  GenResult r = GeneratorRegistry::instance().run("grass.v1", json::object(), 9);
  REQUIRE(r.ok());
  // Grass is instanced tens of thousands of times; the default must be cheap.
  CHECK(r.triangles <= 200);
}

TEST_CASE("kitbash.v1 — builds from a part list and groups by colour") {
  json params;
  json parts = json::array();
  json box;
  box["shape"] = "box";
  box["pos"] = json::array({0.0, 0.5, 0.0});
  box["size"] = json::array({1.0, 1.0, 1.0});
  parts.push_back(box);
  json band;
  band["shape"] = "cylinder";
  band["pos"] = json::array({0.0, 0.0, 0.0});
  band["size"] = json::array({1.2, 0.1, 1.2});
  band["color"] = json::array({0.2, 0.2, 0.22});
  parts.push_back(band);
  params["parts"] = parts;

  GenResult r = GeneratorRegistry::instance().run("kitbash.v1", params, 1);
  REQUIRE(r.ok());
  // Two colours -> two submeshes, so a crate with metal bands is 2 draws.
  CHECK(r.mesh->submeshes.size() == 2);
  CHECK(r.triangles > 12);
}

TEST_CASE("kitbash.v1 — empty part list is an error, bad shapes are warnings") {
  GenResult empty =
      GeneratorRegistry::instance().run("kitbash.v1", json::object(), 1);
  CHECK_FALSE(empty.ok());

  json params;
  json parts = json::array();
  json good;
  good["shape"] = "box";
  good["size"] = json::array({1.0, 1.0, 1.0});
  parts.push_back(good);
  json bad;
  bad["shape"] = "dodecahedron";
  parts.push_back(bad);
  params["parts"] = parts;

  GenResult r = GeneratorRegistry::instance().run("kitbash.v1", params, 1);
  REQUIRE(r.ok()); // the good part still renders
  bool warned = false;
  for (const auto &w : r.warnings)
    if (w.find("dodecahedron") != std::string::npos)
      warned = true;
  CHECK(warned);
}

TEST_CASE("sculpt.v1 — stamps reshape a grounded, deterministic mesh") {
  json params;
  params["radius"] = 1.0;
  params["detail"] = 2;
  params["stamps"] = json::array({
      {{"type", "inflate"}, {"centre", {0.0, 1.0, 0.0}},
       {"radius", 0.55}, {"strength", 0.45}},
      {{"type", "carve"}, {"centre", {0.0, 0.0, 1.0}},
       {"radius", 0.35}, {"strength", 0.20}},
      {{"type", "flatten"}, {"centre", {0.0, -1.0, 0.0}},
       {"radius", 0.8}, {"strength", -0.15}},
  });
  GenResult a = GeneratorRegistry::instance().run("sculpt.v1", params, 42);
  GenResult b = GeneratorRegistry::instance().run("sculpt.v1", params, 42);
  REQUIRE(a.ok());
  REQUIRE(b.ok());
  CHECK(identical(*a.mesh, *b.mesh));
  glm::vec3 mn, mx;
  REQUIRE(a.mesh->getGlobalBounds(mn, mx));
  CHECK(mn.y == doctest::Approx(0.0f).epsilon(0.001f));
  CHECK(mx.y > 2.0f); // the upper inflate is visible in the bounds
  CHECK(a.triangles <= 3000);
}

TEST_CASE("sculpt.v1 — malformed stamps warn without losing valid geometry") {
  json params;
  params["stamps"] = json::array({
      {{"type", "inflate"}, {"centre", {0.0, 1.0}}, {"radius", 1.0}},
      {{"type", "melt"}, {"centre", {0.0, 1.0, 0.0}}},
  });
  GenResult r = GeneratorRegistry::instance().run("sculpt.v1", params, 1);
  REQUIRE(r.ok());
  CHECK(r.warnings.size() >= 2);
}

TEST_CASE("sweep.v1 — follows a path and rejects an incomplete path") {
  json params;
  params["points"] = json::array({{0.0, 0.0, 0.0}, {0.0, 1.0, 0.0},
                                   {0.6, 1.5, 0.0}});
  params["startRadius"] = 0.16;
  params["endRadius"] = 0.04;
  GenResult r = GeneratorRegistry::instance().run("sweep.v1", params, 1);
  REQUIRE(r.ok());
  CHECK(r.triangles > 0);
  GenResult invalid = GeneratorRegistry::instance().run(
      "sweep.v1", json{{"points", json::array({{0.0, 0.0, 0.0}})}}, 1);
  CHECK_FALSE(invalid.ok());
}

TEST_CASE("revolve.v1 and panel.v1 — make grounded authored forms") {
  json profile = json::array({{0.0, 0.0}, {0.45, 0.0}, {0.38, 0.7},
                              {0.18, 1.0}, {0.0, 1.05}});
  GenResult lathed = GeneratorRegistry::instance().run(
      "revolve.v1", json{{"profile", profile}, {"segments", 12}}, 1);
  REQUIRE(lathed.ok());
  glm::vec3 mn, mx;
  REQUIRE(lathed.mesh->getGlobalBounds(mn, mx));
  CHECK(mn.y == doctest::Approx(0.0f).epsilon(0.001f));

  GenResult panel = GeneratorRegistry::instance().run(
      "panel.v1", json{{"rows", 2}, {"columns", 3}, {"studs", true}}, 1);
  REQUIRE(panel.ok());
  CHECK(panel.triangles > 12);
  CHECK(panel.triangles <= 2500);
}

TEST_CASE("character.v1 — deterministic grounded humanoid has material regions") {
  json params;
  params["outfit"] = "armor";
  params["detail"] = 0;
  params["operations"] = json::array({{{"op", "reshape"}, {"region", "torso"}, {"amount", 0.12}},
                                        {{"op", "add_garment"}, {"type", "armor"}}});
  GenResult a = GeneratorRegistry::instance().run("character.v1", params, 19);
  GenResult b = GeneratorRegistry::instance().run("character.v1", params, 19);
  REQUIRE(a.ok());
  REQUIRE(b.ok());
  CHECK(identical(*a.mesh, *b.mesh));
  CHECK(a.mesh->submeshes.size() >= 2);
  glm::vec3 mn, mx;
  REQUIRE(a.mesh->getGlobalBounds(mn, mx));
  CHECK(mn.y == doctest::Approx(0.0f).epsilon(0.002f));
  CHECK(mx.y > 1.3f);
  CHECK(a.triangles < 20000);
}

TEST_CASE("character.v1 — explorer outfit preset emits 7 material regions under 15k triangles") {
  json params;
  params["outfit"] = "explorer";
  params["detail"] = 1;
  params["operations"] = json::array({
      {{"op", "add_garment"}, {"type", "explorer"}},
      {{"op", "add_accessory"}, {"type", "utility_belt"}},
      {{"op", "assign_material"}, {"region", "jacket"}, {"color", {0.08, 0.14, 0.28}}},
      {{"op", "assign_material"}, {"region", "shirt"}, {"color", {0.85, 0.85, 0.82}}},
      {{"op", "assign_material"}, {"region", "trousers"}, {"color", {0.12, 0.12, 0.16}}},
      {{"op", "assign_material"}, {"region", "boots"}, {"color", {0.32, 0.18, 0.09}}},
      {{"op", "assign_material"}, {"region", "belt"}, {"color", {0.25, 0.15, 0.08}}},
      {{"op", "assign_material"}, {"region", "skin"}, {"color", {0.72, 0.48, 0.34}}}
  });
  GenResult r = GeneratorRegistry::instance().run("character.v1", params, 42);
  REQUIRE(r.ok());
  CHECK(r.mesh->submeshes.size() == 7);
  glm::vec3 mn, mx;
  REQUIRE(r.mesh->getGlobalBounds(mn, mx));
  CHECK(mn.y == doctest::Approx(0.0f).epsilon(0.002f));
  CHECK(r.triangles < 15000);
}


TEST_CASE("import.gltf — missing path and missing file fail cleanly") {
  GenResult noPath =
      GeneratorRegistry::instance().run("import.gltf", json::object(), 0);
  CHECK_FALSE(noPath.ok());

  json params;
  params["path"] = "definitely/not/here.obj";
  GenResult missing =
      GeneratorRegistry::instance().run("import.gltf", params, 0);
  CHECK_FALSE(missing.ok());
  CHECK(missing.error.find("no such file") != std::string::npos);
}

// ── Texture generation ─────────────────────────────────────────────────────

TEST_CASE("TextureGen — produces embedded albedo and roughness payloads") {
  json params;
  params["resolution"] = 64;
  TextureSet set;
  std::vector<std::string> warnings;
  std::string error;
  REQUIRE(generateTextureSet("tex.bark", params, 3, "bark", set, warnings, error));

  CHECK(set.valid());
  CHECK_FALSE(set.albedoKey.empty());
  CHECK_FALSE(set.roughnessKey.empty());
  CHECK_FALSE(set.normalKey.empty());
  for (const auto &img : set.images) {
    CHECK(img.width == 64);
    CHECK(img.height == 64);
    CHECK(img.pixels.size() ==
          static_cast<size_t>(img.width) * img.height * img.component);
  }
}

TEST_CASE("TextureGen — leaf cutout actually cuts the card's corners") {
  json params;
  params["resolution"] = 64;
  params["cutout"] = true;
  TextureSet set;
  std::vector<std::string> warnings;
  std::string error;
  REQUIRE(generateTextureSet("tex.leaf", params, 1, "foliage", set, warnings, error));

  const MeshImage *albedo = nullptr;
  for (const auto &img : set.images)
    if (img.key == set.albedoKey)
      albedo = &img;
  REQUIRE(albedo != nullptr);
  REQUIRE(albedo->component == 4);

  auto alphaAt = [&](int x, int y) {
    return albedo->pixels[(static_cast<size_t>(y) * albedo->width + x) * 4 + 3];
  };
  // Opaque in the middle of the leaf, transparent at the card's corner.
  CHECK(alphaAt(32, 32) > 200);
  CHECK(alphaAt(0, 0) < 40);
}

TEST_CASE("TextureGen — unknown generator fails, schemas are published") {
  TextureSet set;
  std::vector<std::string> warnings;
  std::string error;
  CHECK_FALSE(generateTextureSet("tex.nope", json::object(), 1, "x", set,
                                 warnings, error));
  CHECK(error.find("unknown texture generator") != std::string::npos);

  const auto names = textureGeneratorNames();
  CHECK(std::find(names.begin(), names.end(), "tex.rock") != names.end());
  REQUIRE(textureGeneratorSchema("tex.rock") != nullptr);
  CHECK(textureGeneratorSchema("tex.nope") == nullptr);
}

TEST_CASE("TextureGen — applying a set points the material at the payloads") {
  MeshData mesh;
  MaterialAsset material;
  json params;
  params["resolution"] = 32;
  TextureSet set;
  std::vector<std::string> warnings;
  std::string error;
  REQUIRE(generateTextureSet("tex.rock", params, 2, "rock", set, warnings, error));
  applyTextureSet(mesh, material, std::move(set));

  CHECK_FALSE(material.texDiffusePath.empty());
  CHECK_FALSE(material.texRoughnessPath.empty());
  CHECK_FALSE(material.texNormalPath.empty());
  // The renderer resolves these by looking the key up in MeshData::images --
  // if the payload is missing it silently falls back to a file load.
  CHECK(mesh.findImage(material.texDiffusePath) != nullptr);
  CHECK(mesh.findImage(material.texRoughnessPath) != nullptr);
  CHECK(mesh.findImage(material.texNormalPath) != nullptr);
}

// ── AssetLibrary ───────────────────────────────────────────────────────────

TEST_CASE("AssetLibrary — resolve registers a renderable asset") {
  AssetManager assets;
  AssetLibrary lib;
  lib.initialize(assets, "");

  AssetRecipe recipe = makeRecipe("rock.v1");
  recipe.id = "rock/test";
  ResolveResult r = lib.resolve(recipe);

  REQUIRE(r.ok());
  CHECK(r.regenerated);
  CHECK(r.assetId == "gen://rock.v1/rock/test");
  CHECK(r.triangles > 0);

  const MeshData *data = assets.getOBJData(assets.findMeshData(r.assetId));
  REQUIRE(data != nullptr);
  CHECK(triCount(*data) == r.triangles);
  CHECK(data->sourcePath == r.assetId);
}

TEST_CASE("AssetLibrary — identical recipes are served from cache") {
  AssetManager assets;
  AssetLibrary lib;
  lib.initialize(assets, "");

  AssetRecipe recipe = makeRecipe("rock.v1");
  recipe.id = "rock/test";
  ResolveResult first = lib.resolve(recipe);
  ResolveResult second = lib.resolve(recipe);

  REQUIRE(first.ok());
  REQUIRE(second.ok());
  CHECK(first.regenerated);
  CHECK_FALSE(second.regenerated); // cheap to call repeatedly
  CHECK(first.assetId == second.assetId);
}

TEST_CASE("AssetLibrary — editing a named recipe swaps geometry in place") {
  AssetManager assets;
  AssetLibrary lib;
  lib.initialize(assets, "");

  AssetRecipe recipe = makeRecipe("rock.v1");
  recipe.id = "rock/test";
  recipe.params["radius"] = 0.5;
  ResolveResult first = lib.resolve(recipe);
  REQUIRE(first.ok());

  const OBJHandle handle = assets.findMeshData(first.assetId);
  const uint32_t v0 = assets.assetContentVersion(first.assetId);
  glm::vec3 mn0, mx0;
  REQUIRE(assets.getOBJData(handle)->getGlobalBounds(mn0, mx0));

  recipe.params["radius"] = 2.0;
  ResolveResult second = lib.resolve(recipe);
  REQUIRE(second.ok());
  CHECK(second.regenerated);

  // Same asset id and the ORIGINAL handle still resolves -- this is what lets
  // already-placed entities pick up the edit instead of losing their mesh.
  CHECK(second.assetId == first.assetId);
  const MeshData *updated = assets.getOBJData(handle);
  REQUIRE(updated != nullptr);
  glm::vec3 mn1, mx1;
  REQUIRE(updated->getGlobalBounds(mn1, mx1));
  CHECK(mx1.y > mx0.y);
  CHECK(assets.assetContentVersion(first.assetId) > v0);
}

TEST_CASE("AssetLibrary — material slots attach generated textures") {
  AssetManager assets;
  AssetLibrary lib;
  lib.initialize(assets, "");

  AssetRecipe recipe = makeRecipe("tree.v1");
  recipe.id = "tree/textured";
  recipe.params["canopy"] = "conifer";
  json bark;
  bark["generator"] = "tex.bark";
  bark["params"]["resolution"] = 32;
  recipe.material["bark"] = bark;

  ResolveResult r = lib.resolve(recipe);
  REQUIRE(r.ok());

  const MeshData *data = assets.getOBJData(assets.findMeshData(r.assetId));
  REQUIRE(data != nullptr);
  CHECK_FALSE(data->images.empty());
  bool barkTextured = false;
  for (const auto &sm : data->submeshes)
    if (sm.material.id == "bark" && !sm.material.texDiffusePath.empty() &&
        data->findImage(sm.material.texDiffusePath))
      barkTextured = true;
  CHECK(barkTextured);
}

TEST_CASE("AssetLibrary — a bad material slot warns without losing the mesh") {
  AssetManager assets;
  AssetLibrary lib;
  lib.initialize(assets, "");

  AssetRecipe recipe = makeRecipe("rock.v1");
  recipe.id = "rock/badtex";
  json slot;
  slot["generator"] = "tex.does_not_exist";
  recipe.material["rock"] = slot;

  ResolveResult r = lib.resolve(recipe);
  // Geometry must survive a texture failure -- it still renders flat-coloured.
  REQUIRE(r.ok());
  CHECK(r.triangles > 0);
  bool warned = false;
  for (const auto &w : r.warnings)
    if (w.find("flat colour") != std::string::npos)
      warned = true;
  CHECK(warned);
}

TEST_CASE("AssetLibrary — a material slot naming no submesh is reported") {
  AssetManager assets;
  AssetLibrary lib;
  lib.initialize(assets, "");

  AssetRecipe recipe = makeRecipe("rock.v1");
  recipe.id = "rock/wrongslot";
  json slot;
  slot["generator"] = "tex.bark";
  recipe.material["trunk"] = slot; // rock.v1 has no "trunk" submesh

  ResolveResult r = lib.resolve(recipe);
  REQUIRE(r.ok());
  bool warned = false;
  for (const auto &w : r.warnings)
    if (w.find("does not match any submesh") != std::string::npos)
      warned = true;
  CHECK(warned);
}

TEST_CASE("AssetLibrary — editing a recipe file hot-reloads it in place") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "glgen_recipe_hotreload";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  const fs::path file = dir / "rock.json";

  auto writeRecipe = [&](double radius) {
    AssetRecipe r = makeRecipe("rock.v1");
    r.id = "rock/watched";
    r.params["radius"] = radius;
    REQUIRE(writeRecipeFile(file.string(), r));
  };

  writeRecipe(0.5);

  AssetManager assets;
  AssetLibrary lib;
  lib.initialize(assets, dir.string());
  std::vector<std::string> errors;
  REQUIRE(lib.loadAll(errors) == 1);
  CHECK(errors.empty());

  const std::string assetId = lib.assetIdOfRecipe("rock/watched");
  REQUIRE_FALSE(assetId.empty());
  const OBJHandle handle = assets.findMeshData(assetId);
  const uint32_t v0 = assets.assetContentVersion(assetId);
  glm::vec3 mn0, mx0;
  REQUIRE(assets.getOBJData(handle)->getGlobalBounds(mn0, mx0));

  // An unchanged file must not regenerate -- this is polled on an interval in
  // the frame loop, so a false positive means rebuilding every asset forever.
  CHECK(lib.pollHotReload().empty());

  writeRecipe(2.0);
  // Force a distinct timestamp rather than sleeping: filesystem granularity
  // can be coarse enough that two writes in the same test share a time.
  fs::last_write_time(file, fs::last_write_time(file, ec) + std::chrono::seconds(2), ec);

  const std::vector<std::string> messages = lib.pollHotReload();
  REQUIRE_FALSE(messages.empty());
  CHECK(messages[0].find("Reloaded recipe") != std::string::npos);

  // The asset id is unchanged and the ORIGINAL handle now resolves to the new
  // geometry: that is the whole authoring loop -- edit JSON, see it change,
  // without entities losing their mesh.
  CHECK(lib.assetIdOfRecipe("rock/watched") == assetId);
  const MeshData *updated = assets.getOBJData(handle);
  REQUIRE(updated != nullptr);
  glm::vec3 mn1, mx1;
  REQUIRE(updated->getGlobalBounds(mn1, mx1));
  CHECK(mx1.y > mx0.y * 1.5f);
  CHECK(assets.assetContentVersion(assetId) > v0);

  fs::remove_all(dir, ec);
}

TEST_CASE("AssetLibrary — a broken recipe file reports without killing the rest") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "glgen_recipe_broken";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  {
    AssetRecipe good = makeRecipe("rock.v1");
    good.id = "rock/good";
    REQUIRE(writeRecipeFile((dir / "good.json").string(), good));
  }
  {
    std::ofstream bad((dir / "bad.json").string());
    bad << "{ this is not json";
  }

  AssetManager assets;
  AssetLibrary lib;
  lib.initialize(assets, dir.string());
  std::vector<std::string> errors;

  // One bad recipe must not take the project down.
  CHECK(lib.loadAll(errors) == 1);
  REQUIRE(errors.size() == 1);
  CHECK(errors[0].find("bad.json") != std::string::npos);
  CHECK_FALSE(lib.assetIdOfRecipe("rock/good").empty());

  fs::remove_all(dir, ec);
}

TEST_CASE("AssetLibrary — generator errors surface without registering") {
  AssetManager assets;
  AssetLibrary lib;
  lib.initialize(assets, "");

  AssetRecipe recipe = makeRecipe("kitbash.v1"); // no parts
  recipe.id = "prop/broken";
  ResolveResult r = lib.resolve(recipe);

  CHECK_FALSE(r.ok());
  CHECK_FALSE(r.error.empty());
  CHECK(assets.stats().objLive == 0);
  CHECK(lib.assetIdOfRecipe("prop/broken").empty());
}
