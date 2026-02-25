#pragma once
#include "OBJModel.h"
#include <cmath>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Generates procedural OBJModel meshes for common primitives.
// Each create*() allocates a new OBJModel on the heap — caller owns the
// pointer.
class PrimitiveMeshGenerator {
public:
  using V = OBJModel::VertexData;

  // ── Cube (unit cube centered at origin, 1×1×1) ─────────────────────────
  static OBJModel *createCube() {
    auto *model = new OBJModel();
    std::vector<V> verts;
    verts.reserve(36);

    auto face = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
                    glm::vec3 n) {
      // Two triangles: a-b-c, a-c-d
      verts.push_back({a, {0, 0}, n});
      verts.push_back({b, {1, 0}, n});
      verts.push_back({c, {1, 1}, n});
      verts.push_back({a, {0, 0}, n});
      verts.push_back({c, {1, 1}, n});
      verts.push_back({d, {0, 1}, n});
    };

    const float h = 0.5f;
    // Front (+Z)
    face({-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}, {0, 0, 1});
    // Back (-Z)
    face({h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {0, 0, -1});
    // Right (+X)
    face({h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}, {1, 0, 0});
    // Left (-X)
    face({-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-1, 0, 0});
    // Top (+Y)
    face({-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}, {0, 1, 0});
    // Bottom (-Y)
    face({-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}, {0, -1, 0});

    model->loadFromVertices(verts, "Cube");
    return model;
  }

  // ── Sphere (UV sphere, radius 0.5, centered at origin) ─────────────────
  static OBJModel *createSphere(int stacks = 24, int slices = 32) {
    auto *model = new OBJModel();
    std::vector<V> verts;
    verts.reserve(stacks * slices * 6);

    const float r = 0.5f;

    for (int i = 0; i < stacks; ++i) {
      float phi0 = (float)M_PI * i / stacks;
      float phi1 = (float)M_PI * (i + 1) / stacks;

      for (int j = 0; j < slices; ++j) {
        float theta0 = 2.0f * (float)M_PI * j / slices;
        float theta1 = 2.0f * (float)M_PI * (j + 1) / slices;

        auto toPos = [&](float phi, float theta) -> glm::vec3 {
          return {r * sinf(phi) * cosf(theta), r * cosf(phi),
                  r * sinf(phi) * sinf(theta)};
        };
        auto toUV = [&](float phi, float theta) -> glm::vec2 {
          return {theta / (2.0f * (float)M_PI), phi / (float)M_PI};
        };

        glm::vec3 p00 = toPos(phi0, theta0);
        glm::vec3 p10 = toPos(phi1, theta0);
        glm::vec3 p11 = toPos(phi1, theta1);
        glm::vec3 p01 = toPos(phi0, theta1);

        glm::vec3 n00 = glm::normalize(p00);
        glm::vec3 n10 = glm::normalize(p10);
        glm::vec3 n11 = glm::normalize(p11);
        glm::vec3 n01 = glm::normalize(p01);

        glm::vec2 uv00 = toUV(phi0, theta0);
        glm::vec2 uv10 = toUV(phi1, theta0);
        glm::vec2 uv11 = toUV(phi1, theta1);
        glm::vec2 uv01 = toUV(phi0, theta1);

        // Triangle 1
        verts.push_back({p00, uv00, n00});
        verts.push_back({p10, uv10, n10});
        verts.push_back({p11, uv11, n11});

        // Triangle 2
        verts.push_back({p00, uv00, n00});
        verts.push_back({p11, uv11, n11});
        verts.push_back({p01, uv01, n01});
      }
    }

    model->loadFromVertices(verts, "Sphere");
    return model;
  }

  // ── Plane (XZ ground, 1×1, centered at origin, facing +Y) ─────────────
  static OBJModel *createPlane() {
    auto *model = new OBJModel();
    const float h = 0.5f;
    const glm::vec3 n = {0, 1, 0};

    std::vector<V> verts = {
        {{-h, 0, h}, {0, 0}, n}, {{h, 0, h}, {1, 0}, n},
        {{h, 0, -h}, {1, 1}, n}, {{-h, 0, h}, {0, 0}, n},
        {{h, 0, -h}, {1, 1}, n}, {{-h, 0, -h}, {0, 1}, n},
    };

    model->loadFromVertices(verts, "Plane");
    return model;
  }

