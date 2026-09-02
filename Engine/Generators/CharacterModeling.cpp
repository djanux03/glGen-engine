#include "CharacterModeling.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <glm/gtx/norm.hpp>
#include <sstream>
#include <unordered_map>

namespace gen::modeling {
namespace {

using json = nlohmann::json;
using Field = std::function<float(const glm::vec3 &)>;

constexpr float kIso = 0.0f;

float sphere(const glm::vec3 &p, const glm::vec3 &c, float r) {
  return glm::length(p - c) - r;
}

float capsule(const glm::vec3 &p, const glm::vec3 &a, const glm::vec3 &b,
              float r) {
  const glm::vec3 ab = b - a;
  const float denom = glm::dot(ab, ab);
  const float t = denom > 1e-8f ? std::clamp(glm::dot(p - a, ab) / denom, 0.0f, 1.0f)
                                 : 0.0f;
  return glm::length(p - (a + ab * t)) - r;
}

float smoothUnion(float a, float b, float k) {
  const float h = std::clamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
  return glm::mix(b, a, h) - k * h * (1.0f - h);
}

float smoothSubtract(float a, float b, float k) {
  const float h = std::clamp(0.5f - 0.5f * (a + b) / k, 0.0f, 1.0f);
  return glm::mix(a, -b, h) + k * h * (1.0f - h);
}

glm::vec3 color(const json &j, const char *key, glm::vec3 fallback) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_array() || it->size() != 3)
    return fallback;
  for (int i = 0; i < 3; ++i)
    if (!(*it)[i].is_number())
      return fallback;
  return glm::vec3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
}

struct CharacterSpec {
  float height = 1.75f;
  float build = 1.0f;
  std::string outfit = "jacket";
  bool hair = true;
  bool boots = true;
  bool belt = false;
  glm::vec3 skin{0.72f, 0.48f, 0.34f};
  glm::vec3 garment{0.08f, 0.14f, 0.28f};
  glm::vec3 jacketColor{0.08f, 0.14f, 0.28f};
  glm::vec3 shirtColor{0.85f, 0.85f, 0.82f};
  glm::vec3 trousersColor{0.12f, 0.12f, 0.16f};
  glm::vec3 bootsColor{0.32f, 0.18f, 0.09f};
  glm::vec3 beltColor{0.25f, 0.15f, 0.08f};
  glm::vec3 hairColor{0.05f, 0.04f, 0.04f};
  float garmentRoughness = 0.65f;
  int detail = 1;
};

