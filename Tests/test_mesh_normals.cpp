#include "MeshNormals.h"
#include <doctest/doctest.h>
#include <limits>

namespace {
MeshData triangle(bool indexed) {
  MeshData mesh;mesh.submeshes.emplace_back();
  auto &s=mesh.submeshes.back();s.vertices.resize(3);
  s.vertices[0].pos={0,0,0};s.vertices[1].pos={1,0,0};s.vertices[2].pos={0,1,0};
  for(auto &v:s.vertices)v.normal={0,0,0};
  if(indexed)s.indices={0,1,2};
  return mesh;
}
}
TEST_CASE("Missing import normals are reconstructed for indexed and triangle-list meshes") {
  for(bool indexed:{false,true}) {
    auto mesh=triangle(indexed);
    CHECK(repairMissingNormals(mesh)==3);
    for(auto &v:mesh.submeshes[0].vertices)CHECK(v.normal==glm::vec3(0,0,1));
    CHECK(repairMissingNormals(mesh)==0);
    CHECK(mesh.submeshes[0].vertices[1].pos==glm::vec3(1,0,0));
  }
}
TEST_CASE("Normal repair preserves authored values and repairs non-finite input") {
  auto mesh=triangle(true);auto &v=mesh.submeshes[0].vertices;
  v[0].normal={.2f,.3f,.4f};
  v[1].normal={std::numeric_limits<float>::quiet_NaN(),0,0};
  CHECK(repairMissingNormals(mesh)==2);
  CHECK(v[0].normal==glm::vec3(.2f,.3f,.4f));
  CHECK(v[1].normal==glm::vec3(0,0,1));
}
TEST_CASE("Degenerate faces and invalid indices cannot introduce NaN normals") {
  auto mesh=triangle(true);auto &s=mesh.submeshes[0];
  s.vertices[1].pos=s.vertices[0].pos;s.indices.insert(s.indices.end(),{99,0,1});
  CHECK(repairMissingNormals(mesh)==3);
  for(auto &v:s.vertices)CHECK(v.normal==glm::vec3(0,1,0));
}
