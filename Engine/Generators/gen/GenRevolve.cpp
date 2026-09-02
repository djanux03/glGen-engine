// revolve.v1 — lathed forms are compact, natural descriptions for many props.
#include "../GeneratorRegistry.h"
#include "../GeneratorSchema.h"
#include "../MeshBuilder.h"
#include <algorithm>
namespace gen {
void registerRevolveGenerator() {
  GeneratorInfo info; info.name="revolve.v1";
  info.description="A surface of revolution around Y from an ordered profile. profile entries are [radius,height] in metres; use it for pottery, bottles, helmets, lamps, columns, mushrooms, barrels and cups.";
  info.polyBudget=3000;
  info.schema=SchemaBuilder().objectArray("profile", "At least two ordered [radius,height] profile points in metres.")
    .integer("segments",16,3,64,"Sides around the form.").color("color",glm::vec3(.45f,.35f,.24f),"Base colour, linear RGB.")
    .number("roughness",.65f,0,1,"Surface roughness.").boolean("smooth",true,"Average normals for a smooth lathed surface.").schema();
  info.build=[](const Params&p,uint32_t,std::vector<std::string>&warnings,std::string&error){
    std::vector<glm::vec2> profile; for(size_t i=0;i<p.array("profile").size();++i){const auto&e=p.array("profile")[i]; if(!e.is_array()||e.size()!=2||!e[0].is_number()||!e[1].is_number()){warnings.push_back("revolve profile entry "+std::to_string(i)+" is not [radius,height] and was skipped");continue;} profile.emplace_back(std::max(0.0f,e[0].get<float>()),e[1].get<float>());}
    if(profile.size()<2){error="revolve.v1 needs at least two valid profile points";return std::unique_ptr<MeshData>{};}
    MeshBuilder out;MaterialAsset m;m.id="revolve";m.baseColor=glm::vec4(p.color("color"),1);m.roughness=p.num("roughness",.65f);out.beginSubmesh("Revolve",m);out.addRevolve(profile,p.integer("segments",16));if(p.boolean("smooth",true))out.recomputeSmoothNormals();else out.recomputeFlatNormals();
    float minY=1e30f;for(const auto&v:out.current().vertices)minY=std::min(minY,v.pos.y);for(auto&v:out.current().vertices)v.pos.y-=minY;return out.build();};
  GeneratorRegistry::instance().registerGenerator(std::move(info));
}
} // namespace gen