CharacterSpec parse(const json &params, std::vector<std::string> &warnings) {
  CharacterSpec s;
  s.height = std::clamp(params.value("height", s.height), 1.2f, 2.4f);
  s.build = std::clamp(params.value("build", s.build), 0.65f, 1.55f);
  s.outfit = params.value("outfit", s.outfit);
  s.hair = params.value("hair", s.hair);
  s.boots = params.value("boots", s.boots);
  s.skin = color(params, "skinColor", s.skin);
  s.garment = color(params, "garmentColor", s.garment);
  if (params.contains("jacketColor")) s.jacketColor = color(params, "jacketColor", s.jacketColor);
  else s.jacketColor = s.garment;
  s.shirtColor = color(params, "shirtColor", s.shirtColor);
  s.trousersColor = color(params, "trousersColor", s.trousersColor);
  s.bootsColor = color(params, "bootsColor", s.bootsColor);
  s.beltColor = color(params, "beltColor", s.beltColor);
  s.hairColor = color(params, "hairColor", s.hairColor);
  s.garmentRoughness = std::clamp(params.value("garmentRoughness", s.garmentRoughness), 0.0f, 1.0f);
  s.detail = std::clamp(params.value("detail", s.detail), 0, 5);

  if (s.outfit == "explorer") {
    s.belt = true;
  }

  if (!params.contains("operations") || !params["operations"].is_array())
    return s;

  for (const json &op : params["operations"]) {
    if (!op.is_object() || !op.contains("op") || !op["op"].is_string()) {
      warnings.push_back("character operation without a string 'op' was skipped");
      continue;
    }
    const std::string name = op["op"].get<std::string>();
    if (name == "reshape") {
      const float amount = std::clamp(op.value("amount", 0.0f), -0.35f, 0.35f);
      const std::string region = op.value("region", std::string("torso"));
      if (region == "torso" || region == "shoulders") s.build = std::clamp(s.build + amount, .65f, 1.55f);
      else if (region == "height") s.height = std::clamp(s.height + amount, 1.2f, 2.4f);
      else warnings.push_back("character reshape region '" + region + "' is not supported");
    } else if (name == "add_garment") {
      s.outfit = op.value("type", s.outfit);
      if (op.contains("jacketColor")) s.jacketColor = color(op, "jacketColor", s.jacketColor);
      if (op.contains("shirtColor")) s.shirtColor = color(op, "shirtColor", s.shirtColor);
      if (op.contains("trousersColor")) s.trousersColor = color(op, "trousersColor", s.trousersColor);
      if (s.outfit == "explorer") s.belt = true;
    } else if (name == "add_hair") {
      s.hair = true;
      if (op.contains("color")) s.hairColor = color(op, "color", s.hairColor);
    } else if (name == "add_accessory") {
      const std::string type = op.value("type", "");
      if (type == "utility_belt" || type == "belt") {
        s.belt = true;
        if (op.contains("color")) s.beltColor = color(op, "color", s.beltColor);
      }
    } else if (name == "assign_material") {
      const std::string region = op.value("region", "");
      if (region == "skin") s.skin = color(op, "color", s.skin);
      else if (region == "jacket") s.jacketColor = color(op, "color", s.jacketColor);
      else if (region == "shirt") s.shirtColor = color(op, "color", s.shirtColor);
      else if (region == "trousers") s.trousersColor = color(op, "color", s.trousersColor);
      else if (region == "boots") s.bootsColor = color(op, "color", s.bootsColor);
      else if (region == "belt") s.beltColor = color(op, "color", s.beltColor);
      else if (region == "hair") s.hairColor = color(op, "color", s.hairColor);
    } else if (name == "add_feature" || name == "mirror" || name == "smooth" || name == "remesh" || name == "simplify") {
      // Retained in operation graph.
    } else {
      warnings.push_back("unknown character operation '" + name + "' was skipped");
    }
  }
  return s;
}

glm::vec3 gradient(const Field &f, const glm::vec3 &p) {
  constexpr float e = 0.003f;
  const glm::vec3 g(f(p + glm::vec3(e, 0, 0)) - f(p - glm::vec3(e, 0, 0)),
                    f(p + glm::vec3(0, e, 0)) - f(p - glm::vec3(0, e, 0)),
                    f(p + glm::vec3(0, 0, e)) - f(p - glm::vec3(0, 0, e)));
  return glm::length2(g) > 1e-10f ? glm::normalize(g) : glm::vec3(0, 1, 0);
}

struct Surface {
  std::vector<MeshVertex> vertices;
  std::vector<uint32_t> indices;
  std::unordered_map<std::string, uint32_t> byPosition;
  glm::vec3 mn{1e30f}, mx{-1e30f};
  uint32_t vertex(const glm::vec3 &p, const glm::vec3 &n, int region) {
    const glm::ivec3 q = glm::ivec3(glm::round(p * 10000.0f));
    const std::string key = std::to_string(q.x) + ":" + std::to_string(q.y) + ":" + std::to_string(q.z);
    const auto it = byPosition.find(key);
    if (it != byPosition.end()) return it->second;
    MeshVertex v; v.pos = p; v.normal = n; v.uv = glm::vec2(p.x * 0.75f + 0.5f, p.y * 0.65f);
    const uint32_t index = static_cast<uint32_t>(vertices.size());
    vertices.push_back(v); byPosition.emplace(key, index); mn = glm::min(mn, p); mx = glm::max(mx, p); return index;
  }
};

} // namespace

