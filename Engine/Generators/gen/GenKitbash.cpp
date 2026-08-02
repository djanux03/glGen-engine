// kitbash.v1 — crates, fences, posts, barrels, doors, simple structures.
//
// The highest-leverage generator for a solo developer: it covers the entire
// long tail of "generic prop" that would otherwise each need their own
// bespoke generator or a modelling session. The AI describes an assembly of
// primitives with positions and sizes, which is a task LLMs are genuinely
// good at -- unlike emitting vertex arrays.
//
// Deliberately NOT a CSG system. Boolean operations need robust predicates,
// produce degenerate topology at coincident faces, and would be the single
// biggest source of broken output here. Overlapping solid primitives render
// identically for opaque props and cannot fail.

#include "../GenRandom.h"
#include "../GeneratorRegistry.h"
#include "../GeneratorSchema.h"
#include "../MeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace gen {
namespace {

glm::vec3 readVec3(const nlohmann::json &j, const char *key, glm::vec3 fallback) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_array() || it->size() < 3)
    return fallback;
  const auto &a = *it;
  for (int i = 0; i < 3; ++i)
    if (!a[i].is_number())
      return fallback;
  return glm::vec3(a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
}

} // namespace

void registerKitbashGenerator() {
  GeneratorInfo info;
  info.name = "kitbash.v1";
  info.description =
      "Assembles a prop from a list of primitive parts (box, cylinder, cone, "
      "sphere), each with its own position, size and rotation. Use this for "
      "crates, barrels, fences, posts, signs, planters, simple furniture and "
      "small structures. All units are metres in a Y-up space with the origin "
      "at the intended ground contact point. Parts may overlap freely -- they "
      "are solid primitives, not booleans.";
  info.polyBudget = 3000;
  info.schema =
      SchemaBuilder()
          .objectArray(
              "parts",
              "Array of parts. Each is an object: "
              "{\"shape\": \"box\"|\"cylinder\"|\"cone\"|\"sphere\", "
              "\"pos\": [x,y,z] (centre; for cylinder/cone this is the base "
              "centre), \"size\": [x,y,z] (box: full extents; cylinder/cone: "
              "[diameter, height, diameter]; sphere: [diameter,-,-]), "
              "\"rotation\": [xDeg,yDeg,zDeg] (optional), "
              "\"color\": [r,g,b] (optional, overrides the top-level colour), "
              "\"segments\": n (optional, sides for round shapes), "
              "\"taper\": 0..1 (optional, cylinder top radius as a fraction of "
              "the base)}.")
          .color("color", glm::vec3(0.55f, 0.42f, 0.28f),
                 "Default colour for parts that do not specify one.")
          .number("roughness", 0.8f, 0.0f, 1.0f, "Surface roughness.")
          .number("metallic", 0.0f, 0.0f, 1.0f, "Metalness.")
          .integer("roundSegments", 10, 3, 32,
                   "Default sides for cylinders and cones.")
          .number("jitter", 0.0f, 0.0f, 0.2f,
                  "Random per-part position/rotation wobble in metres. A "
                  "little stops a fence reading as machine-perfect.")
          .schema();

  info.build = [](const Params &p, uint32_t seed,
                  std::vector<std::string> &warnings,
                  std::string &error) -> std::unique_ptr<MeshData> {
    const nlohmann::json &parts = p.array("parts");
    if (parts.empty()) {
      error = "kitbash.v1 needs at least one entry in 'parts'";
      return nullptr;
    }

    MeshBuilder out;
    GenRandom rng(seed);

    const glm::vec3 defaultColor = p.color("color", glm::vec3(0.55f, 0.42f, 0.28f));
    const int defaultSegments = p.integer("roundSegments", 10);
    const float jitter = p.num("jitter", 0.0f);

    MaterialAsset mat;
    mat.id = "prop";
    mat.baseColor = glm::vec4(defaultColor, 1.0f);
    mat.roughness = p.num("roughness", 0.8f);
    mat.metallic = p.num("metallic", 0.0f);
    out.beginSubmesh("Prop", mat);

    // One submesh per distinct colour: MaterialAsset is per-submesh, so a
    // multi-coloured prop needs one submesh per colour rather than per part.
    // Parts are grouped by colour so a crate with metal bands is 2 draws, not
    // 20. Insertion-ordered so output stays deterministic.
    std::vector<std::pair<glm::vec3, std::vector<const nlohmann::json *>>> byColor;
    for (const auto &part : parts) {
      if (!part.is_object()) {
        warnings.push_back("skipped a 'parts' entry that is not an object");
        continue;
      }
      const glm::vec3 c = readVec3(part, "color", defaultColor);
      auto it = std::find_if(byColor.begin(), byColor.end(), [&](const auto &e) {
        const glm::vec3 d = e.first - c;
        return glm::dot(d, d) < 1e-8f;
      });
      if (it == byColor.end())
        byColor.push_back({c, {&part}});
      else
        it->second.push_back(&part);
    }

    bool first = true;
    int groupIndex = 0;
    for (const auto &group : byColor) {
      if (!first) {
        MaterialAsset gm = mat;
        gm.id = "prop_" + std::to_string(groupIndex);
        gm.baseColor = glm::vec4(group.first, 1.0f);
        out.beginSubmesh("Prop" + std::to_string(groupIndex), gm);
      } else {
        out.current().material.baseColor = glm::vec4(group.first, 1.0f);
        first = false;
      }
      ++groupIndex;

      for (const nlohmann::json *partPtr : group.second) {
        const nlohmann::json &part = *partPtr;
        const std::string shape = part.value("shape", std::string("box"));
        glm::vec3 pos = readVec3(part, "pos", glm::vec3(0.0f));
        const glm::vec3 size =
            glm::max(readVec3(part, "size", glm::vec3(1.0f)), glm::vec3(1e-4f));
        glm::vec3 rot = readVec3(part, "rotation", glm::vec3(0.0f));
        const int segments =
            std::clamp(part.value("segments", defaultSegments), 3, 32);

        if (jitter > 0.0f) {
          pos += rng.inSphere() * jitter;
          rot += glm::vec3(rng.signedUnit(), rng.signedUnit(), rng.signedUnit()) *
                 (jitter * 20.0f);
        }

        glm::mat4 m = glm::translate(glm::mat4(1.0f), pos);
        if (glm::dot(rot, rot) > 1e-8f) {
          m = glm::rotate(m, glm::radians(rot.z), glm::vec3(0, 0, 1));
          m = glm::rotate(m, glm::radians(rot.y), glm::vec3(0, 1, 0));
          m = glm::rotate(m, glm::radians(rot.x), glm::vec3(1, 0, 0));
        }
        out.pushTransform(m);

        if (shape == "box") {
          out.addBox(-size * 0.5f, size * 0.5f);
        } else if (shape == "cylinder" || shape == "cone") {
          const float taper =
              (shape == "cone") ? 0.0f
                                : std::clamp(part.value("taper", 1.0f), 0.0f, 4.0f);
          const float r = size.x * 0.5f;
          out.addTaperedCylinder(glm::vec3(0.0f), r, glm::vec3(0.0f, size.y, 0.0f),
                                 r * taper, segments);
        } else if (shape == "sphere") {
          // Icosphere detail from the requested segment count, so "segments"
          // means roughly the same thing (smoothness) for every round shape.
          const int detail = std::clamp(segments / 8, 1, 3);
          out.addIcosphere(glm::vec3(0.0f, size.y * 0.5f, 0.0f), size.x * 0.5f,
                           detail);
          out.recomputeSmoothNormals();
        } else {
          warnings.push_back("unknown part shape '" + shape +
                             "' skipped (expected box, cylinder, cone or "
                             "sphere)");
        }
        out.popTransform();
      }
    }

    return out.build();
  };

  GeneratorRegistry::instance().registerGenerator(std::move(info));
}

} // namespace gen
