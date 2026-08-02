#include <doctest/doctest.h>

#include "GeneratorRegistry.h"
#include "MeshBuilder.h"
#include "MeshValidate.h"

#include <cmath>
#include <limits>
#include <string>

// Validation is the TEXT half of the feedback loop: a render shows proportion
// and silhouette, these findings carry what a picture cannot. Each case below
// pins a specific message, because the message is the product -- a model acts
// on the sentence, not on a boolean.

using namespace gen;

namespace {

bool mentions(const std::vector<std::string> &issues, const std::string &needle) {
  for (const auto &issue : issues)
    if (issue.find(needle) != std::string::npos)
      return true;
  return false;
}

// A clean unit box sitting on y=0 -- what every check should pass.
std::unique_ptr<MeshData> makeGoodMesh() {
  MeshBuilder b;
  b.addBox(glm::vec3(-0.5f, 0.0f, -0.5f), glm::vec3(0.5f, 1.0f, 0.5f));
  return b.build();
}

} // namespace

TEST_CASE("validate — a well-formed mesh reports nothing") {
  auto mesh = makeGoodMesh();
  const auto issues = validateMesh(*mesh, MeshChecks{});
  CHECK(issues.empty());
  CHECK_FALSE(meshHasNonFiniteData(*mesh));
}

TEST_CASE("validate — non-finite vertex data is caught") {
  auto mesh = makeGoodMesh();
  mesh->submeshes[0].vertices[3].pos.y =
      std::numeric_limits<float>::quiet_NaN();

  CHECK(meshHasNonFiniteData(*mesh));
  const auto issues = validateMesh(*mesh, MeshChecks{});
  // The one condition that cannot be intentional: it corrupts the
  // acceleration-structure build and crashes far from the cause.
  CHECK(mentions(issues, "NaN or infinite"));

  auto infMesh = makeGoodMesh();
  infMesh->submeshes[0].vertices[1].normal.x =
      std::numeric_limits<float>::infinity();
  CHECK(meshHasNonFiniteData(*infMesh));
}

TEST_CASE("validate — a pivot off the base is reported with the offset") {
  MeshBuilder b;
  // Floating one metre above its own origin: every placement on terrain
  // would hover by exactly that much.
  b.addBox(glm::vec3(-0.5f, 1.0f, -0.5f), glm::vec3(0.5f, 2.0f, 0.5f));
  auto mesh = b.build();

  const auto issues = validateMesh(*mesh, MeshChecks{});
  CHECK(mentions(issues, "pivot is not at the base"));
  CHECK(mentions(issues, "float or sink"));

  // Opting out must silence it -- an import's pivot is the caller's choice.
  MeshChecks noPivotCheck;
  noPivotCheck.expectPivotAtBase = false;
  CHECK_FALSE(mentions(validateMesh(*mesh, noPivotCheck), "pivot"));
}

TEST_CASE("validate — geometry far from its own origin is reported") {
  MeshBuilder b;
  b.addBox(glm::vec3(49.5f, 0.0f, 49.5f), glm::vec3(50.5f, 1.0f, 50.5f));
  auto mesh = b.build();

  const auto issues = validateMesh(*mesh, MeshChecks{});
  // Rotates about a point outside itself, and its culling bounds are huge.
  CHECK(mentions(issues, "away from its own origin"));
}

TEST_CASE("validate — absurd and vanishing extents are both reported") {
  {
    MeshBuilder b;
    b.addBox(glm::vec3(-500.0f, 0.0f, -500.0f), glm::vec3(500.0f, 900.0f, 500.0f));
    const auto issues = validateMesh(*b.build(), MeshChecks{});
    CHECK(mentions(issues, "unit or scale mistake"));
  }
  {
    MeshBuilder b;
    b.addBox(glm::vec3(-0.0001f, 0.0f, -0.0001f), glm::vec3(0.0001f, 0.0002f, 0.0001f));
    const auto issues = validateMesh(*b.build(), MeshChecks{});
    CHECK(mentions(issues, "invisible at any normal scale"));
  }
}