std::unique_ptr<MeshData> buildHumanoid(const json &params, uint32_t,
                                        std::vector<std::string> &warnings,
                                        std::string &error) {
  const CharacterSpec s = parse(params, warnings);
  const float h = s.height, build = s.build;
  const float legTop = h * .52f, shoulder = h * .78f, headY = h * .90f;
  const float torsoR = .19f * build, limbR = .07f * build;
  const bool isExplorer = (s.outfit == "explorer");

  Field skin = [=](const glm::vec3 &p) {
    // Torso and neck
    float d = capsule(p, {0, legTop, 0}, {0, shoulder, 0}, torsoR);
    d = smoothUnion(d, capsule(p, {0, shoulder * .96f, 0}, {0, headY - .08f, 0}, .065f * build), .03f);

    // Anatomical Head base + Jawline + Nose + Ears
    float headBase = sphere(p, {0, headY, 0}, .135f * build);
    float nose = capsule(p, {0, headY - .015f, .125f * build}, {0, headY - .038f, .158f * build}, .018f * build);
    float chin = capsule(p, {-.045f * build, headY - .095f, .04f}, {.045f * build, headY - .095f, .04f}, .038f * build);
    float earL = sphere(p, {-.132f * build, headY - .01f, 0}, .026f * build);
    float earR = sphere(p, { .132f * build, headY - .01f, 0}, .026f * build);
    float brow = capsule(p, {-.055f * build, headY + .022f, .105f}, {.055f * build, headY + .022f, .105f}, .016f * build);

    float headFull = smoothUnion(headBase, nose, .02f);
    headFull = smoothUnion(headFull, chin, .03f);
    headFull = std::min(headFull, std::min(earL, earR));
    headFull = smoothUnion(headFull, brow, .015f);

    d = smoothUnion(d, headFull, .04f);

    // Shoulders (Deltoids) & Chest Pectoral definition
    float shoulderL = sphere(p, {-torsoR * 1.15f, shoulder * .98f, 0}, limbR * 1.25f);
    float shoulderR = sphere(p, { torsoR * 1.15f, shoulder * .98f, 0}, limbR * 1.25f);
    d = smoothUnion(d, std::min(shoulderL, shoulderR), .035f);

    // Legs
    d = smoothUnion(d, capsule(p, {-torsoR*.62f, legTop, 0}, {-torsoR*.72f, .05f, 0}, limbR), .035f);
    d = smoothUnion(d, capsule(p, { torsoR*.62f, legTop, 0}, { torsoR*.72f, .05f, 0}, limbR), .035f);
    // Arms
    d = smoothUnion(d, capsule(p, {-torsoR*1.15f, shoulder*.98f, 0}, {-torsoR*2.25f, legTop*.98f, 0}, limbR*.78f), .03f);
    d = smoothUnion(d, capsule(p, { torsoR*1.15f, shoulder*.98f, 0}, { torsoR*2.25f, legTop*.98f, 0}, limbR*.78f), .03f);
    return d;
  };

  Field jacket = [=](const glm::vec3 &p) {
    float d = capsule(p, {0, legTop*.98f, 0}, {0, shoulder, 0}, torsoR + .028f);
    // V-neck open chest cut for light shirt underneath
    d = smoothSubtract(d, sphere(p, {0, shoulder * .92f, torsoR * 0.85f}, torsoR * 0.55f), .03f);

    // Collar / Lapel folds around neck
    float lapelL = capsule(p, {-.08f * build, shoulder * .95f, torsoR * .7f}, {-.03f * build, shoulder * .85f, torsoR * .85f}, .025f);
    float lapelR = capsule(p, { .08f * build, shoulder * .95f, torsoR * .7f}, { .03f * build, shoulder * .85f, torsoR * .85f}, .025f);
    d = smoothUnion(d, std::min(lapelL, lapelR), .02f);

    // Sleeves down arms with structured shoulder pads
    d = smoothUnion(d, capsule(p, {-torsoR*1.12f, shoulder*.98f, 0}, {-torsoR*2.15f, legTop*.99f, 0}, limbR*.80f + .023f), .025f);
    d = smoothUnion(d, capsule(p, { torsoR*1.12f, shoulder*.98f, 0}, { torsoR*2.15f, legTop*.99f, 0}, limbR*.80f + .023f), .025f);
    return d;
  };

  Field shirt = [=](const glm::vec3 &p) {
    float d = capsule(p, {0, legTop * 0.96f, 0}, {0, shoulder * 0.98f, 0}, torsoR + .018f);
    // Shirt collar ridge around lower neck
    d = smoothUnion(d, capsule(p, {0, shoulder * .92f, 0}, {0, shoulder * .97f, 0}, torsoR + .022f), .015f);
    return d;
  };

  Field trousers = [=](const glm::vec3 &p) {
    float d = capsule(p, {-torsoR*.62f, legTop*.98f, 0}, {-torsoR*.72f, .10f, 0}, limbR + .025f);
    d = smoothUnion(d, capsule(p, { torsoR*.62f, legTop*.98f, 0}, { torsoR*.72f, .10f, 0}, limbR + .025f), .025f);
    return d;
  };

  Field belt = [=](const glm::vec3 &p) {
    float ring = capsule(p, {0, legTop * 1.02f, 0}, {0, legTop * 1.07f, 0}, torsoR + .036f);
    // Front metallic buckle plate
    float buckle = capsule(p, {-.04f * build, legTop * 1.045f, torsoR + .042f}, {.04f * build, legTop * 1.045f, torsoR + .042f}, .025f * build);
    // Utility pouches on sides and back
    float pouchL1 = sphere(p, {-torsoR * 0.92f, legTop * 1.04f, 0.02f}, 0.048f * build);
    float pouchR1 = sphere(p, { torsoR * 0.92f, legTop * 1.04f, 0.02f}, 0.048f * build);
    float pouchL2 = sphere(p, {-torsoR * 0.75f, legTop * 1.04f, -0.08f}, 0.042f * build);
    float pouchR2 = sphere(p, { torsoR * 0.75f, legTop * 1.04f, -0.08f}, 0.042f * build);
    return std::min(ring, std::min(buckle, std::min(std::min(pouchL1, pouchR1), std::min(pouchL2, pouchR2))));
  };

  Field garment = [=](const glm::vec3 &p) {
    if (isExplorer) {
      float d = std::min(jacket(p), trousers(p));
      d = std::min(d, shirt(p));
      if (s.belt) d = std::min(d, belt(p));
      return d;
    }
    float d = capsule(p, {0, legTop*.98f, 0}, {0, shoulder, 0}, torsoR + .025f);
    if (s.outfit == "dress" || s.outfit == "skirt")
      d = smoothUnion(d, capsule(p, {0, legTop*.95f, 0}, {0, legTop*.55f, 0}, torsoR * 1.35f), .05f);
    else if (s.outfit == "trousers" || s.outfit == "armor") {
      d = smoothUnion(d, capsule(p, {-torsoR*.62f, legTop*.98f, 0}, {-torsoR*.72f, .10f, 0}, limbR + .025f), .025f);
      d = smoothUnion(d, capsule(p, { torsoR*.62f, legTop*.98f, 0}, { torsoR*.72f, .10f, 0}, limbR + .025f), .025f);
    }
    d = smoothUnion(d, capsule(p, {-torsoR*1.12f, shoulder*.98f, 0}, {-torsoR*2.15f, legTop*.99f, 0}, limbR*.80f + .023f), .025f);
    d = smoothUnion(d, capsule(p, { torsoR*1.12f, shoulder*.98f, 0}, { torsoR*2.15f, legTop*.99f, 0}, limbR*.80f + .023f), .025f);
    if (s.belt) d = std::min(d, belt(p));
    return d;
  };

  Field hair = [=](const glm::vec3 &p) {
    // Short cropped hair mass on top, back and sides of head
    float hairTop = sphere(p, {0, headY + .04f, 0}, .138f * build);
    float hairBack = sphere(p, {0, headY + .02f, -.03f}, .135f * build);
    return std::min(hairTop, hairBack);
  };

  Field boot = [=](const glm::vec3 &p) {
    // Leather boot sole + ankle collar + toe extension
    float bootL = sphere(p, {-torsoR*.72f, .055f, .045f}, limbR * 1.25f);
    float bootR = sphere(p, { torsoR*.72f, .055f, .045f}, limbR * 1.25f);
    float toeL = capsule(p, {-torsoR*.72f, .035f, .03f}, {-torsoR*.72f, .035f, .095f}, limbR * .95f);
    float toeR = capsule(p, { torsoR*.72f, .035f, .03f}, { torsoR*.72f, .035f, .095f}, limbR * .95f);
    return std::min(std::min(bootL, bootR), std::min(toeL, toeR));
  };

  Field total = [=](const glm::vec3 &p) {
    float d = std::min(skin(p), garment(p));
    if (s.hair) d = std::min(d, hair(p));
    if (s.boots) d = std::min(d, boot(p));
    return d;
  };

  // Support detail 0 through 5 (4 = ~95k tris, 5 = ~150k tris)
  int gx = 20, gy = 34, gz = 16;
  if (s.detail == 1) { gx = 25; gy = 40; gz = 20; }
  else if (s.detail == 2) { gx = 30; gy = 46; gz = 24; }
  else if (s.detail == 3) { gx = 45; gy = 70; gz = 36; }
  else if (s.detail == 4) { gx = 65; gy = 100; gz = 52; }
  else if (s.detail >= 5) { gx = 80; gy = 125; gz = 64; }

  const glm::vec3 mn(-.52f * build, 0.0f, -.34f * build);
  const glm::vec3 mx( .52f * build, h * 1.04f, .34f * build);
  const glm::vec3 step = (mx - mn) / glm::vec3(gx, gy, gz);

  const int numSurfaces = isExplorer ? 7 : 4;
  std::vector<Surface> surfaces(numSurfaces);

  const std::array<std::array<int, 4>, 6> tetra{{{{0,5,1,6}},{{0,1,2,6}},{{0,2,3,6}},{{0,3,7,6}},{{0,7,4,6}},{{0,4,5,6}}}};

  auto regionAt = [&](const glm::vec3 &p) {
    const float sd = skin(p);
    const float hd = s.hair ? hair(p) : 1e9f;
    const float bd = s.boots ? boot(p) : 1e9f;

    if (isExplorer) {
      const float jd = jacket(p);
      const float shd = shirt(p);
      const float td = trousers(p);
      const float bld = s.belt ? belt(p) : 1e9f;

      float minDist = std::min({sd, hd, bd, jd, shd, td, bld});
      if (minDist == hd) return 6;
      if (minDist == bd) return 5;
      if (minDist == bld) return 4;
      if (minDist == td) return 3;
      if (minDist == shd) return 2;
      if (minDist == jd) return 1;
      return 0;
    }

    const float gd = garment(p);
    if (s.hair && hd <= std::min(gd, sd)) return 2;
    if (s.boots && bd <= std::min(gd, sd)) return 3;
    return gd <= sd ? 1 : 0;
  };

  auto emit = [&](const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c) {
    const glm::vec3 centre = (a + b + c) / 3.0f; const int region = regionAt(centre);
    glm::vec3 aa=a, bb=b, cc=c; if (glm::dot(glm::cross(bb-aa, cc-aa), gradient(total, centre)) < 0.0f) std::swap(bb, cc);
    Surface &out = surfaces[region]; const uint32_t ia=out.vertex(aa,gradient(total,aa),region), ib=out.vertex(bb,gradient(total,bb),region), ic=out.vertex(cc,gradient(total,cc),region);
    out.indices.insert(out.indices.end(), {ia,ib,ic});
  };

  for (int z=0; z<gz; ++z) for (int y=0; y<gy; ++y) for (int x=0; x<gx; ++x) {
    constexpr std::array<glm::ivec3, 8> corners{{
        {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0},
        {0,0,1}, {1,0,1}, {1,1,1}, {0,1,1}}};
    std::array<glm::vec3,8> p; std::array<float,8> d;
    for(int i=0;i<8;++i) { p[i]=mn+step*glm::vec3(x+corners[i].x, y+corners[i].y, z+corners[i].z); d[i]=total(p[i]); }
    for(const auto&t:tetra) {
      std::array<glm::vec3,4> tp{{p[t[0]],p[t[1]],p[t[2]],p[t[3]]}};
      std::array<float,4> td{{d[t[0]],d[t[1]],d[t[2]],d[t[3]]}};
      std::vector<int> in, out;
      for (int i=0;i<4;++i) (td[i] < kIso ? in : out).push_back(i);
      auto cut = [&](int a, int b) { const float f=(kIso-td[a])/(td[b]-td[a]); return glm::mix(tp[a],tp[b],f); };
      if (in.size() == 1) {
        emit(cut(in[0], out[0]), cut(in[0], out[1]), cut(in[0], out[2]));
      } else if (in.size() == 3) {
        emit(cut(out[0], in[0]), cut(out[0], in[2]), cut(out[0], in[1]));
      } else if (in.size() == 2) {
        const glm::vec3 ac=cut(in[0],out[0]), ad=cut(in[0],out[1]);
        const glm::vec3 bc=cut(in[1],out[0]), bd=cut(in[1],out[1]);
        emit(ac, bc, bd); emit(ac, bd, ad);
      }
    }
  }

  auto data=std::make_unique<MeshData>(); data->sourcePath="gen://character.v1";

  if (isExplorer) {
    const std::array<const char*, 7> names{{"Skin", "Jacket", "Shirt", "Trousers", "Belt", "Boots", "Hair"}};
    const std::array<glm::vec3, 7> colors{{s.skin, s.jacketColor, s.shirtColor, s.trousersColor, s.beltColor, s.bootsColor, s.hairColor}};
    const std::array<float, 7> roughness{{0.55f, s.garmentRoughness, 0.70f, 0.75f, 0.48f, 0.45f, 0.80f}};
    for (int i = 0; i < 7; ++i) if (!surfaces[i].indices.empty()) {
      MeshSubmeshData sm; sm.objectName=names[i]; sm.material.id=names[i]; sm.material.baseColor=glm::vec4(colors[i],1); sm.material.roughness=roughness[i]; sm.vertices=std::move(surfaces[i].vertices); sm.indices=std::move(surfaces[i].indices); sm.aabbMin=surfaces[i].mn;sm.aabbMax=surfaces[i].mx;sm.hasBounds=true; data->objectBounds.emplace_back(sm.objectName,MeshObjectBounds{sm.aabbMin,sm.aabbMax,true});data->submeshes.push_back(std::move(sm));
    }
  } else {
    const std::array<const char*, 4> names{{"Skin","Garment","Hair","Boots"}};
    const std::array<glm::vec3, 4> colors{{s.skin,s.garment,s.hairColor,s.bootsColor}};
    for (int i = 0; i < 4; ++i) if (!surfaces[i].indices.empty()) {
      MeshSubmeshData sm; sm.objectName=names[i]; sm.material.id=names[i]; sm.material.baseColor=glm::vec4(colors[i],1); sm.material.roughness=(i==1?s.garmentRoughness:(i==0?.58f:.72f)); sm.vertices=std::move(surfaces[i].vertices); sm.indices=std::move(surfaces[i].indices); sm.aabbMin=surfaces[i].mn;sm.aabbMax=surfaces[i].mx;sm.hasBounds=true; data->objectBounds.emplace_back(sm.objectName,MeshObjectBounds{sm.aabbMin,sm.aabbMax,true});data->submeshes.push_back(std::move(sm));
    }
  }

  if(data->submeshes.empty()) { error="character field produced no surface"; return nullptr; }
  data->recenter(MeshData::Recenter::BaseY); return data;
}

} // namespace gen::modeling
