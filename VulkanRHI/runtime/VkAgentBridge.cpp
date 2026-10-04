#include "VkAgentBridge.h"

#include "VkAppState.h"
#include "VkScriptBindings.h"
#include "VulkanRenderer.h"
#include "subsystems/VkTerrainSubsystem.h"

#include "ECS/Components.h"
#include "Assets/MeshExportGLTF.h"
#include "ECS/Registry.h"
#include "Generators/AssetLibrary.h"
#include "Generators/GeneratorRegistry.h"
#include "Generators/TextureGen.h"
#include "Logger.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

using json = nlohmann::json;
using bridge::Response;

namespace {

constexpr float kPi = 3.14159265358979323846f;

glm::vec3 readVec3(const json &j, const char *key, glm::vec3 fallback) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_array() || it->size() < 3)
    return fallback;
  for (int i = 0; i < 3; ++i)
    if (!(*it)[i].is_number())
      return fallback;
  return glm::vec3((*it)[0].get<float>(), (*it)[1].get<float>(),
                   (*it)[2].get<float>());
}

// Turns a recipe request body into an AssetRecipe. Shared shape with the
// on-disk recipe format, so an MCP client can send exactly what it would
// write to assets/recipes/.
bool recipeFromJson(const json &params, gen::AssetRecipe &out,
                    std::string &error) {
  json body = params;
  // Accept both {recipe:{...}} and the recipe fields inline -- an AI writing
  // the call by hand will do either, and the distinction carries no meaning.
  if (params.contains("recipe") && params["recipe"].is_object())
    body = params["recipe"];
  return gen::parseRecipe(body, out, error);
}

} // namespace

bool VkAgentBridge::start(VkAppState &state, uint16_t port) {
  mState = &state;
  registerHandlers_();
  if (!mServer.start(port)) {
    LOG_ERROR("Bridge", "command port failed to start: " + mServer.lastError());
    return false;
  }
  return true;
}

void VkAgentBridge::stop() { mServer.stop(); }

