#include "../CharacterModeling.h"
#include "../GeneratorRegistry.h"
#include "../GeneratorSchema.h"

namespace gen {
void registerCharacterGenerator() {
  GeneratorInfo info; info.name="character.v1";
  info.description="A deterministic stylized static humanoid character with semantic body, clothing, hair and accessory operations. This is the text-to-character target: use MCP character jobs rather than raw mesh data.";
  info.polyBudget=20000;
  info.schema=SchemaBuilder().number("height",1.75f,1.2f,2.4f,"Character height in metres.")
    .number("build",1.0f,.65f,1.55f,"Body breadth and limb mass.")
    .enumString("outfit","jacket",{"shirt","jacket","trousers","skirt","dress","armor","explorer"},"Base clothing template.")
    .boolean("hair",true,"Add a simple hair mass.").boolean("boots",true,"Add footwear.")
    .color("skinColor",glm::vec3(.66f,.40f,.25f),"Skin base colour.")
    .color("garmentColor",glm::vec3(.14f,.22f,.40f),"Clothing base colour.")
    .color("hairColor",glm::vec3(.08f,.035f,.015f),"Hair base colour.")
    .number("garmentRoughness",.72f,0,1,"Clothing roughness.")
    .integer("detail",1,0,5,"Implicit mesh resolution. 1=default (13k), 2=high (19k), 3=ultra (45k), 4=100k+, 5=extreme (150k+).")
    .objectArray("operations","Ordered semantic character edits; no raw vertices are accepted.").schema();
  info.build=[](const Params &p,uint32_t seed,std::vector<std::string>&warnings,std::string&error){
    nlohmann::json j; j["height"]=p.num("height",1.75f);j["build"]=p.num("build",1);j["outfit"]=p.str("outfit","jacket");j["hair"]=p.boolean("hair",true);j["boots"]=p.boolean("boots",true);j["skinColor"]={p.color("skinColor").x,p.color("skinColor").y,p.color("skinColor").z};j["garmentColor"]={p.color("garmentColor").x,p.color("garmentColor").y,p.color("garmentColor").z};j["hairColor"]={p.color("hairColor").x,p.color("hairColor").y,p.color("hairColor").z};j["garmentRoughness"]=p.num("garmentRoughness",.72f);j["detail"]=p.integer("detail",1);j["operations"]=p.array("operations");return modeling::buildHumanoid(j,seed,warnings,error);};
  GeneratorRegistry::instance().registerGenerator(std::move(info));
}
} // namespace gen

