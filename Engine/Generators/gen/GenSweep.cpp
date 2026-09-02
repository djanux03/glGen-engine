// sweep.v1 — a path-driven tube for the long, curved parts kitbash cannot express.
#include "../GeneratorRegistry.h"
#include "../GeneratorSchema.h"
#include "../MeshBuilder.h"
#include <algorithm>
#include <glm/gtx/norm.hpp>

namespace gen {
namespace {
bool vec3(const nlohmann::json &j, glm::vec3 &out) {
  if (!j.is_array() || j.size() != 3) return false;
  for (int i = 0; i < 3; ++i) if (!j[i].is_number()) return false;
  out = {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()}; return true;
}
}
void registerSweepGenerator() {
  GeneratorInfo info;
  info.name = "sweep.v1";
  info.description = "A round tube swept through ordered 3D control points. Use for horns, handles, tentacles, vines, cables, rails, blades and branch-like forms. points is an array of [x,y,z] points in metres; radius may taper from startRadius to endRadius.";
  info.polyBudget = 3000;
  info.schema = SchemaBuilder().objectArray("points", "At least two ordered [x,y,z] control points in metres.")
      .number("startRadius", 0.12f, 0.005f, 5.0f, "Tube radius at the first point.")
      .number("endRadius", 0.06f, 0.0f, 5.0f, "Tube radius at the final point; zero makes a point.")
      .integer("segments", 10, 3, 32, "Sides around the tube.")
      .boolean("capStart", true, "Close the first end.")
      .boolean("capEnd", true, "Close the final end.")
      .color("color", glm::vec3(0.38f, 0.25f, 0.12f), "Base colour, linear RGB.")
      .number("roughness", 0.75f, 0.0f, 1.0f, "Surface roughness.").schema();
  info.build = [](const Params &p, uint32_t, std::vector<std::string> &warnings, std::string &error) {
    std::vector<glm::vec3> points;
    for (size_t i = 0; i < p.array("points").size(); ++i) {
      glm::vec3 v; if (!vec3(p.array("points")[i], v)) { warnings.push_back("sweep point " + std::to_string(i) + " is not [x,y,z] and was skipped"); continue; }
      if (!points.empty() && glm::length2(v - points.back()) < 1e-8f) { warnings.push_back("duplicate sweep point " + std::to_string(i) + " was skipped"); continue; }
      points.push_back(v);
    }
    if (points.size() < 2) { error = "sweep.v1 needs at least two distinct points"; return std::unique_ptr<MeshData>{}; }
    MeshBuilder out; MaterialAsset mat; mat.id="sweep"; mat.baseColor=glm::vec4(p.color("color"),1); mat.roughness=p.num("roughness",.75f); out.beginSubmesh("Sweep",mat);
    const float r0=p.num("startRadius",.12f), r1=p.num("endRadius",.06f); const int sides=p.integer("segments",10);
    for (size_t i=0;i+1<points.size();++i) {
      const float a=static_cast<float>(i)/(points.size()-1), b=static_cast<float>(i+1)/(points.size()-1);
      out.addTaperedCylinder(points[i], glm::mix(r0,r1,a), points[i+1], glm::mix(r0,r1,b), sides,
        i==0 && p.boolean("capStart",true), i+2==points.size() && p.boolean("capEnd",true));
    }
    return out.build();
  };
  GeneratorRegistry::instance().registerGenerator(std::move(info));
}
} // namespace gen
