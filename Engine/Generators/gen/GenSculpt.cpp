// sculpt.v1 — deterministic, stamp-based organic props.
//
// This is intentionally not an arbitrary vertex-array or boolean system.
// An AI is good at describing a small ordered list of broad surface edits;
// it is not good at maintaining watertight indexed topology.  Starting from
// an icosphere keeps triangle density even, and every stamp only moves the
// existing surface, so recipes remain reproducible and renderer-safe.

#include "../GeneratorRegistry.h"
#include "../GeneratorSchema.h"
#include "../MeshBuilder.h"

#include <algorithm>
#include <cmath>

namespace gen {
namespace {

bool readVec3(const nlohmann::json &j, const char *key, glm::vec3 &out) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_array() || it->size() != 3)
    return false;
  for (int i = 0; i < 3; ++i)
    if (!(*it)[i].is_number())
      return false;
  out = glm::vec3((*it)[0].get<float>(), (*it)[1].get<float>(),
                  (*it)[2].get<float>());
  return true;
}

struct Stamp {
  enum class Kind { Inflate, Carve, Flatten, Pinch } kind;
  glm::vec3 direction;
  float radius;
  float strength;
  float falloff;
};

} // namespace

void registerSculptGenerator() {
  GeneratorInfo info;
  info.name = "sculpt.v1";
  info.description =
      "A sculpted organic or hand-shaped prop. Starts as an evenly tessellated "
      "sphere and applies ordered radial stamps, producing rocks, carved "
      "figures, stylised creatures and ornamental forms without raw mesh data. "
      "Each stamp has type inflate|carve|flatten|pinch, centre [x,y,z] "
      "(a direction from the origin), radius in metres, strength in metres, "
      "and optional falloff (1-8).";
  info.polyBudget = 3000;
  info.schema =
      SchemaBuilder()
          .number("radius", 0.75f, 0.05f, 20.0f,
                  "Radius of the unsculpted base in metres.")
          .integer("detail", 2, 0, 4,
                   "Icosphere subdivision level. 2 is suitable for most "
                   "props; 3 is for close-up silhouettes (each level "
                   "quadruples triangle count).")
          .objectArray(
              "stamps",
              "Ordered surface edits. Each object is {type: "
              "inflate|carve|flatten|pinch, centre: [x,y,z], radius: metres, "
              "strength: metres, falloff: 1..8 (optional)}. centre is a "
              "direction from the origin, not a world-space location.")
          .color("color", glm::vec3(0.50f, 0.42f, 0.32f),
                 "Base colour, linear RGB.")
          .number("roughness", 0.82f, 0.0f, 1.0f, "Surface roughness.")
          .boolean("smooth", true,
                   "Average normals after sculpting. False gives deliberately "
                   "faceted low-poly shading.")
          .schema();

  info.build = [](const Params &p, uint32_t,
                  std::vector<std::string> &warnings,
                  std::string &) -> std::unique_ptr<MeshData> {
    const float baseRadius = p.num("radius", 0.75f);
    std::vector<Stamp> stamps;
    const nlohmann::json &input = p.array("stamps");
    constexpr size_t kMaxStamps = 64;
    for (size_t i = 0; i < input.size(); ++i) {
      if (i >= kMaxStamps) {
        warnings.push_back("sculpt.v1 accepts at most 64 stamps; later stamps were ignored");
        break;
      }
      const nlohmann::json &entry = input[i];
      if (!entry.is_object()) {
        warnings.push_back("sculpt stamp " + std::to_string(i) + " is not an object and was skipped");
        continue;
      }
      glm::vec3 centre;
      if (!readVec3(entry, "centre", centre) || glm::dot(centre, centre) < 1e-8f) {
        warnings.push_back("sculpt stamp " + std::to_string(i) +
                           " needs a non-zero three-number 'centre' and was skipped");
        continue;
      }
      const std::string type = entry.value("type", std::string("inflate"));
      Stamp::Kind kind;
      if (type == "inflate") kind = Stamp::Kind::Inflate;
      else if (type == "carve") kind = Stamp::Kind::Carve;
      else if (type == "flatten") kind = Stamp::Kind::Flatten;
      else if (type == "pinch") kind = Stamp::Kind::Pinch;
      else {
        warnings.push_back("sculpt stamp " + std::to_string(i) + " has unknown type '" +
                           type + "' and was skipped");
        continue;
      }
      const float stampRadius = std::clamp(entry.value("radius", baseRadius * 0.35f),
                                           0.01f, baseRadius * 3.0f);
      const float strength = std::clamp(entry.value("strength", baseRadius * 0.15f),
                                        -baseRadius * 1.5f, baseRadius * 1.5f);
      const float falloff = std::clamp(entry.value("falloff", 2.0f), 1.0f, 8.0f);
      stamps.push_back({kind, glm::normalize(centre), stampRadius, strength, falloff});
    }

    MeshBuilder out;
    MaterialAsset mat;
    mat.id = "sculpt";
    mat.baseColor = glm::vec4(p.color("color", glm::vec3(0.50f, 0.42f, 0.32f)), 1.0f);
    mat.roughness = p.num("roughness", 0.82f);
    out.beginSubmesh("Sculpt", mat);
    out.addIcosphere(glm::vec3(0.0f), baseRadius, p.integer("detail", 2));

    out.displace([&](const glm::vec3 &pos, const glm::vec3 &) {
      glm::vec3 direction = glm::normalize(pos);
      float radialDistance = baseRadius;
      for (const Stamp &stamp : stamps) {
        const float angle = std::acos(std::clamp(glm::dot(direction, stamp.direction),
                                                 -1.0f, 1.0f));
        const float angularRadius = std::min(stamp.radius / baseRadius, glm::pi<float>());
        if (angle >= angularRadius)
          continue;
        const float weight = std::pow(1.0f - angle / angularRadius, stamp.falloff);
        switch (stamp.kind) {
        case Stamp::Kind::Inflate: radialDistance += stamp.strength * weight; break;
        case Stamp::Kind::Carve: radialDistance -= stamp.strength * weight; break;
        case Stamp::Kind::Flatten: {
          // A tangent plane creates a true flattened area, unlike merely
          // subtracting a radial bump which reads as a circular dent.
          const float denom = std::max(0.12f, glm::dot(direction, stamp.direction));
          const float target = std::clamp((baseRadius + stamp.strength) / denom,
                                          baseRadius * 0.08f, baseRadius * 2.5f);
          radialDistance += (target - radialDistance) * weight;
          break;
        }
        case Stamp::Kind::Pinch:
          direction = glm::normalize(glm::mix(
              direction, stamp.direction,
              std::clamp(std::abs(stamp.strength) / baseRadius * weight, 0.0f, 0.92f)));
          break;
        }
        // Avoid inverted radial geometry, which makes folds and invalid
        // acceleration-structure input instead of a useful sculpted form.
        radialDistance = std::max(baseRadius * 0.08f, radialDistance);
      }
      return direction * radialDistance;
    });

    if (p.boolean("smooth", true))
      out.recomputeSmoothNormals();
    else
      out.recomputeFlatNormals();

    // As with every terrain-placeable generator, make y=0 the contact plane.
    float minY = 1e30f;
    for (const auto &v : out.current().vertices)
      minY = std::min(minY, v.pos.y);
    for (auto &v : out.current().vertices)
      v.pos.y -= minY;
    return out.build();
  };

  GeneratorRegistry::instance().registerGenerator(std::move(info));
}

} // namespace gen
