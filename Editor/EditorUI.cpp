#include "EditorUI.h"
#include "CloudFX.h"
#include "AssetManager.h"
#include "Core/EngineEvents.h"
#include "Core/EventBus.h"
#include "Core/Logger.h"
#include "Core/ProjectConfig.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "FBXModel.h"
#include "FireFX.h"
#include "HDRSky.h"
#include "OBJModel.h"
#include "Scene.h"
#include "SunFX.h"

#include <imgui.h>

#include <ImGuizmo.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

// Helper: build TRS matrix from euler-degrees rotation
static auto buildTRS = [](const glm::vec3 &pos, const glm::vec3 &rotDeg,
                          const glm::vec3 &scale) {
  glm::mat4 m(1.0f);
  m = glm::translate(m, pos);
  m = glm::rotate(m, glm::radians(rotDeg.y), glm::vec3(0, 1, 0));
  m = glm::rotate(m, glm::radians(rotDeg.x), glm::vec3(1, 0, 0));
  m = glm::rotate(m, glm::radians(rotDeg.z), glm::vec3(0, 0, 1));
  m = glm::scale(m, scale);
  return m;
};

// =============================================================================
// Main Editor Panel
// =============================================================================
EditorUIOutput EditorUI::draw(EditorContext &ctx) {
  EditorUIOutput out{};

  if (!ctx.uiMode)
    return out;

  ImGuiIO &io = ImGui::GetIO();
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
    ctx.events.publish(UndoRequestedEvent{});
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
    ctx.events.publish(RedoRequestedEvent{});

  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("Edit")) {
      if (ImGui::MenuItem("Undo", "Ctrl+Z"))
        ctx.events.publish(UndoRequestedEvent{});
      if (ImGui::MenuItem("Redo", "Ctrl+Y"))
        ctx.events.publish(RedoRequestedEvent{});
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
      ImGui::MenuItem("Editor Controls", nullptr, &mShowPanelEditor);
      ImGui::MenuItem("Asset Browser", nullptr, &mShowPanelAssetBrowser);
      ImGui::MenuItem("History", nullptr, &mShowPanelHistory);
      ImGui::MenuItem("Log Console", nullptr, &mShowPanelLogConsole);
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }

  if (!mShowPanelEditor && !mShowPanelAssetBrowser && !mShowPanelHistory &&
      !mShowPanelLogConsole) {
    out.wantCaptureMouse = io.WantCaptureMouse;
    out.wantCaptureKeyboard = io.WantCaptureKeyboard;
    return out;
  }

  if (mShowPanelEditor) {
    ImGui::Begin("Editor Controls", &mShowPanelEditor);

  // Save / Load
  if (ImGui::Button("Save Config"))
    ctx.events.publish(SaveConfigRequestedEvent{});
  ImGui::SameLine();
  if (ImGui::Button("Load Config"))
    ctx.events.publish(LoadConfigRequestedEvent{});

  ImGui::Separator();

  if (ImGui::CollapsingHeader("Project Settings")) {
    static char projectRoot[512] = "";
    static char shaderRoot[512] = "";
    static char assetRoot[512] = "";
    static bool initialized = false;

    if (!initialized) {
      std::snprintf(projectRoot, sizeof(projectRoot), "%s",
                    ctx.projectConfig.projectRoot.c_str());
      std::snprintf(shaderRoot, sizeof(shaderRoot), "%s",
                    ctx.projectConfig.shaderRoot.c_str());
      std::snprintf(assetRoot, sizeof(assetRoot), "%s",
                    ctx.projectConfig.assetRoot.c_str());
      initialized = true;
    }

    if (ImGui::InputText("Project Root", projectRoot, sizeof(projectRoot)))
      ctx.projectConfig.projectRoot = projectRoot;
    if (ImGui::InputText("Shader Root", shaderRoot, sizeof(shaderRoot)))
      ctx.projectConfig.shaderRoot = shaderRoot;
    if (ImGui::InputText("Asset Root", assetRoot, sizeof(assetRoot)))
      ctx.projectConfig.assetRoot = assetRoot;

    if (ImGui::Button("Save Project Settings"))
      ctx.events.publish(SaveProjectConfigRequestedEvent{});
  }

  if (ImGui::CollapsingHeader("Asset Pipeline")) {
    static char importPath[512] = "";
    ImGui::InputText("Import Path", importPath, sizeof(importPath));
    if (ImGui::Button("Queue Import")) {
      if (importPath[0] != '\0') {
        (void)ctx.assets.queueImport(importPath);
        importPath[0] = '\0';
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Process Queue")) {
      ctx.assets.processImportQueue();
    }
    ImGui::Checkbox("Auto Process Queue", &ctx.autoProcessImportQueue);
    ImGui::Text("Cook Root: %s", ctx.assets.cookRoot().c_str());

    ImGui::Separator();
    ImGui::TextUnformatted("Import Queue");
    ImGui::BeginChild("ImportQueue", ImVec2(0, 120), true);
    for (const auto &job : ctx.assets.importJobs()) {
      ImGui::Text("#%llu [%s] %s",
                  (unsigned long long)job.id,
                  job.status.c_str(),
                  job.sourcePath.c_str());
      if (!job.warning.empty()) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "  %s",
                           job.warning.c_str());
      }
    }
    ImGui::EndChild();
  }

  if (ImGui::CollapsingHeader("Hot Reload")) {
    ImGui::Checkbox("Enabled", &ctx.hotReloadEnabled);
    if (ctx.hotReloadMessages && !ctx.hotReloadMessages->empty()) {
      ImGui::BeginChild("HotReloadMsgs", ImVec2(0, 100), true);
      for (const auto &m : *ctx.hotReloadMessages)
        ImGui::TextUnformatted(m.c_str());
      ImGui::EndChild();
    }
  }

  // ── Performance ──────────────────────────────────────────────────────
  if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
    float fps = (ctx.dt > 0.0f) ? 1.0f / ctx.dt : 0.0f;
    ImGui::Text("FPS: %.1f  (%.2f ms)", fps, ctx.dt * 1000.0f);
    ImGui::Text("Entities: %d", ctx.entityCount);
    ImGui::Text("Particles: %d", ctx.particleCount);
    ImGui::Text("Visible Drawn: %d", ctx.visibleDrawn);
    ImGui::Text("Culled: %d", ctx.visibleCulled);

    // Frame time graph
    static float frameHistory[120] = {};
    static int frameIdx = 0;
    frameHistory[frameIdx] = ctx.dt * 1000.0f;
    frameIdx = (frameIdx + 1) % 120;
    ImGui::PlotLines("Frame (ms)", frameHistory, 120, frameIdx, nullptr, 0.0f,
                     33.3f, ImVec2(0, 40));
  }

  // ── Render Debug ─────────────────────────────────────────────────────
  if (ImGui::CollapsingHeader("Render Debug")) {
    ImGui::Checkbox("Wireframe", &ctx.wireframe);
    ImGui::Checkbox("Disable Shadows", &ctx.disableShadows);
    ImGui::Checkbox("Disable Clouds", &ctx.disableClouds);
    ImGui::Checkbox("Freeze Time", &ctx.freezeTime);
    ImGui::Checkbox("Frustum Culling", &ctx.cullingEnabled);
  }

  if (ImGui::CollapsingHeader("Render Graph")) {
    if (ctx.renderPassOrder && !ctx.renderPassOrder->empty()) {
      for (std::size_t i = 0; i < ctx.renderPassOrder->size(); ++i) {
        ImGui::Text("%zu. %s", i + 1, (*ctx.renderPassOrder)[i].c_str());
      }
    } else {
      ImGui::TextUnformatted("No pass data.");
    }
  }

  // ── Sun ──────────────────────────────────────────────────────────────
  if (ImGui::CollapsingHeader("Sun", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::DragFloat3("Position##Sun", &ctx.sun.sunPos.x, 0.1f);
    ImGui::SliderFloat("Size##Sun", &ctx.sun.sunSize, 0.2f, 10.0f);
    ImGui::SliderFloat("Halo Mult", &ctx.sun.haloSizeMult, 1.0f, 3.0f);
    ImGui::SliderFloat("Glow Strength", &ctx.sun.glowStrength, 0.0f, 12.0f);

    ImGui::Spacing();
    ImGui::TextUnformatted("Lighting");
    ImGui::DragFloat3("Direction##Sun", &ctx.sun.sunDir.x, 0.01f, -1.0f, 1.0f);
    if (glm::length(ctx.sun.sunDir) < 0.0001f)
      ctx.sun.sunDir = glm::vec3(0.0f, -1.0f, 0.0f);
    ctx.sun.sunDir = glm::normalize(ctx.sun.sunDir);
    ImGui::ColorEdit3("Color##Sun", &ctx.sun.sunColor.x);
    ImGui::SliderFloat("Ambient", &ctx.sun.ambientStrength, 0.0f, 1.0f);

    ImGui::Spacing();
    ImGui::TextUnformatted("Particles");
    ImGui::SliderFloat("Emit Rate", &ctx.sun.emitRate, 0.0f, 3000.0f);
    ImGui::SliderInt("Max Particles", &ctx.sun.maxParticles, 0, 8000);
    ImGui::SliderFloat("Speed##SunPart", &ctx.sun.particleSpeed, 0.0f, 25.0f);
    ImGui::SliderFloat("Lifetime##SunPart", &ctx.sun.particleLife, 0.05f, 6.0f);
    ImGui::SliderFloat("Size##SunPart", &ctx.sun.particleSize, 0.01f, 1.0f);
  }

  // ── Shadows ──────────────────────────────────────────────────────────
  if (ImGui::CollapsingHeader("Shadows")) {
    ImGui::SliderFloat("Shadow Strength", &ctx.shadowStrength, 0.0f, 3.0f);
    ImGui::DragFloat("Shadow Far Plane", &ctx.shadowFarPlane, 1.0f, 10.0f,
                     1000.0f);
  }

  // ── Sky ──────────────────────────────────────────────────────────────
  if (ImGui::CollapsingHeader("Sky")) {
    ImGui::SliderFloat("Exposure", &ctx.exposure, 0.1f, 5.0f);
    ImGui::SliderFloat("Gamma", &ctx.gamma, 1.0f, 4.0f);

    ImGui::Spacing();
    ImGui::Checkbox("Solid / Gradient Sky", &ctx.solidSky);
    ImGui::ColorEdit3("Horizon##Sky", ctx.skyHorizon);
    ImGui::ColorEdit3("Top##Sky", ctx.skyTop);

    ImGui::Spacing();
    static glm::vec3 skyRotDeg(0.0f);
    ImGui::SliderFloat("Rot X##Sky", &skyRotDeg.x, -180.0f, 180.0f);
    ImGui::SliderFloat("Rot Y##Sky", &skyRotDeg.y, -180.0f, 180.0f);
    ImGui::SliderFloat("Rot Z##Sky", &skyRotDeg.z, -180.0f, 180.0f);
    ctx.sky.setRotationDegrees(skyRotDeg);
    ctx.sky.setSolidSky(ctx.solidSky);
    ctx.sky.setSkyColors(
        glm::vec3(ctx.skyHorizon[0], ctx.skyHorizon[1], ctx.skyHorizon[2]),
        glm::vec3(ctx.skyTop[0], ctx.skyTop[1], ctx.skyTop[2]));
  }

  // ── Clouds ───────────────────────────────────────────────────────────
  if (ImGui::CollapsingHeader("Clouds")) {
    ImGui::SliderFloat("Height##Cloud", &ctx.cloud.height, 0.0f, 100.0f);
    ImGui::SliderFloat("Size##Cloud", &ctx.cloud.size, 10.0f, 500.0f);
    ImGui::SliderFloat("Scale##Cloud", &ctx.cloud.scale, 1.0f, 50.0f);
    ImGui::SliderFloat("Speed##Cloud", &ctx.cloud.speed, 0.001f, 0.1f);
    ImGui::SliderFloat("Cover##Cloud", &ctx.cloud.cover, 0.0f, 1.0f);
    ImGui::SliderFloat("Softness##Cloud", &ctx.cloud.softness, 0.0f, 1.0f);
    ImGui::SliderFloat("Alpha##Cloud", &ctx.cloud.alpha, 0.0f, 1.0f);
    ImGui::ColorEdit3("Color##Cloud", &ctx.cloud.color.x);

    ImGui::Spacing();
    ImGui::TextUnformatted("Volumetric");
    ImGui::SliderFloat("Thickness", &ctx.cloud.thickness, 0.5f, 30.0f);
    ImGui::SliderFloat("Density", &ctx.cloud.density, 0.1f, 5.0f);
    ImGui::SliderFloat("Light Absorption", &ctx.cloud.lightAbsorption, 0.0f,
                       5.0f);
    ImGui::SliderFloat("Phase G (HG)", &ctx.cloud.phaseG, 0.0f, 1.0f);
    ImGui::DragFloat2("Wind Dir", &ctx.cloud.windDir.x, 0.01f, -1.0f, 1.0f);
  }

  // ── Fire ─────────────────────────────────────────────────────────────
  if (ImGui::CollapsingHeader("Fire")) {
    auto &fp = ctx.fire.params();
    ImGui::Checkbox("Enabled##Fire", &fp.enabled);
    ImGui::DragFloat3("Offset##Fire", &fp.offset.x, 0.01f);
    ImGui::SliderFloat("Size##Fire", &fp.size, 0.1f, 10.0f);
    ImGui::SliderFloat("Intensity##Fire", &fp.intensity, 0.0f, 5.0f);

    ImGui::Spacing();
    ImGui::TextUnformatted("Smoke");
    ImGui::SliderFloat("Opacity##Smoke", &fp.smokeOpacity, 0.0f, 1.0f);
    ImGui::SliderFloat("Width##Smoke", &fp.smokeScaleXY, 0.5f, 4.0f);
    ImGui::SliderFloat("Height##Smoke", &fp.smokeScaleY, 0.5f, 6.0f);
    ImGui::SliderFloat("Lift##Smoke", &fp.smokeLift, 0.0f, 3.0f);
  }

  // ── Player ───────────────────────────────────────────────────────────
  if (ImGui::CollapsingHeader("Player")) {
    ImGui::SliderFloat("Walk Speed", &ctx.walkStep, 0.005f, 0.20f);
    ImGui::SliderFloat("Run Multiplier", &ctx.runMult, 1.0f, 6.0f);
    ImGui::SliderFloat("Jump Strength", &ctx.jumpStrength, 0.01f, 0.60f);
    ImGui::SliderFloat("Gravity##Player", &ctx.gravity, 0.001f, 0.05f);
    ImGui::Checkbox("Freeze Physics", &ctx.freezePhysics);
  }

  // ── Camera ───────────────────────────────────────────────────────────
  if (ImGui::CollapsingHeader("Camera")) {
    ImGui::SliderFloat("Mouse Sensitivity", &ctx.mouseSensitivity, 0.01f, 1.0f);
    ImGui::SliderFloat("FOV", &ctx.fov, 30.0f, 120.0f);
  }

  // ── Terrain ──────────────────────────────────────────────────────────
  if (ImGui::CollapsingHeader("Terrain")) {
    bool changed = false;
    changed |= ImGui::InputInt("Size##Terrain", &ctx.terrainSize);
    changed |= ImGui::InputFloat("Spacing##Terrain", &ctx.terrainSpacing);
    if (ctx.terrainSize < 1)
      ctx.terrainSize = 1;
    if (ctx.terrainSize > 200)
      ctx.terrainSize = 200;
    out.terrainDirty |= changed;
  }

  // ── Scene (Entity Loading) ──────────────────────────────────────────
  if (ImGui::CollapsingHeader("Scene")) {
    ImGui::TextUnformatted("Load Model (OBJ / FBX / GLTF / GLB)");

    // Direct path input
    ImGui::InputText("Path##Load", mPathInput, sizeof(mPathInput));
    ImGui::SameLine();
    if (ImGui::Button("Load##Direct")) {
      std::string p(mPathInput);
      if (!p.empty()) {
        ctx.events.publish(SpawnEntityRequestedEvent{p});
        mPathInput[0] = '\0';
      }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(
        "Use Asset Browser for searchable assets and drag-drop spawning.");

    ImGui::Separator();

    // New empty entity
    if (ImGui::Button("New Empty Entity")) {
      ctx.events.publish(CreateEmptyEntityRequestedEvent{"Empty"});
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Scene Serialization");
    static char scenePath[512] = "scene.json";
    ImGui::InputText("Scene Path", scenePath, sizeof(scenePath));
    if (ImGui::Button("Save Scene")) {
      ctx.events.publish(SaveSceneRequestedEvent{scenePath});
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Scene")) {
      ctx.events.publish(LoadSceneRequestedEvent{scenePath});
    }
  }

    ImGui::End();
  }

  if (mShowPanelAssetBrowser) {
    ImGui::Begin("Asset Browser", &mShowPanelAssetBrowser);

    if (mBrowsePath.empty())
      mBrowsePath = ctx.projectConfig.assetPath("");
    if (mBrowsePath.empty())
      mBrowsePath = "assets";

    ImGui::InputTextWithHint("Search", "name/ext/path", mAssetSearch,
                             sizeof(mAssetSearch));
    ImGui::TextWrapped("Root: %s", mBrowsePath.c_str());

    namespace fs = std::filesystem;
    const auto filterPass = [&](const std::string &path) {
      if (mAssetSearch[0] == '\0')
        return true;
      std::string hay = path;
      std::string needle = mAssetSearch;
      std::transform(hay.begin(), hay.end(), hay.begin(),
                     [](unsigned char c) { return (char)std::tolower(c); });
      std::transform(needle.begin(), needle.end(), needle.begin(),
                     [](unsigned char c) { return (char)std::tolower(c); });
      return hay.find(needle) != std::string::npos;
    };
    const auto isSupported = [](const fs::path &p) {
      std::string ext = p.extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c) { return (char)std::tolower(c); });
      return ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb";
    };

    if (fs::exists(mBrowsePath) && fs::is_directory(mBrowsePath)) {
      ImGui::BeginChild("AssetBrowserList", ImVec2(0, 260), true);
      for (auto it = fs::recursive_directory_iterator(mBrowsePath);
           it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file())
          continue;
        const fs::path p = it->path();
        if (!isSupported(p))
          continue;

        const std::string rel = fs::relative(p, mBrowsePath).string();
        if (!filterPass(rel))
          continue;

        ImGui::PushID(rel.c_str());
        const bool selected = ImGui::Selectable(rel.c_str());
        if (ImGui::BeginDragDropSource()) {
          const std::string full = p.string();
          ImGui::SetDragDropPayload("ASSET_PATH", full.c_str(),
                                    full.size() + 1);
          ImGui::TextUnformatted(rel.c_str());
          ImGui::EndDragDropSource();
        }
        if (selected)
          ctx.events.publish(SpawnEntityRequestedEvent{p.string()});
        ImGui::PopID();
      }
      ImGui::EndChild();
    } else {
      ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1),
                         "Asset root does not exist: %s", mBrowsePath.c_str());
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Import Queue");
    ImGui::BeginChild("AssetBrowserImportQueue", ImVec2(0, 120), true);
    for (const auto &job : ctx.assets.importJobs()) {
      ImGui::Text("#%llu [%s] %s", (unsigned long long)job.id,
                  job.status.c_str(), job.sourcePath.c_str());
      if (!job.warning.empty()) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "  %s",
                           job.warning.c_str());
      }
    }
    ImGui::EndChild();
    ImGui::End();
  }

  if (mShowPanelHistory) {
    ImGui::Begin("History", &mShowPanelHistory);
    if (ImGui::Button("Undo"))
      ctx.events.publish(UndoRequestedEvent{});
    ImGui::SameLine();
    if (ImGui::Button("Redo"))
      ctx.events.publish(RedoRequestedEvent{});
    ImGui::Separator();

    if (ctx.historyLabels && !ctx.historyLabels->empty()) {
      for (int i = 0; i < (int)ctx.historyLabels->size(); ++i) {
        const bool selected = (i == ctx.historyIndex);
        std::string label = std::to_string(i) + ": " + (*ctx.historyLabels)[i];
        if (ImGui::Selectable(label.c_str(), selected))
          ctx.events.publish(SceneHistoryJumpRequestedEvent{i});
      }
    } else {
      ImGui::TextUnformatted("No history snapshots.");
    }
    ImGui::End();
  }

  if (mShowPanelLogConsole) {
    ImGui::Begin("Log Console", &mShowPanelLogConsole);
    static bool showRender = true;
    static bool showAsset = true;
    static bool showECS = true;
    static bool showEditor = true;
    static bool showCore = true;
    static bool showRuntime = true;
    static bool showOther = true;
    static bool showTrace = false;
    static bool showInfo = true;
    static bool showWarn = true;
    static bool showError = true;
    static bool autoScroll = true;
    static char textFilter[128] = "";

    ImGui::Checkbox("Render", &showRender);
    ImGui::SameLine();
    ImGui::Checkbox("Asset", &showAsset);
    ImGui::SameLine();
    ImGui::Checkbox("ECS", &showECS);
    ImGui::SameLine();
    ImGui::Checkbox("Editor", &showEditor);
    ImGui::SameLine();
    ImGui::Checkbox("Core", &showCore);
    ImGui::SameLine();
    ImGui::Checkbox("Runtime", &showRuntime);
    ImGui::SameLine();
    ImGui::Checkbox("Other", &showOther);

    ImGui::Checkbox("Trace", &showTrace);
    ImGui::SameLine();
    ImGui::Checkbox("Info", &showInfo);
    ImGui::SameLine();
    ImGui::Checkbox("Warn", &showWarn);
    ImGui::SameLine();
    ImGui::Checkbox("Error/Fatal", &showError);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll);
    ImGui::InputTextWithHint("Filter", "message text", textFilter,
                             sizeof(textFilter));

    const auto entries = Logger::instance().recentEntries(3000);
    ImGui::BeginChild("LogEntries", ImVec2(0, 260), true);
    for (const auto &e : entries) {
      const bool levelPass =
          (e.level == Logger::Level::Trace && showTrace) ||
          (e.level == Logger::Level::Info && showInfo) ||
          (e.level == Logger::Level::Warn && showWarn) ||
          ((e.level == Logger::Level::Error || e.level == Logger::Level::Fatal) &&
           showError);
      if (!levelPass)
        continue;

      bool categoryPass = showOther;
      if (e.category == "Render")
        categoryPass = showRender;
      else if (e.category == "Asset")
        categoryPass = showAsset;
      else if (e.category == "ECS")
        categoryPass = showECS;
      else if (e.category == "Editor")
        categoryPass = showEditor;
      else if (e.category == "Core")
        categoryPass = showCore;
      else if (e.category == "Runtime")
        categoryPass = showRuntime;
      if (!categoryPass)
        continue;

      if (textFilter[0] != '\0' &&
          e.message.find(textFilter) == std::string::npos)
        continue;

      ImVec4 c(0.8f, 0.8f, 0.8f, 1.0f);
      if (e.level == Logger::Level::Warn)
        c = ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
      else if (e.level == Logger::Level::Error || e.level == Logger::Level::Fatal)
        c = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
      ImGui::TextColored(c, "[%s][%s][%s] %s", e.timestamp.c_str(),
                         Logger::levelName(e.level), e.category.c_str(),
                         e.message.c_str());
    }
    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
      ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
  }

  // Capture IO state
  out.wantCaptureMouse = io.WantCaptureMouse;
  out.wantCaptureKeyboard = io.WantCaptureKeyboard;
  return out;
}