void VkAgentBridge::registerHandlers_() {
  mServer.setHandler("render.atmosphereProbe", [this](const json &params, uint64_t) {
    if(!mState->renderer) return Response::fail("renderer unavailable");
    mState->renderer->enableAtmosphereProbe(params.value("enabled",true));
    return Response::ok(mState->renderer->atmosphereProbe());
  });

  VkAppState *st = mState;

  mServer.setHandler("engine.info", [this, st](const json &, uint64_t) {
    json info;
    info["engine"] = "glGen";
    info["renderer"] = "vulkan";
    info["frame"] = mFrame;
    info["generators"] = gen::GeneratorRegistry::instance().names();
    info["textureGenerators"] = gen::textureGeneratorNames();
    info["recipes"] = st->assetLibrary.recipeIds();
    // What a client may call. Cheaper than a round trip per probe.
    info["methods"] = json::array(
        {"engine.info", "script.eval", "assets.generators", "assets.schema",
         "assets.define", "assets.info", "assets.exportGlb", "scene.spawn", "scene.query", "scene.clear",
         "render.setParams", "render.getParams", "render.atmosphereProbe", "render.capture",
         "render.turntable"});
    return Response::ok(info);
  });

  // The general escape hatch: everything Phase 2 bound to Lua is reachable
  // here without the port needing a typed method for each.
  mServer.setHandler("script.eval", [st](const json &params, uint64_t) {
    if (!params.contains("code") || !params["code"].is_string())
      return Response::fail("script.eval needs a string 'code'");
    json result;
    std::string error;
    if (!st->scriptSystem.eval(params["code"].get<std::string>(), result, error))
      return Response::fail(error);
    return Response::ok(json{{"value", result}});
  });

  mServer.setHandler("assets.generators", [](const json &, uint64_t) {
    json list = json::array();
    for (const std::string &name : gen::GeneratorRegistry::instance().names()) {
      const gen::GeneratorInfo *info =
          gen::GeneratorRegistry::instance().find(name);
      json entry;
      entry["name"] = name;
      entry["description"] = info ? info->description : "";
      entry["polyBudget"] = info ? info->polyBudget : 0;
      list.push_back(entry);
    }
    json textures = json::array();
    for (const std::string &name : gen::textureGeneratorNames())
      textures.push_back(name);
    return Response::ok(json{{"generators", list}, {"textureGenerators", textures}});
  });

  mServer.setHandler("assets.schema", [](const json &params, uint64_t) {
    if (!params.contains("name") || !params["name"].is_string())
      return Response::fail("assets.schema needs a string 'name'");
    const std::string name = params["name"].get<std::string>();
    if (const gen::GeneratorInfo *info =
            gen::GeneratorRegistry::instance().find(name))
      return Response::ok(json{{"name", name},
                               {"description", info->description},
                               {"polyBudget", info->polyBudget},
                               {"schema", info->schema}});
    if (const json *tex = gen::textureGeneratorSchema(name))
      return Response::ok(json{{"name", name}, {"schema", *tex}});
    return Response::fail("unknown generator '" + name + "'");
  });

  mServer.setHandler("assets.define", [st](const json &params, uint64_t) {
    gen::AssetRecipe recipe;
    std::string error;
    if (!recipeFromJson(params, recipe, error))
      return Response::fail(error);

    gen::ResolveResult r = st->assetLibrary.resolve(recipe);
    if (!r.ok())
      return Response::fail(r.error);
    // Warnings travel back as text alongside the result: this is the channel
    // that lets a model correct its own parameters instead of inferring the
    // problem from pixels (AI_ASSET_PIPELINE_PLAN.md §4 / Phase 6).
    return Response::ok(json{{"assetId", r.assetId},
                             {"triangles", r.triangles},
                             {"regenerated", r.regenerated},
                             {"warnings", r.warnings}});
  });

  // A typed metrics query keeps MCP's validation path out of Lua string
  // construction and returns the same facts the regression harness compares.
  mServer.setHandler("assets.info", [st](const json &params, uint64_t) {
    const std::string assetId = params.value("assetId", std::string{});
    if (assetId.empty())
      return Response::fail("assets.info needs an 'assetId'");
    const gen::AssetLibrary::AssetInfo info = st->assetLibrary.info(assetId);
    if (!info.valid)
      return Response::fail("unknown asset '" + assetId + "'");
    return Response::ok(json{{"assetId", assetId},
                             {"triangles", info.triangles},
                             {"vertices", info.vertices},
                             {"submeshes", info.submeshes},
                             {"boundsMin", {info.boundsMin.x, info.boundsMin.y, info.boundsMin.z}},
                             {"boundsMax", {info.boundsMax.x, info.boundsMax.y, info.boundsMax.z}}});
  });

  mServer.setHandler("assets.exportGlb", [st](const json &params, uint64_t) {
    const std::string assetId = params.value("assetId", std::string{});
    const std::string path = params.value("path", std::string{});
    if (assetId.empty() || path.empty())
      return Response::fail("assets.exportGlb needs 'assetId' and 'path'");
    const MeshData *data = st->assets.getOBJData(st->assets.findMeshData(assetId));
    if (!data) return Response::fail("unknown generated asset '" + assetId + "'");
    std::string error;
    if (!exportMeshGLB(*data, path, error)) return Response::fail(error);
    return Response::ok(json{{"path", path}});
  });

  mServer.setHandler("scene.spawn", [st](const json &params, uint64_t) {
    if (!params.contains("assetId") || !params["assetId"].is_string())
      return Response::fail("scene.spawn needs a string 'assetId'");
    const std::string assetId = params["assetId"].get<std::string>();

    Registry &reg = st->scene.registry();
    const EntityId id = reg.create();
    auto &tr = reg.emplace<TransformComponent>(id);
    tr.position = readVec3(params, "pos", glm::vec3(0.0f));
    tr.rotation = readVec3(params, "rot", glm::vec3(0.0f));
    if (params.contains("scale")) {
      if (params["scale"].is_number())
        tr.scale = glm::vec3(params["scale"].get<float>());
      else
        tr.scale = readVec3(params, "scale", glm::vec3(1.0f));
    }
    // Convenience the Lua binding does not have: drop the asset onto the
    // procedural surface, which is what a client placing content almost
    // always wants and cannot compute itself.
    if (params.value("onGround", false) && st->terrainSubsystem)
      tr.position.y =
          st->terrainSubsystem->heightAt(glm::vec2(tr.position.x, tr.position.z));

    reg.emplace<MeshComponent>(id).assetId = assetId;
    reg.emplace<NameComponent>(
        id, NameComponent(params.value("name", assetId)));
    return Response::ok(json{{"entityId", id},
                             {"pos", json::array({tr.position.x, tr.position.y,
                                                  tr.position.z})}});
  });

  mServer.setHandler("scene.query", [st](const json &params, uint64_t) {
    Registry &reg = st->scene.registry();
    const std::string filter = params.value("name", std::string{});
    json entities = json::array();
    for (EntityId e : reg.view<MeshComponent>()) {
      if (!reg.has<TransformComponent>(e))
        continue;
      const auto &mc = reg.get<MeshComponent>(e);
      std::string name;
      if (reg.has<NameComponent>(e))
        name = reg.get<NameComponent>(e).name;
      if (!filter.empty() && name.find(filter) == std::string::npos)
        continue;
      const auto &tr = reg.get<TransformComponent>(e);
      json entry;
      entry["id"] = e;
      entry["name"] = name;
      entry["assetId"] = mc.assetId;
      entry["pos"] = json::array({tr.position.x, tr.position.y, tr.position.z});
      entry["visible"] = mc.visible;
      entities.push_back(entry);
    }
    return Response::ok(json{{"entities", entities}, {"count", entities.size()}});
  });

  mServer.setHandler("scene.clear", [st](const json &params, uint64_t) {
    Registry &reg = st->scene.registry();
    const std::string filter = params.value("name", std::string{});
    std::vector<EntityId> doomed;
    for (EntityId e : reg.view<MeshComponent>()) {
      if (!filter.empty()) {
        if (!reg.has<NameComponent>(e) ||
            reg.get<NameComponent>(e).name.find(filter) == std::string::npos)
          continue;
      }
      // Never delete the player: it carries the camera and controller, and a
      // blanket clear would leave the session unusable.
      if (e == st->gameplay.playerId)
        continue;
      doomed.push_back(e);
    }
    for (EntityId e : doomed)
      reg.destroy(e);
    return Response::ok(json{{"destroyed", doomed.size()}});
  });

  mServer.setHandler("render.setParams", [st](const json &params, uint64_t) {
    applyRenderParamsJson(*st, params);
    return Response::ok(renderParamsToJson(*st));
  });

  mServer.setHandler("render.getParams", [st](const json &, uint64_t) {
    return Response::ok(renderParamsToJson(*st));
  });

  // --- deferred: the image does not exist until a frame has been drawn ---
  mServer.setHandler("render.capture", [this](const json &params, uint64_t token) {
    if (mCaptureActive)
      return Response::fail("a capture is already in flight");
    const std::string path = params.value("path", std::string{});
    if (path.empty())
      return Response::fail("render.capture needs a 'path'");
    mCapture = PendingCapture{};
    mCapture.token = token;
    mCapture.path = path;
    mCapture.maxDim = params.value("maxDimension", mCaptureMaxDim);
    // Editor chrome is off by default: a bridge capture usually ends up in a
    // model's context, where docked panels are noise. Opt in when the QUESTION
    // is about the editor rather than the scene.
    mCapture.includeUi = params.value("includeUi", false);
    mCaptureActive = true;
    return Response::defer();
  });

  mServer.setHandler("render.turntable", [this, st](const json &params,
                                                    uint64_t token) {
    if (mTurntableActive)
      return Response::fail("a turntable is already in flight");
    if (!st->renderer)
      return Response::fail("no renderer");
    const std::string assetId = params.value("assetId", std::string{});
    if (assetId.empty())
      return Response::fail("render.turntable needs an 'assetId'");

    // Resolve the asset's bounds so the camera can frame it, whatever its
    // size. Reviewing an asset means reading its silhouette; a fixed camera
    // distance would crop a tree and lose a pebble.
    const MeshData *data =
        st->assets.getOBJData(st->assets.findMeshData(assetId));
    if (!data)
      return Response::fail("unknown or non-generated asset '" + assetId + "'");
    glm::vec3 mn(0.0f), mx(0.0f);
    if (!data->getGlobalBounds(mn, mx))
      return Response::fail("asset '" + assetId + "' has no bounds");

    mTurntable = PendingTurntable{};
    mTurntable.token = token;
    mTurntable.basePath = params.value("path", std::string("captures/turntable"));
    mTurntable.steps = std::clamp(params.value("steps", 6), 1, 32);

    const glm::vec3 size = mx - mn;
    const float extent = std::max({size.x, size.y, size.z, 0.1f});
    // Lift the subject clear of the terrain and its scattered vegetation:
    // at ground level the streamed forest occludes exactly the silhouette
    // being reviewed (the lesson from Phase 1's showcase rig).
    const float ground =
        st->terrainSubsystem ? st->terrainSubsystem->heightAt(glm::vec2(0.0f)) : 0.0f;
    // ABOVE THE FAR PLANE, deliberately. At merely "high" altitude the world is
    // still in frame -- streaming terrain, settling physics bodies, scattered
    // vegetation -- and all three change between runs, which makes two renders
    // of the same asset differ in thousands of pixels and defeats any
    // golden-image comparison. Past the far plane the world is simply not
    // drawn, leaving the clean sky backdrop asset review wanted anyway.
    mTurntable.baseY =
        ground + std::max(st->renderer->params().farPlane * 1.25f, 600.0f);
    mTurntable.centerY = mTurntable.baseY + (mn.y + mx.y) * 0.5f;
    // Distance to fit `extent` vertically with headroom. The margin is
    // generous because the editor's docked panels overlay the frame edges.
    const float halfFov = glm::radians(st->renderer->params().fovDeg * 0.5f);
    // The floor only exists to stay clear of the 0.05 m near plane. It used to
    // be 2 m, which framed a boulder well and left a hand-sized prop as a few
    // pixels -- the distance must follow the subject, not a constant.
    mTurntable.radius = std::max(0.3f, (extent * 0.95f) / std::tan(halfFov));

    Registry &reg = st->scene.registry();
    mTurntable.entity = reg.create();
    reg.emplace<TransformComponent>(mTurntable.entity).position =
        glm::vec3(0.0f, mTurntable.baseY, 0.0f);
    reg.emplace<MeshComponent>(mTurntable.entity).assetId = assetId;
    reg.emplace<NameComponent>(mTurntable.entity, NameComponent("__turntable"));

    vkrhi::VulkanRenderer::Params &p = st->renderer->params();
    mTurntable.savedCamPos = p.camPos;
    mTurntable.savedYaw = p.camYawDeg;
    mTurntable.savedPitch = p.camPitchDeg;
    mTurntable.savedAutoExposure = p.autoExposure;
    mTurntable.savedTemporalAA = p.temporalAA;
    mTurntable.savedDeterministicCapture = p.deterministicCapture;
    mTurntable.savedExposure = p.exposure;
    mTurntable.savedSunYaw = p.lightYawDeg;
    mTurntable.savedSunPitch = p.lightPitchDeg;
    mTurntable.savedAtmosphere = p.atmosphere;
    mTurntable.savedCloudMaxSteps=p.style.cloudMaxSteps;
    mTurntable.savedCloudLightTaps=p.style.cloudLightTaps;
    mTurntable.savedCloudDetailScale=p.style.cloudDetailScale;
    mTurntable.savedFogDensity = p.fogDensity;
    mTurntable.savedFogMaxOpacity = p.fogMaxOpacity;
    // Fixed light, exposure AND clock: two turntables are only comparable if
    // everything except the asset is held still. The clock matters as much as
    // the light -- clouds drift and volumetrics shimmer with it, so without
    // pinning it no two renders of the same asset are ever the same image.
    p.atmosphere.enabled = false;
    p.atmosphere.bloomStrength = 0;
    p.autoExposure = false;
    // History and the global jitter phase depend on when a review starts.
    // Use the spatial resolve for reproducible goldens, then restore live AA.
    p.temporalAA = false;
    p.deterministicCapture = true;
    // Canonical reviews use a fixed legacy background. Map cloud quality and
    // the new periodic-noise scale must not alter a material comparison.
    p.style.cloudMaxSteps=64;p.style.cloudLightTaps=3;p.style.cloudDetailScale=90;
    p.exposure = 0.95f;
    p.lightYawDeg = 215.0f;
    p.lightPitchDeg = 38.0f;
    p.fogDensity = 0.0f;
    p.fogMaxOpacity = 0.0f;
    mTurntable.savedFixedTime = p.fixedTimeSeconds;
    p.fixedTimeSeconds = params.value("fixedTime", 100.0f);
    // The camera grade eases toward the biome under the camera using the REAL
    // clock, so it lands somewhere slightly different on every run. Off for the
    // duration; it grades the whole frame and would otherwise tint every shot
    // by an amount that depends on how long the engine had been running.
    mTurntable.savedCameraGrade = p.cameraGradeEnabled;
    p.cameraGradeEnabled = false;

    mTurntableActive = true;
    return Response::defer();
  });
}

