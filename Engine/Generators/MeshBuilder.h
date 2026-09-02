#pragma once
// MeshBuilder.h — the primitive kit generators are written against.
//
// This exists so a generator is ~150 lines of intent (a trunk tapers, a
// branch recurses, a blade bends) instead of ~800 lines of vertex bookkeeping.
// Everything it emits is INDEXED (unlike Engine/Assets/MeshPrimitives.cpp's
// non-indexed triangle soup) and lands in a MeshData that goes straight to
// AssetManager::registerMeshData().
//
// Two pieces of state make the generators short: a *current submesh* (each
// carries its own MaterialAsset, so a tree can emit bark and foliage as
// separate draws) and a *transform stack* (so a recursive branch just pushes
// its own frame and calls the same code again).

#include "MeshData.h"

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace gen {

class MeshBuilder {
public:
  explicit MeshBuilder(std::string sourcePath = {});

  // --- submeshes -----------------------------------------------------------
  // Starts a new submesh. Emitting anything without calling this first
  // implicitly opens a "default" submesh with default material values.
  void beginSubmesh(const std::string &name, const MaterialAsset &material);
  MeshSubmeshData &current();
  bool empty() const { return mSubmeshes.empty(); }

  // --- transform stack -----------------------------------------------------
  // `m` is multiplied ONTO the current top, so pushes compose the way nested
  // coordinate frames do. Normals use the inverse-transpose, so non-uniform
  // scale is safe.
  void pushTransform(const glm::mat4 &m);
  void popTransform();
  const glm::mat4 &transform() const { return mStack.back(); }

  // --- primitives ----------------------------------------------------------
  // All of these emit into the current submesh, through the transform stack.

  // Axis-aligned box spanning [min,max] (pre-transform). Per-face UVs.
  void addBox(const glm::vec3 &min, const glm::vec3 &max);

  // Cone/cylinder/trunk segment: a tube from p0 (radius r0) to p1 (radius r1).
  // The workhorse for trunks, branches, posts and rails. `vTile` scales the
  // along-axis UV so bark doesn't stretch on a long segment.
  void addTaperedCylinder(const glm::vec3 &p0, float r0, const glm::vec3 &p1,
                          float r1, int segments, bool capStart = true,
                          bool capEnd = true, float vTile = 1.0f);

  // Subdivided icosahedron: near-uniform triangle density with no poles, which
  // is what makes it the right base for noise-displaced rocks (a lat/long
  // sphere bunches vertices at the poles and displaces them unevenly).
  // Vertices are NOT shared between faces and UVs are planar-projected per
  // triangle along its dominant axis -- a crude unwrap that avoids the
  // seam/pole stretching a spherical mapping would give a displaced rock.
  // Call recomputeSmoothNormals() afterwards for smooth shading.
  void addIcosphere(const glm::vec3 &center, float radius, int subdivisions);

  // Single quad, corners in order (a-b-c-d). Normal from the winding.
  void addQuad(const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c,
               const glm::vec3 &d);

  // Upright card, base centered at the origin, spanning y=0..height, facing
  // +Z. `bendDeg` is the TOTAL forward bend from base to tip, distributed over
  // `segments` rows -- the difference between a grass blade and a paper strip.
  // Double-sided is unnecessary: the engine renders with cullMode NONE.
  //
  // `taper` is how much the card narrows toward its tip: 1 = a blade that
  // comes to a point (grass), 0 = a parallel-sided rectangle. Foliage cards
  // want a LOW taper, because their silhouette comes from the leaf texture's
  // own alpha cutout -- taper the geometry as well and the two narrowings
  // compound into a spike.
  void addCard(float width, float height, float bendDeg, int segments = 3,
               float taper = 1.0f);

  // A leaf/leaf-cluster card whose SILHOUETTE is in the geometry: a pointed
  // oval, 6 vertices and 4 triangles, base at the origin, bending forward
  // like addCard.
  //
  // The raster path now honors alpha cutout, but ray-query shadows still see
  // triangle geometry rather than material alpha. Keeping a cheap real outline
  // avoids rectangular foliage shadows and remains more stable in the distance
  // than a single nearly transparent quad.
  void addLeafCard(float width, float height, float bendDeg);

  // Surface of revolution about +Y. `profile` is a polyline of (radius,
  // height) pairs, swept `segments` ways. Covers barrels, pots, mushroom
  // caps, tapered posts.
  void addRevolve(const std::vector<glm::vec2> &profile, int segments);

  // --- post-processing -----------------------------------------------------
  // Moves every vertex of the current submesh. `fn` receives the vertex
  // position and normal and returns the NEW position. Used by rock.v1 to push
  // an icosphere around with noise.
  void displace(
      const std::function<glm::vec3(const glm::vec3 &pos, const glm::vec3 &normal)> &fn);

  // Averages normals across vertices sharing a position (within `weldEps`).
  // Does not merge the vertices themselves -- UV seams stay intact.
  void recomputeSmoothNormals(float weldEps = 1e-4f);
  // Per-triangle geometric normals (faceted). Splits shared vertices.
  void recomputeFlatNormals();

  // --- results -------------------------------------------------------------
  size_t triangleCount() const;
  size_t vertexCount() const;
  // Computes per-submesh and per-object bounds and hands over the MeshData.
  // The builder is empty afterwards.
  std::unique_ptr<MeshData> build();

private:
  uint32_t emit_(const glm::vec3 &pos, const glm::vec3 &normal,
                 const glm::vec2 &uv);
  void tri_(uint32_t a, uint32_t b, uint32_t c);
  void ensureSubmesh_();

  std::string mSourcePath;
  std::vector<MeshSubmeshData> mSubmeshes;
  std::vector<glm::mat4> mStack{glm::mat4(1.0f)};
  std::vector<glm::mat3> mNormalStack{glm::mat3(1.0f)};
};

} // namespace gen