  // ── Cylinder (radius 0.5, height 1, centered at origin, Y-up) ─────────
  static OBJModel *createCylinder(int segments = 32) {
    auto *model = new OBJModel();
    std::vector<V> verts;
    verts.reserve(segments * 12);

    const float r = 0.5f, halfH = 0.5f;

    for (int i = 0; i < segments; ++i) {
      float a0 = 2.0f * (float)M_PI * i / segments;
      float a1 = 2.0f * (float)M_PI * (i + 1) / segments;
      float c0 = cosf(a0), s0 = sinf(a0);
      float c1 = cosf(a1), s1 = sinf(a1);

      float u0 = (float)i / segments;
      float u1 = (float)(i + 1) / segments;

      // Side quad (two triangles)
      glm::vec3 n0 = {c0, 0, s0};
      glm::vec3 n1 = {c1, 0, s1};

      glm::vec3 bl = {r * c0, -halfH, r * s0};
      glm::vec3 br = {r * c1, -halfH, r * s1};
      glm::vec3 tr = {r * c1, halfH, r * s1};
      glm::vec3 tl = {r * c0, halfH, r * s0};

      verts.push_back({bl, {u0, 0}, n0});
      verts.push_back({br, {u1, 0}, n1});
      verts.push_back({tr, {u1, 1}, n1});
      verts.push_back({bl, {u0, 0}, n0});
      verts.push_back({tr, {u1, 1}, n1});
      verts.push_back({tl, {u0, 1}, n0});

      // Top cap triangle
      verts.push_back({{0, halfH, 0}, {0.5f, 0.5f}, {0, 1, 0}});
      verts.push_back({tl, {c0 * 0.5f + 0.5f, s0 * 0.5f + 0.5f}, {0, 1, 0}});
      verts.push_back({tr, {c1 * 0.5f + 0.5f, s1 * 0.5f + 0.5f}, {0, 1, 0}});

      // Bottom cap triangle
      verts.push_back({{0, -halfH, 0}, {0.5f, 0.5f}, {0, -1, 0}});
      verts.push_back({br, {c1 * 0.5f + 0.5f, s1 * 0.5f + 0.5f}, {0, -1, 0}});
      verts.push_back({bl, {c0 * 0.5f + 0.5f, s0 * 0.5f + 0.5f}, {0, -1, 0}});
    }

    model->loadFromVertices(verts, "Cylinder");
    return model;
  }

  // ── Cone (radius 0.5, height 1, base at Y=-0.5, tip at Y=0.5) ─────────
  static OBJModel *createCone(int segments = 32) {
    auto *model = new OBJModel();
    std::vector<V> verts;
    verts.reserve(segments * 6);

    const float r = 0.5f, halfH = 0.5f;
    const glm::vec3 tip = {0, halfH, 0};

    for (int i = 0; i < segments; ++i) {
      float a0 = 2.0f * (float)M_PI * i / segments;
      float a1 = 2.0f * (float)M_PI * (i + 1) / segments;
      float c0 = cosf(a0), s0 = sinf(a0);
      float c1 = cosf(a1), s1 = sinf(a1);

      glm::vec3 b0 = {r * c0, -halfH, r * s0};
      glm::vec3 b1 = {r * c1, -halfH, r * s1};

      // Side normal (average of face normal)
      glm::vec3 sideN = glm::normalize(glm::vec3(c0 + c1, 2.0f * r, s0 + s1));

      // Side triangle
      verts.push_back({tip, {0.5f, 1}, sideN});
      verts.push_back({b0, {(float)i / segments, 0}, sideN});
      verts.push_back({b1, {(float)(i + 1) / segments, 0}, sideN});

      // Bottom cap triangle
      verts.push_back({{0, -halfH, 0}, {0.5f, 0.5f}, {0, -1, 0}});
      verts.push_back({b1, {c1 * 0.5f + 0.5f, s1 * 0.5f + 0.5f}, {0, -1, 0}});
      verts.push_back({b0, {c0 * 0.5f + 0.5f, s0 * 0.5f + 0.5f}, {0, -1, 0}});
    }

    model->loadFromVertices(verts, "Cone");
    return model;
  }
};
