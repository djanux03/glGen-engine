#pragma once

#include <glm/glm.hpp>
#include <json.hpp>
#include <algorithm>
#include <cmath>
#include <type_traits>

// Backend-independent transport and authoring contract. Distances are metres,
// extinction is m^-1, and albedo is a dimensionless fraction of extinction.
namespace atmosphere {
enum class Quality : int { Balanced = 0, High = 1 };
struct Medium {
  double extinction = 0;
  glm::dvec3 source = glm::dvec3(0); // scattering coefficient * incident radiance
};
struct Transport {
  double transmittance = 1;
  glm::dvec3 scattering = glm::dvec3(0);
};
inline double segmentWeight(double extinction, double distance) {
  const double tau = std::max(0.0, extinction) * std::max(0.0, distance);
  // Dividing by sigma fails at vacuum; exp(-tau) also loses tiny differences.
  return tau < 1e-5 ? std::max(0.0, distance) * (1 - tau / 2 + tau * tau / 6)
                   : -std::expm1(-tau) / extinction;
}
inline Transport segment(const Medium& medium, double distance) {
  return {std::exp(-std::max(0.0, medium.extinction) * std::max(0.0, distance)),
          medium.source * segmentWeight(medium.extinction, distance)};
}
inline Transport append(const Transport& a, const Transport& b) {
  return {a.transmittance * b.transmittance,
          a.scattering + a.transmittance * b.scattering};
}
// cosTheta = dot(camera-to-sample direction, sample-to-emitter direction).
// Looking toward the emitter therefore gives the forward peak for g > 0.
inline double hg(double cosTheta, double g) {
  g = std::clamp(g, -0.95, 0.95);
  const double d = std::max(1e-9, 1 + g*g - 2*g*std::clamp(cosTheta, -1.0, 1.0));
  return (1-g*g) / (12.566370614359172 * d * std::sqrt(d));
}
inline double heightIntegral(double y0, double dy, double distance,
                             double density, double falloff, double reference) {
  distance = std::max(0.0, distance);
  density = std::max(0.0, density);
  falloff = std::max(0.0, falloff);
  if (falloff == 0 || distance == 0) return density * distance;
  y0 -= reference;
  auto above = [&](double y, double length) {
    const double x = falloff * std::abs(dy) * length;
    const double integral = x < 1e-5
        ? length * (1 - x/2 + x*x/6) : -std::expm1(-x)/(falloff*std::abs(dy));
    const double weight = integral;
    // Integrate from the denser endpoint: a steep descending segment must
    // never multiply underflowing exp(-height) by overflowing exp(+length).
    return std::exp(-falloff * std::max(std::min(y,y+dy*length), 0.0)) * weight;
  };
  const double y1 = y0 + dy * distance;
  if (y0 <= 0 && y1 <= 0) return density * distance;
  if (y0 >= 0 && y1 >= 0) return density * above(y0, distance);
  const double crossing = std::clamp(-y0/dy, 0.0, distance);
  return density * (y0 < 0 ? crossing + above(0, distance-crossing)
                          : above(y0, crossing) + distance-crossing);
}
inline double sliceBoundary(int index, int slices, double range) {
  if (index <= 0) return 0; // include the camera-to-near-plane segment
  return .05 * std::pow(std::max(range, .1)/.05, double(index-1)/double(slices-1));
}
struct Settings {
  static constexpr int schemaVersion = 2;
  bool enabled = true;
  bool physicalSky = true;
  bool cloudShadows = true;
  bool cloudHistory = true;
  bool histogramExposure = true;
  float exposureKey = .18f;
  float exposureMin = .01f;
  float exposureMax = 64;
  Quality quality = Quality::Balanced;
  float range = 180;
  float historyWeight = .9f;
  bool history = true;
  float groundExtinction = .006f;
  float groundFalloff = .045f;
  float groundReference = -2;
  float start = 0;
  glm::vec3 groundAlbedo = glm::vec3(.9f);
  float groundAnisotropy = .55f;
  float groundTintStrength = 0;
  float dustExtinction = .00175f;
  glm::vec3 dustAlbedo = glm::vec3(.8f);
  float dustAnisotropy = .87f;
  bool directionalShadows = true;
  bool skyVisibility = true;
  bool terrainMist = true;
  bool valleyPooling = true;
  float valleyExtinction = .001f;
  float valleyDepth = 8;
  float terrainExtinction = .002f;
  float terrainFalloff = .4f;
  float waterBoost = 1;
  float bloomStrength = .04f;
  bool bloomFireflySuppression = true;
  // Regression-only homogeneous medium. Negative extinction selects the
  // normal authored medium; this is never enabled by scenery presets.
  float referenceExtinction = -1;
  float referenceSceneRadiance = -1;
  glm::vec3 referenceRadiance = glm::vec3(.7f);
};
inline void sanitize(Settings& s) {
  auto finite = [](float v, float fallback, float lo, float hi) {
    return std::isfinite(v) ? std::clamp(v,lo,hi) : fallback;
  };
  s.range = finite(s.range,180,1,2000);
  s.historyWeight = finite(s.historyWeight,.9f,0,.9f);
  s.exposureKey=finite(s.exposureKey,.18f,.001f,1);
  s.exposureMin=finite(s.exposureMin,.01f,1e-6f,10000);
  s.exposureMax=finite(s.exposureMax,64,s.exposureMin,10000);
  s.groundExtinction = finite(s.groundExtinction,0,0,10);
  s.dustExtinction = finite(s.dustExtinction,0,0,10);
  s.groundFalloff = finite(s.groundFalloff,.045f,0,10);
  s.groundReference = finite(s.groundReference,-2,-100000,100000);
  s.start = finite(s.start,0,0,2000);
  s.groundAnisotropy = finite(s.groundAnisotropy,.55f,-.95f,.95f);
  s.dustAnisotropy = finite(s.dustAnisotropy,.87f,-.95f,.95f);
  s.terrainExtinction = finite(s.terrainExtinction,.002f,0,10);
  s.valleyExtinction = finite(s.valleyExtinction,.001f,0,10);
  s.valleyDepth = finite(s.valleyDepth,8,0,64);
  s.terrainFalloff = finite(s.terrainFalloff,.4f,0,10);
  s.waterBoost = finite(s.waterBoost,1,0,20);
  s.bloomStrength = finite(s.bloomStrength,.04f,0,1);
  s.quality = s.quality == Quality::High ? Quality::High : Quality::Balanced;
  for(int c=0;c<3;++c) {
    s.groundAlbedo[c]=finite(s.groundAlbedo[c],.9f,0,1);
    s.dustAlbedo[c]=finite(s.dustAlbedo[c],.8f,0,1);
  }
}
inline nlohmann::json encode(const Settings& s) {
  return {{"version",Settings::schemaVersion},{"enabled",s.enabled},{"physicalSky",s.physicalSky},{"cloudShadows",s.cloudShadows},{"cloudHistory",s.cloudHistory},{"quality",int(s.quality)},
    {"range",s.range},{"history",s.history},{"historyWeight",s.historyWeight},
    {"valleyPooling",s.valleyPooling},{"valleyExtinction",s.valleyExtinction},{"valleyDepth",s.valleyDepth},
    {"histogramExposure",s.histogramExposure},{"exposureKey",s.exposureKey},{"exposureMin",s.exposureMin},{"exposureMax",s.exposureMax},
    {"groundExtinction",s.groundExtinction},{"groundFalloff",s.groundFalloff},
    {"groundReference",s.groundReference},{"start",s.start},
    {"groundAlbedo",{s.groundAlbedo.x,s.groundAlbedo.y,s.groundAlbedo.z}},
    {"groundAnisotropy",s.groundAnisotropy},{"groundTintStrength",s.groundTintStrength},{"dustExtinction",s.dustExtinction},
    {"dustAlbedo",{s.dustAlbedo.x,s.dustAlbedo.y,s.dustAlbedo.z}},
    {"dustAnisotropy",s.dustAnisotropy},{"directionalShadows",s.directionalShadows},
    {"skyVisibility",s.skyVisibility},{"terrainMist",s.terrainMist},
    {"terrainExtinction",s.terrainExtinction},{"terrainFalloff",s.terrainFalloff},
    {"waterBoost",s.waterBoost},{"bloomStrength",s.bloomStrength},
    {"bloomFireflySuppression",s.bloomFireflySuppression},{"referenceExtinction",s.referenceExtinction},
    {"referenceSceneRadiance",s.referenceSceneRadiance},
    {"referenceRadiance",{s.referenceRadiance.x,s.referenceRadiance.y,s.referenceRadiance.z}}};
}
inline void decode(const nlohmann::json& j, Settings& s) {
  if(!j.is_object()) return;
  auto scalar = [&](const char* key, auto& value) {
    auto it=j.find(key); if(it==j.end()) return;
    using T=std::decay_t<decltype(value)>;
    if constexpr(std::is_same_v<T,bool>) { if(it->is_boolean()) value=it->get<bool>(); }
    else if(it->is_number()) value=it->get<T>();
  };
  auto color=[&](const char* key,glm::vec3& value) {
    auto it=j.find(key); if(it==j.end()||!it->is_array()||it->size()!=3) return;
    for(int c=0;c<3;++c) if((*it)[c].is_number()) value[c]=(*it)[c].get<float>();
  };
  int quality=int(s.quality); scalar("quality",quality); s.quality=Quality(quality);
  if(j.contains("quality") && j["quality"].is_string())
    s.quality=j["quality"]=="high" ? Quality::High : Quality::Balanced;
  scalar("enabled",s.enabled); scalar("physicalSky",s.physicalSky); scalar("range",s.range); scalar("history",s.history);
  scalar("cloudShadows",s.cloudShadows);
  scalar("cloudHistory",s.cloudHistory);
  scalar("valleyPooling",s.valleyPooling);scalar("valleyExtinction",s.valleyExtinction);scalar("valleyDepth",s.valleyDepth);
  scalar("histogramExposure",s.histogramExposure);scalar("exposureKey",s.exposureKey);scalar("exposureMin",s.exposureMin);scalar("exposureMax",s.exposureMax);
  scalar("historyWeight",s.historyWeight); scalar("groundExtinction",s.groundExtinction);
  scalar("groundFalloff",s.groundFalloff); scalar("groundReference",s.groundReference);
  scalar("start",s.start); color("groundAlbedo",s.groundAlbedo);
  scalar("groundAnisotropy",s.groundAnisotropy); scalar("dustExtinction",s.dustExtinction);
  scalar("groundTintStrength",s.groundTintStrength);
  s.groundTintStrength=std::isfinite(s.groundTintStrength)?std::clamp(s.groundTintStrength,0.f,1.f):0;
  color("dustAlbedo",s.dustAlbedo); scalar("dustAnisotropy",s.dustAnisotropy);
  scalar("directionalShadows",s.directionalShadows); scalar("skyVisibility",s.skyVisibility);
  scalar("terrainMist",s.terrainMist); scalar("terrainExtinction",s.terrainExtinction);
  scalar("terrainFalloff",s.terrainFalloff); scalar("waterBoost",s.waterBoost);
  scalar("bloomStrength",s.bloomStrength); scalar("bloomFireflySuppression",s.bloomFireflySuppression);
  scalar("referenceSceneRadiance",s.referenceSceneRadiance);
  scalar("referenceExtinction",s.referenceExtinction); color("referenceRadiance",s.referenceRadiance);
  if(!std::isfinite(s.referenceExtinction)) s.referenceExtinction=-1;
  s.referenceExtinction=std::clamp(s.referenceExtinction,-1.f,10.f);
  sanitize(s);
}
// Legacy fields stay as API/UI aliases while shipped scenes migrate in memory.
// Call after aliases have been parsed, then decode nested values (which win).
template<class P> Settings fromLegacy(const P& p) {
  Settings s=p.atmosphere;
  s.groundExtinction=p.fogDensity; s.groundFalloff=p.fogHeightFalloff;
  s.groundReference=p.fogHeightRef; s.start=p.fogStart;
  s.groundAnisotropy=p.fogAnisotropy;
  s.dustExtinction=p.volumetricEnabled ? .001f*p.volumetricDensityScale : 0;
  s.dustAnisotropy=p.volumetricAnisotropy;
  s.dustAlbedo=glm::mix(glm::vec3(1),p.volumetricTintColor,p.volumetricTintStrength)
                    *p.volumetricIntensity;
  sanitize(s); return s;
}
template<class P> void publishAliases(P& p) {
  const auto& s=p.atmosphere;
  p.fogDensity=s.groundExtinction; p.fogHeightFalloff=s.groundFalloff;
  p.fogHeightRef=s.groundReference; p.fogStart=s.start; p.fogAnisotropy=s.groundAnisotropy;
  p.volumetricEnabled=s.dustExtinction>0; p.volumetricDensityScale=s.dustExtinction*1000;
  p.volumetricAnisotropy=s.dustAnisotropy;
  p.volumetricIntensity=1; p.volumetricTintColor=s.dustAlbedo; p.volumetricTintStrength=1;
  p.bloomIntensity=s.bloomStrength;
}
template<class P> void read(const nlohmann::json& request,P& p) {
  p.atmosphere=fromLegacy(p);
  if(request.contains("bloomIntensity") && request["bloomIntensity"].is_number()) p.atmosphere.bloomStrength = std::clamp(p.bloomIntensity,0.f,1.f);
  if(request.contains("atmosphere")) { decode(request["atmosphere"],p.atmosphere); publishAliases(p); }
}
// Renderer-owned lights use one codec until the ECS light phase. Keeping the
// volumetric switches here prevents scenery/editor saves from dropping them.
template<class P> nlohmann::json encodeLights(const P& p) {
  auto lights=nlohmann::json::array();
  for(size_t i=0;i<std::min<size_t>(p.pointLightCount,p.pointLights.size());++i) {
    const auto& l=p.pointLights[i];
    lights.push_back({{"position",{l.position.x,l.position.y,l.position.z}},
      {"color",{l.color.x,l.color.y,l.color.z}},{"radius",l.radius},
      {"intensity",l.intensity},{"volumetricParticipation",l.volumetricParticipation},
      {"volumetricShadows",l.volumetricShadows}});
  }
  return lights;
}
template<class P> void readLights(const nlohmann::json& request,P& p) {
  auto entries=request.find("pointLights");
  if(entries==request.end() || !entries->is_array()) return;
  p.pointLightCount=0;
  for(const auto& entry:*entries) {
    if(!entry.is_object() || p.pointLightCount>=p.pointLights.size()) continue;
    auto& light=p.pointLights[p.pointLightCount++];
    light=std::decay_t<decltype(light)>{};
    auto number=[&](const char* key,float& v,float lo) {
      auto it=entry.find(key);
      if(it!=entry.end() && it->is_number()) {
        const float input=it->template get<float>();
        if(std::isfinite(input)) v=std::max(lo,input);
      }
    };
    auto vector=[&](const char* key,glm::vec3& v,bool nonnegative) {
      auto it=entry.find(key);
      if(it==entry.end() || !it->is_array() || it->size()!=3) return;
      for(int c=0;c<3;++c) if((*it)[c].is_number()) {
        const float input=(*it)[c].template get<float>();
        if(std::isfinite(input)) v[c]=nonnegative?std::max(0.f,input):input;
      }
    };
    vector("position",light.position,false); vector("color",light.color,true);
    number("radius",light.radius,.1f); number("intensity",light.intensity,0);
    number("volumetricParticipation",light.volumetricParticipation,0);
    light.volumetricParticipation=std::min(light.volumetricParticipation,1.f);
    auto shadows=entry.find("volumetricShadows");
    if(shadows!=entry.end() && shadows->is_boolean()) light.volumetricShadows=shadows->template get<bool>();
  }
}
} // namespace atmosphere
