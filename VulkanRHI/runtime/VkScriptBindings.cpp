#include "VkScriptBindings.h"

#include "VkAppState.h"
#include "VulkanRenderer.h"
#include "subsystems/VkTerrainSubsystem.h"

#include "Logger.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include "Scripting/ScriptJson.h"

#include <algorithm>
#include <string>

namespace {

// Reads a {r,g,b} Lua table into `dst`, leaving it alone if absent. Colours
// are 1-based triples everywhere in this API, matching the recipe JSON.
void readColor(const sol::table &t, const char *key, glm::vec3 &dst) {
  sol::optional<sol::table> c = t[key];
  if (c && c->size() >= 3)
    dst = glm::vec3((*c)[1].get_or(dst.x), (*c)[2].get_or(dst.y),
                    (*c)[3].get_or(dst.z));
}

// --- JSON param mapping (shared by the Lua binding and the command port) ---

template <typename T>
void readField(const nlohmann::json &j, const char *key, T &dst) {
  const auto it = j.find(key);
  if (it != j.end() && !it->is_null()) {
    try {
      dst = it->get<T>();
    } catch (const std::exception &) {
      // Wrong type for this field: leave it alone rather than abort the whole
      // update. The caller still gets every other field applied.
    }
  }
}

void readVec3Field(const nlohmann::json &j, const char *key, glm::vec3 &dst) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_array() || it->size() < 3)
    return;
  for (int i = 0; i < 3; ++i)
    if (!(*it)[i].is_number())
      return;
  dst = glm::vec3((*it)[0].get<float>(), (*it)[1].get<float>(),
                  (*it)[2].get<float>());
}

} // namespace

void applyRenderParamsJson(VkAppState &state, const nlohmann::json &params) {
  if (!state.renderer || !params.is_object())
    return;
  vkrhi::VulkanRenderer::Params &p = state.renderer->params();

  readField(params, "exposure", p.exposure);
  readField(params, "gamma", p.gamma);
  readField(params, "saturation", p.saturation);
  readField(params, "contrast", p.contrast);
  readField(params, "vignette", p.vignette);
  readField(params, "autoExposure", p.autoExposure);
  readField(params, "tonemapMode", p.tonemapMode);

  readField(params, "fov", p.fovDeg);
  readField(params, "farPlane", p.farPlane);

  readField(params, "sunYaw", p.lightYawDeg);
  readField(params, "sunPitch", p.lightPitchDeg);
  readField(params, "sunIntensity", p.sunIntensity);
  readField(params, "ambientIntensity", p.ambientIntensity);
  readField(params, "timeOfDay", p.timeOfDayEnabled);
  readField(params, "timeOfDaySpeed", p.timeOfDaySpeed);

  readField(params, "shadowStrength", p.shadowStrength);
  readField(params, "shadowSoftness", p.shadowSoftness);

  readField(params, "fogDensity", p.fogDensity);
  readField(params, "fogStart", p.fogStart);
  readField(params, "fogMaxOpacity", p.fogMaxOpacity);

  readField(params, "volumetric", p.volumetricEnabled);
  readField(params, "volumetricIntensity", p.volumetricIntensity);
  readField(params, "bloomIntensity", p.bloomIntensity);
  readField(params, "aoStrength", p.aoStrength);
  readField(params, "debugView", p.debugViewMode);

  readVec3Field(params, "fogDayColor", p.fogDayColor);
  readVec3Field(params, "fogNightColor", p.fogNightColor);
  readVec3Field(params, "camPos", p.camPos);
  readField(params, "camYaw", p.camYawDeg);
  readField(params, "camPitch", p.camPitchDeg);
}

nlohmann::json renderParamsToJson(const VkAppState &state) {
  nlohmann::json j = nlohmann::json::object();
  if (!state.renderer)
    return j;
  const vkrhi::VulkanRenderer::Params &p = state.renderer->params();
  j["exposure"] = p.exposure;
  j["autoExposure"] = p.autoExposure;
  j["fov"] = p.fovDeg;
  j["farPlane"] = p.farPlane;
  j["sunYaw"] = p.lightYawDeg;
  j["sunPitch"] = p.lightPitchDeg;
  j["sunIntensity"] = p.sunIntensity;
  j["ambientIntensity"] = p.ambientIntensity;
  j["fogDensity"] = p.fogDensity;
  j["fogStart"] = p.fogStart;
  j["shadowStrength"] = p.shadowStrength;
  j["bloomIntensity"] = p.bloomIntensity;
  j["debugView"] = p.debugViewMode;
  j["camPos"] = nlohmann::json::array({p.camPos.x, p.camPos.y, p.camPos.z});
  j["camYaw"] = p.camYawDeg;
  j["camPitch"] = p.camPitchDeg;
  return j;
}

