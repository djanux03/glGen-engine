#include <doctest/doctest.h>

#include "MeshBuilder.h"
#include "MeshDecimate.h"

#include <algorithm>
#include <cmath>
#include <set>

// Decimation conditions imported (text-to-3D, store-bought) meshes down to
// something this renderer can afford -- one acceleration structure per unique
// mesh. The invariants below are the ones that matter for that job: the mesh
// stays valid, stays roughly the same shape, and never comes back empty.

using namespace gen;

namespace {

size_t triCount(const MeshSubmeshData &sm) {
  return (sm.indices.empty() ? sm.vertices.size() : sm.indices.size()) / 3;
}

size_t triCount(const MeshData &m) {
  size_t n = 0;
  for (const auto &sm : m.submeshes)
    n += triCount(sm);
  return n;
}

// Every index must address a real vertex and no triangle may be degenerate --
// the renderer reads these straight into a vertex buffer and a BLAS.
void checkWellFormed(const MeshSubmeshData &sm) {
  REQUIRE(sm.indices.size() % 3 == 0);
  for (uint32_t i : sm.indices)
    REQUIRE(i < sm.vertices.size());
  for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
    const uint32_t a = sm.indices[i], b = sm.indices[i + 1], c = sm.indices[i + 2];
    CHECK(a != b);
    CHECK(b != c);
    CHECK(a != c);
  }
}

glm::vec3 boundsSize(const MeshSubmeshData &sm) {
  glm::vec3 mn(1e30f), mx(-1e30f);
  for (const auto &v : sm.vertices) {
    mn = glm::min(mn, v.pos);
    mx = glm::max(mx, v.pos);
  }
  return mx - mn;
}

// A dense sphere: the standard decimation subject, and close to what a
// text-to-3D service actually returns (smooth, over-tessellated, no hard edges).
std::unique_ptr<MeshData> makeDenseSphere(int subdivisions = 4) {
  MeshBuilder b;
  b.addIcosphere(glm::vec3(0.0f), 1.0f, subdivisions);
  b.recomputeSmoothNormals();
  return b.build();
}

} // namespace

TEST_CASE("decimate — reduces a dense mesh toward the target") {
  auto mesh = makeDenseSphere(4); // 20 * 4^4 = 5120 triangles
  const size_t before = triCount(*mesh);
  REQUIRE(before > 4000);

  const DecimateResult result = decimateMesh(*mesh, 800);

  CHECK(result.trianglesBefore == before);
  CHECK(result.changed());
  CHECK(triCount(*mesh) < before);
  // Collapses that would flip a face are refused, so the target is a goal and
  // not a guarantee. It must get close, not exact.
  CHECK(triCount(*mesh) <= 1200);
  checkWellFormed(mesh->submeshes[0]);
}

TEST_CASE("decimate — preserves overall shape and scale") {
  auto mesh = makeDenseSphere(4);
  const glm::vec3 sizeBefore = boundsSize(mesh->submeshes[0]);

  decimateMesh(*mesh, 400);
  const glm::vec3 sizeAfter = boundsSize(mesh->submeshes[0]);

  // Silhouette is what survives at the distance scattered props are seen from.
  // A vertex-clustering decimator would fail this by shrinking the hull.
  for (int axis = 0; axis < 3; ++axis)
    CHECK(sizeAfter[axis] == doctest::Approx(sizeBefore[axis]).epsilon(0.15));
}

TEST_CASE("decimate — bounds are refreshed, not left stale") {
  auto mesh = makeDenseSphere(3);
  decimateMesh(*mesh, 100);

  const auto &sm = mesh->submeshes[0];
  REQUIRE(sm.hasBounds);
  glm::vec3 mn(1e30f), mx(-1e30f);
  for (const auto &v : sm.vertices) {
    mn = glm::min(mn, v.pos);
    mx = glm::max(mx, v.pos);
  }
  CHECK(sm.aabbMin.x == doctest::Approx(mn.x));
  CHECK(sm.aabbMax.y == doctest::Approx(mx.y));
  // Object bounds are separate copies and must not keep pre-decimation values.
  REQUIRE_FALSE(mesh->objectBounds.empty());
  CHECK(mesh->objectBounds[0].second.aabbMax.y == doctest::Approx(mx.y));
}

TEST_CASE("decimate — a target at or above the current count is a no-op") {
  auto mesh = makeDenseSphere(2);
  const size_t before = triCount(*mesh);

  DecimateResult result = decimateMesh(*mesh, before);
  CHECK_FALSE(result.changed());
  CHECK(triCount(*mesh) == before);

  result = decimateMesh(*mesh, before * 10);
  CHECK_FALSE(result.changed());
  CHECK(triCount(*mesh) == before);

  // 0 means "no budget", not "reduce to nothing".
  result = decimateMesh(*mesh, 0);
  CHECK_FALSE(result.changed());
  CHECK(triCount(*mesh) == before);
}

TEST_CASE("decimate — never returns an empty mesh") {
  auto mesh = makeDenseSphere(3);
  // An absurd target must still leave something renderable: handing the
  // renderer zero geometry is worse than ignoring the request.
  decimateMesh(*mesh, 1);
  CHECK(triCount(*mesh) > 0);
  checkWellFormed(mesh->submeshes[0]);
}

