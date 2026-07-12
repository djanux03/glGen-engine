#pragma once
#include "MeshPrimitives.h"
#include "OBJModel.h"
#include <vector>

// Generates procedural OBJModel meshes for common primitives (GL upload of
// the API-agnostic PrimitiveGeometry vertex data). Each create*() allocates a
// new OBJModel on the heap — caller owns the pointer.
class PrimitiveMeshGenerator {
public:
  static OBJModel *createCube() {
    return fromVertices(PrimitiveGeometry::cube(), "Cube");
  }

  static OBJModel *createSphere(int stacks = 24, int slices = 32) {
    return fromVertices(PrimitiveGeometry::sphere(stacks, slices), "Sphere");
  }

  static OBJModel *createPlane() {
    return fromVertices(PrimitiveGeometry::plane(), "Plane");
  }

  static OBJModel *createCylinder(int segments = 32) {
    return fromVertices(PrimitiveGeometry::cylinder(segments), "Cylinder");
  }

  static OBJModel *createCone(int segments = 32) {
    return fromVertices(PrimitiveGeometry::cone(segments), "Cone");
  }

private:
  static OBJModel *fromVertices(const std::vector<MeshVertex> &verts,
                                const char *name) {
    std::vector<OBJModel::VertexData> converted(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
      converted[i].pos = verts[i].pos;
      converted[i].uv = verts[i].uv;
      converted[i].normal = verts[i].normal;
    }
    auto *model = new OBJModel();
    model->loadFromVertices(converted, name);
    return model;
  }
};