void registerVkScriptBindings(sol::state &lua, VkAppState &state) {
  VkAppState *st = &state;

  // ── render ─────────────────────────────────────────────────────────
  auto renderTable = lua.create_named_table("render");

  // render.params{ exposure=, sunPitch=, ... } -- every field optional, and
  // anything omitted keeps its current value. Deliberately a keyword table
  // rather than positional arguments: this set grows, and a script written
  // today should not break when it does.
  // Converts the Lua table to JSON and applies it through the shared mapping,
  // so this binding and the command port's render.setParams cannot drift.
  // sol::object rather than sol::table: it binds a table argument just as
  // well and converts straight through ScriptJson without a cast.
  renderTable["params"] = [st](sol::object opts) {
    applyRenderParamsJson(*st, scriptjson::toJson(opts));
  };

  renderTable["get_params"] = [st, &lua]() -> sol::object {
    return scriptjson::toLua(lua, renderParamsToJson(*st));
  };

  // render.capture(path) -- the PNG is written by the NEXT drawFrame, so it
  // is not on disk when this returns. Anything waiting on the file has to
  // let at least one frame pass; the eventual command port turns this into a
  // deferred reply for exactly that reason.
  renderTable["capture"] = [st](const std::string &path) -> bool {
    if (!st->renderer || path.empty())
      return false;
    st->renderer->requestCapture(path);
    return true;
  };

  renderTable["stats"] = [st, &lua]() -> sol::table {
    sol::table t = lua.create_table();
    if (!st->renderer)
      return t;
    const vkrhi::VulkanRenderer::FrameStats &s = st->renderer->frameStats();
    t["instancesDrawn"] = s.instancesDrawn;
    t["instancesCulled"] = s.instancesCulled;
    t["vegInstancesDrawn"] = s.vegInstancesDrawn;
    t["tlasInstances"] = s.tlasInstances;
    t["meshSlotsLive"] = s.meshSlotsLive;
    t["meshSlotsFree"] = s.meshSlotsFree;
    return t;
  };

  // ── terrain ────────────────────────────────────────────────────────
  auto terrainTable = lua.create_named_table("terrain");

  terrainTable["height_at"] = [st](float x, float z) -> float {
    return st->terrainSubsystem ? st->terrainSubsystem->heightAt(glm::vec2(x, z))
                                : 0.0f;
  };

  terrainTable["regenerate"] = [st](sol::optional<sol::table> optsOpt) -> bool {
    if (!st->terrainSubsystem)
      return false;
    TerrainSettings s = st->terrainSubsystem->settings();
    if (optsOpt) {
      const sol::table opts = *optsOpt;
      s.seed = opts.get_or("seed", s.seed);
      s.heightScale = opts.get_or("heightScale", s.heightScale);
      s.noiseFrequency = opts.get_or("noiseFrequency", s.noiseFrequency);
      s.octaves = opts.get_or("octaves", s.octaves);
      s.viewDistanceChunks =
          opts.get_or("viewDistanceChunks", s.viewDistanceChunks);
      s.spawnVegetation = opts.get_or("vegetation", s.spawnVegetation);
      s.spawnGrass = opts.get_or("grass", s.spawnGrass);
      s.treeDensityMultiplier =
          opts.get_or("treeDensity", s.treeDensityMultiplier);
      s.grassDensityMultiplier =
          opts.get_or("grassDensity", s.grassDensityMultiplier);
      s.rockDensityMultiplier =
          opts.get_or("rockDensity", s.rockDensityMultiplier);
    }
    // Safe from script context: the frame loop is strictly sequential, and
    // scripts run before the terrain's own per-frame work (see
    // VkTerrainSubsystem's header note on regenerate()).
    st->terrainSubsystem->regenerate(s);
    return true;
  };

  terrainTable["settings"] = [st, &lua]() -> sol::table {
    sol::table t = lua.create_table();
    if (!st->terrainSubsystem)
      return t;
    const TerrainSettings &s = st->terrainSubsystem->settings();
    t["seed"] = s.seed;
    t["heightScale"] = s.heightScale;
    t["noiseFrequency"] = s.noiseFrequency;
    t["octaves"] = s.octaves;
    t["viewDistanceChunks"] = s.viewDistanceChunks;
    t["vegetation"] = s.spawnVegetation;
    t["grass"] = s.spawnGrass;
    t["treeDensity"] = s.treeDensityMultiplier;
    t["grassDensity"] = s.grassDensityMultiplier;
    t["rockDensity"] = s.rockDensityMultiplier;
    return t;
  };

  // terrain.scatter_layer{ name=, mesh=, density=, ... }
  // Adds a layer, or replaces the existing one with the same name. `mesh`
  // takes any asset id the renderer can resolve, including a "gen://" id from
  // assets.define -- which is the whole point: an AI can generate a tree and
  // scatter it across the world in two calls.
  terrainTable["scatter_layer"] = [st](sol::table opts) -> bool {
    if (!st->terrainSubsystem)
      return false;
    const std::string name = opts.get_or("name", std::string{});
    const std::string mesh = opts.get_or("mesh", std::string{});
    if (name.empty() || mesh.empty()) {
      LOG_ERROR("Script", "terrain.scatter_layer needs 'name' and 'mesh'");
      return false;
    }

    ScatterManifest &manifest = st->terrainSubsystem->manifest();
    auto it = std::find_if(manifest.layers.begin(), manifest.layers.end(),
                           [&](const ScatterLayer &l) { return l.name == name; });
    ScatterLayer &layer =
        (it != manifest.layers.end())
            ? *it
            : (manifest.layers.push_back(ScatterLayer{}), manifest.layers.back());

    layer.name = name;
    layer.meshPath = mesh;
    const std::string type = opts.get_or("type", std::string("tree"));
    layer.type = (type == "grass")  ? ScatterLayerType::Grass
                 : (type == "rock") ? ScatterLayerType::Rock
                                    : ScatterLayerType::Tree;
    layer.density = opts.get_or("density", layer.density);
    layer.biomeMeadow = opts.get_or("biomeMeadow", layer.biomeMeadow);
    layer.biomeForest = opts.get_or("biomeForest", layer.biomeForest);
    layer.biomeMountain = opts.get_or("biomeMountain", layer.biomeMountain);
    layer.minSpacing = opts.get_or("minSpacing", layer.minSpacing);
    layer.scaleMin = opts.get_or("scaleMin", layer.scaleMin);
    layer.scaleMax = opts.get_or("scaleMax", layer.scaleMax);
    layer.slopeMax = opts.get_or("slopeMax", layer.slopeMax);
    layer.randomYaw = opts.get_or("randomYaw", layer.randomYaw);
    layer.alignToNormal = opts.get_or("alignToNormal", layer.alignToNormal);
    layer.leanMaxDeg = opts.get_or("leanMaxDeg", layer.leanMaxDeg);
    layer.sinkIntoGround = opts.get_or("sinkIntoGround", layer.sinkIntoGround);
    layer.castRayShadow = opts.get_or("castRayShadow", layer.castRayShadow);
    layer.maxDrawDistance = opts.get_or("maxDrawDistance", layer.maxDrawDistance);
    layer.cullCellSize = opts.get_or("cullCellSize", layer.cullCellSize);
    layer.windStrength = opts.get_or("windStrength", layer.windStrength);
    layer.wind = layer.windStrength > 0.0f;
    layer.groundOcclusion = opts.get_or("groundOcclusion", layer.groundOcclusion);
    readColor(opts, "tint", layer.tint);

    // Grass placed by the ten-thousand must stay out of the TLAS; letting a
    // script opt a dense layer in is a frame-time cliff, not a look choice.
    if (layer.type == ScatterLayerType::Grass && layer.castRayShadow &&
        !opts.get_or("forceRayShadow", false))
      LOG_WARN("Script", "scatter layer '" + name +
                             "' is grass with ray-traced shadows on; the TLAS "
                             "takes one instance per placement. Set "
                             "castRayShadow=false unless density is very low.");

    // Rebuild with the current settings so the new layer takes effect.
    st->terrainSubsystem->regenerate(st->terrainSubsystem->settings());
    return true;
  };

  terrainTable["scatter_layers"] = [st, &lua]() -> sol::table {
    sol::table out = lua.create_table();
    if (!st->terrainSubsystem)
      return out;
    const ScatterManifest &m = st->terrainSubsystem->manifest();
    for (size_t i = 0; i < m.layers.size(); ++i) {
      sol::table l = lua.create_table();
      l["name"] = m.layers[i].name;
      l["mesh"] = m.layers[i].meshPath;
      l["density"] = m.layers[i].density;
      out[i + 1] = l;
    }
    return out;
  };
}