// =============================================================================
// Component Inspector
// =============================================================================
bool EditorUI::drawInspector(bool uiMode, Registry &reg,
                             uint32_t selectedEntityId) {
  bool edited = false;
  if (!uiMode || selectedEntityId == 0)
    return false;

  ImGui::Begin("Inspector");

  ImGui::Text("Entity %u", selectedEntityId);
  ImGui::Separator();

  // ── Name ─────────────────────────────────────────────────────────────
  if (reg.has<NameComponent>(selectedEntityId)) {
    auto &nc = reg.get<NameComponent>(selectedEntityId);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", nc.name.c_str());
    if (ImGui::InputText("Name", buf, sizeof(buf))) {
      nc.name = buf;
      edited = true;
    }
  }

  // ── Transform ────────────────────────────────────────────────────────
  if (reg.has<TransformComponent>(selectedEntityId)) {
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
      auto &tr = reg.get<TransformComponent>(selectedEntityId);
      edited |= ImGui::DragFloat3("Position##Tr", &tr.position.x, 0.1f);
      edited |= ImGui::DragFloat3("Rotation##Tr", &tr.rotation.x, 0.5f);
      edited |=
          ImGui::DragFloat3("Scale##Tr", &tr.scale.x, 0.01f, 0.01f, 100.0f);
    }
  }

  // ── Mesh ─────────────────────────────────────────────────────────────
  if (reg.has<MeshComponent>(selectedEntityId)) {
    if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
      auto &mc = reg.get<MeshComponent>(selectedEntityId);
      edited |= ImGui::Checkbox("Visible", &mc.visible);
      edited |= ImGui::Checkbox("Casts Shadow", &mc.castsShadow);
      if (mc.type == MeshComponent::AssetType::OBJ && mc.objModel) {
        ImGui::Text("Model: OBJ (%zu submeshes)", mc.objModel->submeshCount());
      } else if (mc.type == MeshComponent::AssetType::FBX && mc.fbxModel) {
        ImGui::Text("Model: FBX/GLTF (%zu submeshes)",
                    mc.fbxModel->submeshCount());
      } else {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Model: null");
      }
    }
  }

  // ── Physics ──────────────────────────────────────────────────────────
  if (reg.has<PhysicsComponent>(selectedEntityId)) {
    if (ImGui::CollapsingHeader("Physics")) {
      auto &ph = reg.get<PhysicsComponent>(selectedEntityId);
      edited |= ImGui::DragFloat3("Velocity", &ph.velocity.x, 0.01f);
      edited |=
          ImGui::DragFloat("Gravity##Phys", &ph.gravity, 0.001f, 0.0f, 0.1f);
      edited |= ImGui::Checkbox("On Ground", &ph.onGround);
    }
  }

  // ── Lifecycle ────────────────────────────────────────────────────────
  if (reg.has<LifecycleComponent>(selectedEntityId)) {
    if (ImGui::CollapsingHeader("Lifecycle")) {
      auto &lc = reg.get<LifecycleComponent>(selectedEntityId);
      const char *state = "Alive";
      if (lc.state == EntityLifecycleState::Disabled)
        state = "Disabled";
      else if (lc.state == EntityLifecycleState::PendingDestroy)
        state = "PendingDestroy";
      ImGui::Text("State: %s", state);
      if (lc.state != EntityLifecycleState::PendingDestroy) {
        bool disabled = (lc.state == EntityLifecycleState::Disabled);
        if (ImGui::Checkbox("Disabled", &disabled)) {
          lc.state = disabled ? EntityLifecycleState::Disabled
                              : EntityLifecycleState::Alive;
          edited = true;
        }
      }
    }
  }

  // ── Hierarchy ────────────────────────────────────────────────────────
  if (reg.has<HierarchyComponent>(selectedEntityId)) {
    if (ImGui::CollapsingHeader("Hierarchy")) {
      auto &h = reg.get<HierarchyComponent>(selectedEntityId);
      ImGui::Text("Parent: %u", h.parent);
      ImGui::Text("Children: %d", (int)h.children.size());
    }
  }

  // ── Camera ───────────────────────────────────────────────────────────
  if (reg.has<CameraComponent>(selectedEntityId)) {
    if (ImGui::CollapsingHeader("Camera Component")) {
      auto &cam = reg.get<CameraComponent>(selectedEntityId);
      edited |= ImGui::DragFloat("FOV##Cam", &cam.fov, 0.5f, 10.0f, 170.0f);
      edited |= ImGui::DragFloat("Yaw##Cam", &cam.yaw, 0.5f);
      edited |= ImGui::DragFloat("Pitch##Cam", &cam.pitch, 0.5f, -89.0f, 89.0f);
      edited |= ImGui::Checkbox("Is Primary", &cam.isPrimary);
      ImGui::Text("Front: (%.2f, %.2f, %.2f)", cam.front.x, cam.front.y,
                  cam.front.z);
    }
  }

  ImGui::End();
  return edited;
}

