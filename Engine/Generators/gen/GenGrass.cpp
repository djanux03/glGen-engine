// grass.v1 — ground-cover clumps, tufts, ferns and reeds.
//
// The tightest poly budget in the set, and the reason is architectural rather
// than aesthetic: grass is placed by the ten-thousand through
// VulkanRenderer::setVegetationBatches(), so every triangle here is multiplied
// by the instance count. ScatterLayer keeps grass out of the TLAS entirely for
// the same reason (see its castRayShadow comment).

#include "../GenRandom.h"
#include "../GeneratorRegistry.h"
#include "../GeneratorSchema.h"
#include "../MeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace gen {

void registerGrassGenerator() {
  GeneratorInfo info;
  info.name = "grass.v1";
  info.description =
      "A clump of ground cover: grass blades, tufts, ferns or reeds. Built "
      "from curved cards with the pivot at the base. Keep the blade count low "
      "-- this mesh gets instanced tens of thousands of times across the "
      "terrain.";
  info.polyBudget = 200;
  info.schema =
      SchemaBuilder()
          .integer("blades", 7, 1, 24,
                   "Blades in the clump. Every blade is multiplied by the "
                   "instance count across the terrain, so stay low; variety "
                   "is cheaper from placement jitter than from geometry.")
          .number("height", 0.35f, 0.02f, 3.0f,
                  "Blade height in metres. ~0.3 for meadow grass, 1.5+ for "
                  "reeds.")
          .number("heightVariance", 0.4f, 0.0f, 1.0f,
                  "Spread of blade heights within the clump. 0 makes a mown "
                  "look, high values a wild one.")
          .number("width", 0.035f, 0.002f, 0.4f,
                  "Blade width at the base in metres.")
          .number("spread", 0.09f, 0.0f, 1.0f,
                  "Radius the blade bases are scattered over.")
          .number("bendDeg", 55.0f, 0.0f, 150.0f,
                  "How far a blade arcs over from base to tip. Low is upright "
                  "reeds, high is soft drooping meadow grass.")
          .integer("segments", 2, 1, 5,
                   "Segments per blade. 2 is enough for the curve to read; "
                   "raise only for tall reeds seen close up.")
          .color("color", glm::vec3(0.26f, 0.44f, 0.16f),
                 "Base colour, linear RGB. Per-instance tint is applied on top "
                 "by the scatter system.")
          .color("tipColor", glm::vec3(0.42f, 0.52f, 0.20f),
                 "Colour at the tips. Only used when 'tipFade' is on.")
          .boolean("tipFade", true,
                   "Lighten the tips toward tipColor. Cheap way to stop a "
                   "field reading as one flat green.")
          .schema();

  info.build = [](const Params &p, uint32_t seed,
                  std::vector<std::string> &warnings,
                  std::string &) -> std::unique_ptr<MeshData> {
    MeshBuilder out;
    GenRandom rng(seed);

    const int blades = p.integer("blades", 7);
    const float height = p.num("height", 0.35f);
    const float variance = p.num("heightVariance", 0.4f);
    const float width = p.num("width", 0.035f);
    const float spread = p.num("spread", 0.09f);
    const float bend = p.num("bendDeg", 55.0f);
    const int segments = p.integer("segments", 2);

    MaterialAsset mat;
    mat.id = "grass";
    mat.baseColor = glm::vec4(p.color("color", glm::vec3(0.26f, 0.44f, 0.16f)), 1.0f);
    mat.roughness = 0.8f;
    mat.alphaCutoff = 0.5f;
    out.beginSubmesh("Grass", mat);

    for (int i = 0; i < blades; ++i) {
      // Square-root radius keeps the clump evenly dense instead of piling
      // blades at the centre.
      const float r = spread * std::sqrt(rng.unit());
      const float a = rng.range(0.0f, 6.2831853f);
      const glm::vec3 base(std::cos(a) * r, 0.0f, std::sin(a) * r);

      glm::mat4 m = glm::translate(glm::mat4(1.0f), base);
      m = glm::rotate(m, rng.range(0.0f, 6.2831853f), glm::vec3(0, 1, 0));
      // Blades further from the centre lean further out -- a clump splays.
      const float outward = (spread > 1e-5f) ? (r / spread) : 0.0f;
      m = glm::rotate(m, glm::radians(outward * rng.range(5.0f, 22.0f)),
                      glm::vec3(1, 0, 0));

      out.pushTransform(m);
      out.addCard(width * rng.range(0.7f, 1.3f),
                  height * (1.0f + rng.signedUnit() * variance),
                  bend * rng.range(0.7f, 1.3f), segments);
      out.popTransform();
    }

    if (p.boolean("tipFade", true)) {
      // Vertex-level tint via UV.y (0 at base, 1 at tip). The engine has no
      // vertex-colour channel, so this bakes into the material's tip colour
      // only when a texture generator is attached; on its own it is a no-op
      // beyond documenting intent.
      const glm::vec3 tip = p.color("tipColor", glm::vec3(0.42f, 0.52f, 0.20f));
      MaterialAsset &mref = out.current().material;
      mref.emissiveColor = glm::vec3(0.0f);
      // Blend a little of the tip colour into the base so the clump doesn't
      // read as one flat value even without a texture.
      mref.baseColor =
          glm::vec4(glm::mix(glm::vec3(mref.baseColor), tip, 0.25f), 1.0f);
    }

    if (blades * segments * 2 > 200)
      warnings.push_back(
          "this clump exceeds the 200-triangle grass budget; grass is "
          "instanced tens of thousands of times, so prefer fewer blades and "
          "more placement density");

    return out.build();
  };

  GeneratorRegistry::instance().registerGenerator(std::move(info));
}

} // namespace gen