TEST_CASE("decimate — a mesh at minimum complexity survives") {
  MeshBuilder b;
  b.addBox(glm::vec3(-1.0f), glm::vec3(1.0f)); // 12 triangles
  auto mesh = b.build();

  decimateMesh(*mesh, 2);
  // A closed box cannot reach 2 triangles without flipping faces; the guard
  // must stop it rather than producing a folded mess.
  CHECK(triCount(*mesh) > 0);
  checkWellFormed(mesh->submeshes[0]);
}

TEST_CASE("decimate — submeshes are budgeted proportionally") {
  MaterialAsset big, small;
  big.id = "big";
  small.id = "small";

  MeshBuilder b;
  b.beginSubmesh("Big", big);
  b.addIcosphere(glm::vec3(0.0f), 1.0f, 3); // 1280 triangles
  b.beginSubmesh("Small", small);
  b.addIcosphere(glm::vec3(5.0f, 0.0f, 0.0f), 0.3f, 1); // 80 triangles
  auto mesh = b.build();

  REQUIRE(mesh->submeshes.size() == 2);
  const size_t smallBefore = triCount(mesh->submeshes[1]);

  decimateMesh(*mesh, 400);

  // The small submesh must not be erased to save the large one -- that is how
  // handles, hinges and trim vanish from a decimated prop.
  CHECK(triCount(mesh->submeshes[1]) > 0);
  CHECK(triCount(mesh->submeshes[1]) <= smallBefore);
  CHECK(triCount(mesh->submeshes[0]) > triCount(mesh->submeshes[1]));
  // Material identity survives, or the mesh comes back with the wrong looks.
  CHECK(mesh->submeshes[0].material.id == "big");
  CHECK(mesh->submeshes[1].material.id == "small");
}

TEST_CASE("decimate — handles a non-indexed submesh") {
  // The file parsers can emit non-indexed triangle soup; decimation has to
  // cope rather than reading indices that are not there.
  MeshData mesh;
  MeshSubmeshData sm;
  sm.objectName = "soup";
  for (int i = 0; i < 60; ++i) {
    const float f = static_cast<float>(i);
    MeshVertex a, b, c;
    a.pos = glm::vec3(f, 0.0f, 0.0f);
    b.pos = glm::vec3(f + 1.0f, 0.0f, 0.0f);
    c.pos = glm::vec3(f, 1.0f, 0.0f);
    a.normal = b.normal = c.normal = glm::vec3(0, 0, 1);
    sm.vertices.push_back(a);
    sm.vertices.push_back(b);
    sm.vertices.push_back(c);
  }
  sm.hasBounds = false;
  mesh.submeshes.push_back(std::move(sm));

  const size_t before = triCount(mesh);
  REQUIRE(before == 60);
  decimateMesh(mesh, 20);
  CHECK(triCount(mesh) <= before);
  CHECK(triCount(mesh) > 0);
  checkWellFormed(mesh.submeshes[0]);
}

TEST_CASE("decimate — welds seams so collapses can actually happen") {
  // Two triangles sharing an edge, but with the shared vertices DUPLICATED as
  // a UV seam would leave them. Without welding, the topology has no shared
  // edge and nothing can collapse.
  MeshData mesh;
  MeshSubmeshData sm;
  auto push = [&](glm::vec3 p, glm::vec2 uv) {
    MeshVertex v;
    v.pos = p;
    v.uv = uv;
    v.normal = glm::vec3(0, 1, 0);
    sm.vertices.push_back(v);
    sm.indices.push_back(static_cast<uint32_t>(sm.vertices.size() - 1));
  };
  // Fan of triangles around a centre, each with its own copy of the centre.
  for (int i = 0; i < 12; ++i) {
    const float a0 = (i / 12.0f) * 6.2831853f;
    const float a1 = ((i + 1) / 12.0f) * 6.2831853f;
    push(glm::vec3(0.0f), glm::vec2(0.5f, 0.5f));
    push(glm::vec3(std::cos(a0), 0.0f, std::sin(a0)), glm::vec2(0.0f, 0.0f));
    push(glm::vec3(std::cos(a1), 0.0f, std::sin(a1)), glm::vec2(1.0f, 0.0f));
  }
  mesh.submeshes.push_back(std::move(sm));

  const size_t before = triCount(mesh);
  REQUIRE(before == 12);
  REQUIRE(mesh.submeshes[0].vertices.size() == 36); // fully split

  decimateMesh(mesh, 6);
  CHECK(triCount(mesh) < before);
  checkWellFormed(mesh.submeshes[0]);
}

TEST_CASE("decimate — is deterministic") {
  auto a = makeDenseSphere(3);
  auto b = makeDenseSphere(3);
  decimateMesh(*a, 300);
  decimateMesh(*b, 300);

  // Recipes are reproducible only if every step is; an import that decimates
  // differently run to run breaks that promise.
  REQUIRE(a->submeshes.size() == b->submeshes.size());
  REQUIRE(a->submeshes[0].vertices.size() == b->submeshes[0].vertices.size());
  REQUIRE(a->submeshes[0].indices == b->submeshes[0].indices);
  for (size_t i = 0; i < a->submeshes[0].vertices.size(); ++i)
    CHECK(a->submeshes[0].vertices[i].pos == b->submeshes[0].vertices[i].pos);
}