// =============================================================================
// Gizmo & Outliner
// =============================================================================
bool EditorUI::drawGizmo(bool uiMode, const glm::mat4 &view,
                         const glm::mat4 &projection, Scene &scene, SunFX &sun,
                         EventBus &events, EditorSelectionState &s,
                         glm::vec3 &cameraPos) {
  bool edited = false;
  if (!uiMode)
    return false;

  ImGuizmo::BeginFrame();
  ImGuizmo::SetOrthographic(false);
  ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());

  ImGuiIO &io = ImGui::GetIO();
  ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);

  auto &reg = scene.registry();

  // --- Helpers ---
  auto passFilter = [&](const char *name) -> bool {
    if (s.outlinerFilter[0] == 0)
      return true;
    std::string a = name ? name : "";
    std::string b = s.outlinerFilter;
    for (auto &c : a)
      c = (char)tolower((unsigned char)c);
    for (auto &c : b)
      c = (char)tolower((unsigned char)c);
    return a.find(b) != std::string::npos;
  };

  auto isSelected = [&](uint32_t id) {
    for (uint32_t v : s.selectedEntities)
      if (v == id)
        return true;
    return false;
  };

  auto addToSelection = [&](uint32_t id) {
    if (!isSelected(id))
      s.selectedEntities.push_back(id);
  };

  auto removeFromSelection = [&](uint32_t id) {
    for (int i = 0; i < (int)s.selectedEntities.size(); ++i) {
      if (s.selectedEntities[i] == id) {
        s.selectedEntities.erase(s.selectedEntities.begin() + i);
        break;
      }
    }
  };

  auto clearSelection = [&]() {
    s.selectedEntities.clear();
    s.selectedEntityId = 0;
    s.lastClickedEntity = 0;
    s.editObjPart = false;
    s.selectedObjPartName.clear();
    s.renaming = false;
  };

  // --- Gizmo Tools Window ---
  ImGui::Begin("Gizmo");
  if (ImGui::RadioButton("Translate", s.gizmoOp == ImGuizmo::TRANSLATE))
    s.gizmoOp = ImGuizmo::TRANSLATE;
  ImGui::SameLine();
  if (ImGui::RadioButton("Rotate", s.gizmoOp == ImGuizmo::ROTATE))
    s.gizmoOp = ImGuizmo::ROTATE;
  ImGui::SameLine();
  if (ImGui::RadioButton("Scale", s.gizmoOp == ImGuizmo::SCALE))
    s.gizmoOp = ImGuizmo::SCALE;
  ImGui::Separator();

  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload *payload =
            ImGui::AcceptDragDropPayload("ASSET_PATH")) {
      const char *path = static_cast<const char *>(payload->Data);
      if (path && path[0] != '\0')
        events.publish(SpawnEntityRequestedEvent{path});
    }
    ImGui::EndDragDropTarget();
  }
  ImGui::DragFloat("Focus distance", &s.focusDistance, 0.25f, 1.0f, 200.0f);
  ImGui::End();

  // --- Outliner Window ---
  ImGui::Begin("Outliner");
  ImGui::InputTextWithHint("##filter", "Search...", s.outlinerFilter, 128);
  ImGui::SameLine();
  if (ImGui::Button("Clear"))
    s.outlinerFilter[0] = 0;
  ImGui::Separator();

  // Sun selection (ID 0)
  bool sunSelected = (s.selectedEntityId == 0 && !s.selectedEntities.empty() &&
                      s.selectedEntities[0] == 0);

  if (ImGui::Selectable("Sun", s.selectedEntityId == 0)) {
    clearSelection();
    s.selectedEntityId = 0;
    s.selectedEntities.push_back(0);
  }
  ImGui::Separator();

  // Iterate ECS entities
  auto viewTr = reg.view<TransformComponent>();

  for (auto entity : viewTr) {
    uint32_t id = (uint32_t)entity;

    std::string name = "Entity " + std::to_string(id);
    if (reg.has<NameComponent>(entity))
      name = reg.get<NameComponent>(entity).name;

    if (!passFilter(name.c_str()))
      continue;

    bool selected = isSelected(id);
    const bool primary = (id == s.selectedEntityId);
    bool pushedPrimary = false;

    if (primary) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 0.4f, 1));
      pushedPrimary = true;
    }

    ImGui::PushID((int)id);
    if (ImGui::Selectable(name.c_str(), selected)) {
      const bool ctrl = ImGui::GetIO().KeyCtrl;

      if (ctrl) {
        if (selected)
          removeFromSelection(id);
        else
          addToSelection(id);
        s.lastClickedEntity = id;
      } else {
        clearSelection();
        s.selectedEntities.push_back(id);
        s.selectedEntityId = id;
        s.lastClickedEntity = id;
      }

      if (s.selectedEntityId == 0 || !isSelected(s.selectedEntityId))
        s.selectedEntityId = id;
    }
    ImGui::PopID();

    if (pushedPrimary)
      ImGui::PopStyleColor();
  }

  ImGui::Separator();
  bool hasPrimaryEntity = (s.selectedEntityId != 0 &&
                           reg.has<TransformComponent>(s.selectedEntityId));

  if (ImGui::Button("Focus") && hasPrimaryEntity) {
    auto &tr = reg.get<TransformComponent>(s.selectedEntityId);
    cameraPos = tr.position + glm::vec3(0, 5, 10);
  }
  ImGui::SameLine();
  if (ImGui::Button("Rename") && hasPrimaryEntity) {
    s.renaming = true;
    std::string name = "Entity " + std::to_string(s.selectedEntityId);
    if (reg.has<NameComponent>(s.selectedEntityId))
      name = reg.get<NameComponent>(s.selectedEntityId).name;
    std::snprintf(s.renameBuf, 128, "%s", name.c_str());
  }
  ImGui::SameLine();
  if (ImGui::Button("Duplicate")) {
  } // Stub
  ImGui::SameLine();

  bool justCleared = false;
  if (ImGui::Button("Clear selection")) {
    clearSelection();
    justCleared = true;
  }
  ImGui::End();

  if (justCleared)
    return edited;

  // Rename Popup
  if (s.renaming && hasPrimaryEntity) {
    ImGui::Begin("Rename", &s.renaming, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::InputText("Name", s.renameBuf, 128);
    if (ImGui::Button("Apply")) {
      if (!reg.has<NameComponent>(s.selectedEntityId))
        reg.emplace<NameComponent>(s.selectedEntityId);
      reg.get<NameComponent>(s.selectedEntityId).name = s.renameBuf;
      edited = true;
      s.renaming = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
      s.renaming = false;
    ImGui::End();
  }

  // Model Parts
  OBJModel *modelPtr = nullptr;
  if (hasPrimaryEntity) {
    if (reg.has<MeshComponent>(s.selectedEntityId))
      modelPtr = reg.get<MeshComponent>(s.selectedEntityId).objModel;
  }

  ImGui::Begin("Model Parts");
  if (!hasPrimaryEntity)
    ImGui::Text("Select an entity first.");
  else if (!modelPtr)
    ImGui::Text("Selected entity has no OBJ model.");
  else {
    ImGui::Checkbox("Edit selected part", &s.editObjPart);
    auto names = modelPtr->objectNames();
    if (names.empty())
      ImGui::Text("No named parts found.");
    else {
      ImGui::Text("Select part:");
      for (const auto &n : names) {
        bool partSelected = (s.selectedObjPartName == n);
        if (ImGui::Selectable(n.c_str(), partSelected))
          s.selectedObjPartName = n;
      }
      if (!s.selectedObjPartName.empty()) {
        if (ImGui::Button("Clear selected part TRS")) {
          modelPtr->clearObjectLocalTRS(s.selectedObjPartName);
          edited = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear ALL parts TRS")) {
          modelPtr->clearAllObjectLocalTRS();
          edited = true;
        }
      }
    }
  }
  ImGui::End();

  // Gizmo Logic
  if (s.selectedEntityId == 0) {
    if (sunSelected) {
      glm::mat4 model = glm::translate(glm::mat4(1.0f), sun.sunPos);
      ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection),
                           (ImGuizmo::OPERATION)s.gizmoOp,
                           (ImGuizmo::MODE)s.gizmoMode, glm::value_ptr(model));
      if (ImGuizmo::IsUsing()) {
        float t[3], r[3], sc[3];
        ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(model), t, r, sc);
        sun.sunPos = {t[0], t[1], t[2]};
        edited = true;
      }
    }
    return edited;
  }

  if (!hasPrimaryEntity)
    return edited;

  auto &tr = reg.get<TransformComponent>(s.selectedEntityId);
  glm::mat4 M_entity = tr.getMatrix();
  bool canEditPart =
      (s.editObjPart && modelPtr && !s.selectedObjPartName.empty());

  if (!canEditPart) {
    glm::mat4 model = M_entity;
    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection),
                         (ImGuizmo::OPERATION)s.gizmoOp,
                         (ImGuizmo::MODE)s.gizmoMode, glm::value_ptr(model));
    if (ImGuizmo::IsUsing()) {
      float t[3], r[3], sc[3];
      ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(model), t, r, sc);
      tr.position = {t[0], t[1], t[2]};
      tr.rotation = {r[0], r[1], r[2]};
      tr.scale = {sc[0], sc[1], sc[2]};
      edited = true;
    }
  } else {
    glm::vec3 lp(0.0f), lr(0.0f), ls(1.0f);
    (void)modelPtr->getObjectLocalTRS(s.selectedObjPartName, lp, lr, ls);
    glm::mat4 M_partLocal = buildTRS(lp, lr, ls);
    glm::mat4 model = M_entity * M_partLocal;
    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection),
                         (ImGuizmo::OPERATION)s.gizmoOp,
                         (ImGuizmo::MODE)s.gizmoMode, glm::value_ptr(model));
    if (ImGuizmo::IsUsing()) {
      glm::mat4 newLocal = glm::inverse(M_entity) * model;
      float t[3], r[3], sc[3];
      ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(newLocal), t, r, sc);
      modelPtr->setObjectLocalTRS(
          s.selectedObjPartName, glm::vec3(t[0], t[1], t[2]),
          glm::vec3(r[0], r[1], r[2]), glm::vec3(sc[0], sc[1], sc[2]));
      edited = true;
    }
  }
  return edited;
}
