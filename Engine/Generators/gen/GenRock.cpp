// rock.v1 — boulders, scree and cliff chunks.
//
// An icosphere pushed around by fractal noise, optionally sliced by planar
// cuts. The icosphere base matters: a lat/long sphere bunches vertices at the
// poles, so noise displacement there produces visibly pinched artifacts, while
// an icosphere's near-uniform triangle density displaces evenly in every
// direction.

#include "../GenRandom.h"
#include "../GeneratorRegistry.h"
#include "../GeneratorSchema.h"
#include "../MeshBuilder.h"
#include "PerlinNoise.h"

#include <algorithm>
#include <cmath>

namespace gen {

namespace {

// 3D fractal noise from the engine's existing 2D Perlin: three decorrelated
// 2D slices summed. Cheaper than a real 3D noise and, for surface
// displacement on a closed blob, indistinguishable.
float fbm3(const PerlinNoise &noise, glm::vec3 p, int octaves, float lacunarity,
           float gain) {
  float sum = 0.0f, amp = 1.0f, norm = 0.0f, freq = 1.0f;
  for (int i = 0; i < octaves; ++i) {
    const glm::vec3 q = p * freq;
    const float n = (noise.noise(q.x, q.y) + noise.noise(q.y + 31.7f, q.z) +
                     noise.noise(q.z + 71.3f, q.x)) /
                    3.0f;
    sum += n * amp;
    norm += amp;
    amp *= gain;
    freq *= lacunarity;
  }
  return norm > 0.0f ? sum / norm : 0.0f;
}

} // namespace

void registerRockGenerator() {
  GeneratorInfo info;
  info.name = "rock.v1";
  info.description =
      "A boulder, scree chunk or cliff fragment: a noise-displaced sphere with "
      "optional flat cut faces. Good for anything from fist-sized gravel to a "
      "house-sized erratic. Scale is in metres.";
  info.polyBudget = 1500;
  info.schema =
      SchemaBuilder()
          .number("radius", 0.6f, 0.05f, 20.0f,
                  "Base radius in metres before displacement.")
          .integer("detail", 2, 0, 4,
                   "Subdivision level. 2 is right for most props; 3+ only for "
                   "hero rocks seen up close (triangle count quadruples each "
                   "step).")
          .number("roughness", 0.35f, 0.0f, 1.0f,
                  "How far the surface is pushed in and out. 0 is a smooth "
                  "pebble, 1 is a jagged shard.")
          .number("noiseScale", 2.2f, 0.3f, 12.0f,
                  "Frequency of the displacement. Low values give big lumpy "
                  "forms, high values give fine crumbly detail.")
          .integer("octaves", 3, 1, 6,
                   "Layers of noise. More adds fine detail on top of the "
                   "large forms.")
          .color("stretch", glm::vec3(1.0f, 0.75f, 0.95f),
                 "Per-axis scale before displacement, as [x,y,z]. Flattening "
                 "Y is what makes a rock sit like a boulder rather than a "
                 "ball.")
          .integer("flatCuts", 1, 0, 6,
                   "Planar slices flattening random faces -- reads as cleaved "
                   "stone. 0 for weathered river rock.")
          .number("cutDepth", 0.22f, 0.0f, 0.6f,
                  "How deep the flat cuts bite, as a fraction of radius.")
          .number("sinkFlat", 0.15f, 0.0f, 0.6f,
                  "Flattens the underside so the rock sits on ground instead "
                  "of balancing on a curved point.")
          .color("color", glm::vec3(0.42f, 0.40f, 0.38f),
                 "Base colour, linear RGB.")
          .boolean("smooth", true,
                   "Smooth (averaged) normals. False gives faceted, low-poly "
                   "shading.")
          .schema();

  info.build = [](const Params &p, uint32_t seed,
                  std::vector<std::string> &warnings,
                  std::string &) -> std::unique_ptr<MeshData> {
    MeshBuilder out;
    GenRandom rng(seed);
    const PerlinNoise noise(seed);

    const float radius = p.num("radius", 0.6f);
    const float roughness = p.num("roughness", 0.35f);
    const float noiseScale = p.num("noiseScale", 2.2f);
    const int octaves = p.integer("octaves", 3);
    const glm::vec3 stretch = p.color("stretch", glm::vec3(1.0f, 0.75f, 0.95f));
    const int flatCuts = p.integer("flatCuts", 1);
    const float cutDepth = p.num("cutDepth", 0.22f);
    const float sinkFlat = p.num("sinkFlat", 0.15f);

    MaterialAsset mat;
    mat.id = "rock";
    mat.baseColor = glm::vec4(p.color("color", glm::vec3(0.42f, 0.40f, 0.38f)), 1.0f);
    mat.roughness = 0.85f;
    mat.metallic = 0.0f;
    out.beginSubmesh("Rock", mat);

    out.addIcosphere(glm::vec3(0.0f), radius, p.integer("detail", 2));

    // Random cut planes, chosen once so every vertex sees the same set.
    struct Plane {
      glm::vec3 n;
      float d;
    };
    std::vector<Plane> planes;
    planes.reserve(static_cast<size_t>(std::max(0, flatCuts)));
    for (int i = 0; i < flatCuts; ++i) {
      glm::vec3 n = rng.inSphere();
      if (glm::dot(n, n) < 1e-6f)
        n = glm::vec3(0.0f, 1.0f, 0.0f);
      n = glm::normalize(n);
      planes.push_back({n, radius * (1.0f - cutDepth * rng.range(0.4f, 1.0f))});
    }

    const glm::vec3 offset(rng.range(-50.0f, 50.0f), rng.range(-50.0f, 50.0f),
                           rng.range(-50.0f, 50.0f));

    out.displace([&](const glm::vec3 &pos, const glm::vec3 &) {
      const glm::vec3 dir = glm::normalize(pos);
      glm::vec3 p3 = dir * stretch * radius;

      const float n = fbm3(noise, dir * noiseScale + offset, octaves, 2.0f, 0.5f);
      p3 *= 1.0f + n * roughness;

      // Cut planes: anything past the plane collapses onto it, producing a
      // genuinely flat face rather than a dent.
      for (const Plane &pl : planes) {
        const float dist = glm::dot(p3, pl.n);
        if (dist > pl.d)
          p3 -= pl.n * (dist - pl.d);
      }

      // Flatten the base so it rests on terrain.
      const float floorY = -radius * stretch.y * (1.0f - sinkFlat);
      if (p3.y < floorY)
        p3.y = floorY;
      return p3;
    });

    if (p.boolean("smooth", true))
      out.recomputeSmoothNormals();
    else
      out.recomputeFlatNormals();

    // Sit the rock on y=0 so placement code can put it straight on the ground.
    float minY = 1e30f;
    for (const auto &v : out.current().vertices)
      minY = std::min(minY, v.pos.y);
    for (auto &v : out.current().vertices)
      v.pos.y -= minY;

    if (roughness > 0.75f && p.integer("detail", 2) < 2)
      warnings.push_back(
          "high roughness at low detail: the displacement has too few vertices "
          "to resolve and will read as spiky rather than rugged (raise "
          "'detail' to 2+)");

    return out.build();
  };

  GeneratorRegistry::instance().registerGenerator(std::move(info));
}

} // namespace gen