void VkAgentBridge::update() {
  if (!mServer.running() || !mState)
    return;
  ++mFrame;

  // Handlers run here and nowhere else -- the whole point of the queue.
  mServer.poll();

  mHideUiThisFrame = false;

  if (mCaptureActive && !mCapture.requested && mState->renderer) {
    std::error_code ec;
    const std::filesystem::path p(mCapture.path);
    if (p.has_parent_path())
      std::filesystem::create_directories(p.parent_path(), ec);
    mState->renderer->requestCapture(mCapture.path, mCapture.maxDim);
    mCapture.requested = true;
    mHideUiThisFrame = !mCapture.includeUi;
  }

  if (mTurntableActive && mState->renderer) {
    vkrhi::VulkanRenderer::Params &p = mState->renderer->params();
    if (!mTurntable.shotRequested) {
      if (mTurntable.current >= mTurntable.steps) {
        // Done: restore the view and drop the temporary entity.
        p.camPos = mTurntable.savedCamPos;
        p.camYawDeg = mTurntable.savedYaw;
        p.camPitchDeg = mTurntable.savedPitch;
        p.autoExposure = mTurntable.savedAutoExposure;
        p.temporalAA = mTurntable.savedTemporalAA;
        p.deterministicCapture = mTurntable.savedDeterministicCapture;
        p.fixedTimeSeconds = mTurntable.savedFixedTime;
        p.cameraGradeEnabled = mTurntable.savedCameraGrade;
        // Review must not leave the user's world with turntable lighting/fog.
        p.exposure = mTurntable.savedExposure;
        p.lightYawDeg = mTurntable.savedSunYaw;
        p.lightPitchDeg = mTurntable.savedSunPitch;
        p.atmosphere = mTurntable.savedAtmosphere;
        p.style.cloudMaxSteps=mTurntable.savedCloudMaxSteps;
        p.style.cloudLightTaps=mTurntable.savedCloudLightTaps;
        p.style.cloudDetailScale=mTurntable.savedCloudDetailScale;
        p.fogDensity = mTurntable.savedFogDensity;
        p.fogMaxOpacity = mTurntable.savedFogMaxOpacity;
        Registry &reg = mState->scene.registry();
        if (reg.valid(mTurntable.entity))
          reg.destroy(mTurntable.entity);
        mServer.complete(mTurntable.token,
                         json{{"paths", mTurntable.paths},
                              {"steps", mTurntable.steps}});
        mTurntableActive = false;
      } else {
        const float t =
            static_cast<float>(mTurntable.current) / static_cast<float>(mTurntable.steps);
        const float angle = t * 2.0f * kPi;
        p.camPos = glm::vec3(std::sin(angle) * mTurntable.radius,
                             mTurntable.centerY + mTurntable.radius * 0.25f,
                             std::cos(angle) * mTurntable.radius);
        // Face the subject: the camera's yaw convention points along
        // (sin(yaw), _, cos(yaw)), so looking inward is the opposite bearing.
        p.camYawDeg = glm::degrees(angle) + 180.0f;
        p.camPitchDeg = -14.0f;

        if (mTurntable.settleFrames > 0) {
          --mTurntable.settleFrames;
          return;
        }

        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), "_%02d.png", mTurntable.current);
        const std::string path = mTurntable.basePath + suffix;
        std::error_code ec;
        const std::filesystem::path fs(path);
        if (fs.has_parent_path())
          std::filesystem::create_directories(fs.parent_path(), ec);
        mState->renderer->requestCapture(path, mCaptureMaxDim);
        mTurntable.paths.push_back(path);
        mTurntable.shotRequested = true;
        mHideUiThisFrame = true; // a turntable is always for review
        ++mTurntable.current;  // post-increment: advance AFTER requesting this frame's shot
        mTurntable.settleFrames = 8;
      }
    }
  }
}

bool VkAgentBridge::wantsCleanFrame() const { return mHideUiThisFrame; }

void VkAgentBridge::postFrame() {
  if (!mServer.running())
    return;

  // The draw that just finished is what wrote the PNG, so this is the first
  // honest moment to say it exists.
  if (mCaptureActive && mCapture.requested) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(mCapture.path, ec);
    if (exists)
      mServer.complete(mCapture.token, json{{"path", mCapture.path}});
    else
      mServer.complete(mCapture.token, nullptr,
                       "capture did not produce '" + mCapture.path + "'");
    mCaptureActive = false;
  }

  if (mTurntableActive && mTurntable.shotRequested)
    mTurntable.shotRequested = false; // next frame advances to the next step
}