TEST_CASE("validate — degenerate triangles are reported above a threshold") {
  MeshData mesh;
  MeshSubmeshData sm;
  sm.objectName = "degenerate";
  // Every triangle collinear: zero area, undefined normals.
  for (int i = 0; i < 10; ++i) {
    MeshVertex a, b, c;
    a.pos = glm::vec3(0.0f, 0.0f, 0.0f);
    b.pos = glm::vec3(1.0f, 0.0f, 0.0f);
    c.pos = glm::vec3(2.0f, 0.0f, 0.0f); // collinear with a and b
    a.normal = b.normal = c.normal = glm::vec3(0, 1, 0);
    sm.vertices.push_back(a);
    sm.vertices.push_back(b);
    sm.vertices.push_back(c);
  }
  sm.aabbMin = glm::vec3(0.0f);
  sm.aabbMax = glm::vec3(2.0f, 0.0f, 0.0f);
  sm.hasBounds = true;
  mesh.submeshes.push_back(std::move(sm));

  const auto issues = validateMesh(mesh, MeshChecks{});
  CHECK(mentions(issues, "zero area"));
}

TEST_CASE("validate — zero-length normals are reported") {
  auto mesh = makeGoodMesh();
  mesh->submeshes[0].vertices[0].normal = glm::vec3(0.0f);
  CHECK(mentions(validateMesh(*mesh, MeshChecks{}), "shade black"));
}

TEST_CASE("validate — out-of-range indices are reported") {
  auto mesh = makeGoodMesh();
  mesh->submeshes[0].indices[0] = 99999;
  CHECK(mentions(validateMesh(*mesh, MeshChecks{}), "do not exist"));
}

TEST_CASE("validate — a textured submesh with no UV variation is reported") {
  MeshBuilder b;
  MaterialAsset mat;
  mat.id = "flat";
  mat.texDiffusePath = "flat/albedo"; // claims a texture
  b.beginSubmesh("Flat", mat);
  b.addBox(glm::vec3(-0.5f, 0.0f, -0.5f), glm::vec3(0.5f, 1.0f, 0.5f));
  auto mesh = b.build();
  for (auto &v : mesh->submeshes[0].vertices)
    v.uv = glm::vec2(0.25f); // every vertex on the same texel

  // Renders flat, which reads as "the texture generator is broken".
  CHECK(mentions(validateMesh(*mesh, MeshChecks{}), "no UV variation"));

  // An UNtextured submesh with the same UVs is fine -- nothing samples them.
  auto plain = makeGoodMesh();
  for (auto &v : plain->submeshes[0].vertices)
    v.uv = glm::vec2(0.25f);
  CHECK_FALSE(mentions(validateMesh(*plain, MeshChecks{}), "UV"));
}

TEST_CASE("validate — the poly budget is reported when exceeded") {
  auto mesh = makeGoodMesh(); // 12 triangles
  MeshChecks checks;
  checks.polyBudget = 4;
  CHECK(mentions(validateMesh(*mesh, checks), "over the 4 budget"));

  checks.polyBudget = 100;
  CHECK_FALSE(mentions(validateMesh(*mesh, checks), "budget"));
  checks.polyBudget = 0; // 0 means no budget, not "reject everything"
  CHECK_FALSE(mentions(validateMesh(*mesh, checks), "budget"));
}

TEST_CASE("validate — empty meshes are reported, not silently accepted") {
  MeshData empty;
  CHECK(mentions(validateMesh(empty, MeshChecks{}), "no submeshes"));

  MeshData noTris;
  noTris.submeshes.emplace_back();
  CHECK(mentions(validateMesh(noTris, MeshChecks{}), "no triangles"));
}

// ── integration: every generator inherits validation ──────────────────────

TEST_CASE("registry — built-in generators produce clean meshes at defaults") {
  // Defaults are what an AI reaches for first; if they warn, the pipeline
  // cries wolf and the real findings stop being read.
  for (const char *name : {"tree.v1", "rock.v1", "grass.v1"}) {
    CAPTURE(name);
    GenResult r =
        GeneratorRegistry::instance().run(name, nlohmann::json::object(), 12345);
    REQUIRE(r.ok());
    for (const auto &w : r.warnings)
      CAPTURE(w);
    CHECK(r.warnings.empty());
  }
}

TEST_CASE("registry — validation findings reach the caller as warnings") {
  nlohmann::json params;
  params["detail"] = 4; // 5120 triangles, over rock.v1's 1500 budget
  GenResult r = GeneratorRegistry::instance().run("rock.v1", params, 5);

  REQUIRE(r.ok()); // a warning, never a rejection
  CHECK(mentions(r.warnings, "budget"));
}

TEST_CASE("registry — import.gltf is exempt from the pivot check") {
  // The caller picks the file and the recenter mode; second-guessing that
  // would emit a warning on every legitimate 'recenter: none' import.
  const GeneratorInfo *info =
      GeneratorRegistry::instance().find("import.gltf");
  REQUIRE(info != nullptr);
  CHECK(info->polyBudget == 0);
}
