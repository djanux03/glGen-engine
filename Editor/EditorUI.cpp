#include "EditorUI.h"
#include "AssetManager.h"
#include "CloudFX.h"
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
#include "UFBXModel.h"

#include <imgui.h>

#include <ImGuizmo.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
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

  // Shortcuts
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
    ctx.events.publish(UndoRequestedEvent{});
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
    ctx.events.publish(RedoRequestedEvent{});

  // Main Menu
  drawMainMenuBar(ctx);

  // ── Toolbar ────────────────────────────────────────────────────────────────
  // Sync toolbar state FROM context (in case external code changed it)
  toolbarState.gizmoOp =
      static_cast<ToolbarState::GizmoOp>(ctx.selection.gizmoOp);
  toolbarState.shadingMode =
      ctx.wireframe ? ToolbarState::Wireframe : ToolbarState::Textured;

  EditorToolbar::draw(toolbarState);
  EditorToolbar::processShortcuts(toolbarState);

  // Sync toolbar state BACK to context
  ctx.selection.gizmoOp = static_cast<int>(toolbarState.gizmoOp);
  ctx.wireframe = (toolbarState.shadingMode == ToolbarState::Wireframe);
#ifdef IMGUI_HAS_DOCK
  ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::SetNextWindowViewport(viewport->ID);

  ImGuiWindowFlags host_window_flags = 0;
  host_window_flags |= ImGuiWindowFlags_NoTitleBar |
                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove;
  host_window_flags |=
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
  host_window_flags |= ImGuiWindowFlags_NoBackground;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

  ImGui::Begin("MainDockSpaceHost", nullptr, host_window_flags);

  // If the host window is hovered, it means the mouse is over the empty
  // viewport area and NOT over any docked panels. We record this to bypass
  // ImGui's mouse capture!
  bool isViewportHovered = ImGui::IsWindowHovered();

  ImGui::PopStyleVar(3);

  ImGuiID dockspaceId = ImGui::GetID("glGenDockspace");

  // Build default layout on first run or reset
  if (mResetLayout || ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImVec2 workSize = ImGui::GetMainViewport()->WorkSize;
    ImGui::DockBuilderSetNodeSize(dockspaceId, workSize);

    // AAA Layout
    // Split: Left 20% | Center | Right 25%
    ImGuiID dockLeft, dockCenter, dockRight;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.20f, &dockLeft,
                                &dockCenter);
    ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.25f, &dockRight,
                                &dockCenter);

    // Split center: Bottom 30% for content browser
    ImGuiID dockBottom;
    ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, 0.30f, &dockBottom,
                                &dockCenter);

    // Dock panels into their dedicated AAA positions
    ImGui::DockBuilderDockWindow("Hierarchy", dockLeft);
    ImGui::DockBuilderDockWindow("Inspector", dockRight);
    ImGui::DockBuilderDockWindow("Environment", dockRight);

    // Bottom dock gets browser, console, stats
    ImGui::DockBuilderDockWindow("Content Browser", dockBottom);
    ImGui::DockBuilderDockWindow("Console", dockBottom);
    ImGui::DockBuilderDockWindow("Statistics", dockBottom);
    ImGui::DockBuilderDockWindow("Profiler", dockBottom);

    ImGui::DockBuilderFinish(dockspaceId);

    // Ensure key panels are visible
    mShowHierarchy = true;
    mShowInspector = true;
    mShowAssets = true;
    mShowEnvironment = false; // Hide by default until user needs it
    mShowStats = false;
    mShowLog = false;
    mResetLayout = false;
  }

  // Activate dockspace every frame
  ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f),
                   ImGuiDockNodeFlags_PassthruCentralNode);

  ImGui::End();
#endif

  // ── Draw Panels ────────────────────────────────────────────────────────────
  const ImGuiWindowFlags panelFlags =
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove;

  if (mShowHierarchy) {
    ImGui::SetNextWindowSize(ImVec2(280, 400), ImGuiCond_FirstUseEver);
    drawHierarchy(ctx);
  }
  if (mShowInspector) {
    ImGui::SetNextWindowSize(ImVec2(320, 500), ImGuiCond_FirstUseEver);
    out.sceneModified |= drawInspector(ctx);
  }
  if (mShowAssets) {
    ImGui::SetNextWindowSize(ImVec2(600, 250), ImGuiCond_FirstUseEver);
    drawAssets(ctx);
  }
  if (mShowEnvironment) {
    ImGui::SetNextWindowSize(ImVec2(320, 250), ImGuiCond_FirstUseEver);
    drawEnvironment(ctx);
  }
  if (mShowLog) {
    ImGui::SetNextWindowSize(ImVec2(600, 250), ImGuiCond_FirstUseEver);
    drawLog(ctx);
  }
  if (mShowStats) {
    ImGui::SetNextWindowSize(ImVec2(250, 200), ImGuiCond_FirstUseEver);
    drawStats(ctx);
  }

  // Capture IO state
  out.wantCaptureMouse = io.WantCaptureMouse;
#ifdef IMGUI_HAS_DOCK
  if (isViewportHovered) {
    out.wantCaptureMouse = false;
  }
#endif
  out.wantCaptureKeyboard = io.WantCaptureKeyboard;

  return out;
}

void EditorUI::drawMainMenuBar(EditorContext &ctx) {
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("Save Config", "Ctrl+S"))
        ctx.events.publish(SaveConfigRequestedEvent{});
      if (ImGui::MenuItem("Load Config"))
        ctx.events.publish(LoadConfigRequestedEvent{});
      ImGui::Separator();
      if (ImGui::MenuItem("Save Project"))
        ctx.events.publish(SaveProjectConfigRequestedEvent{});
      ImGui::Separator();
      if (ImGui::MenuItem("Quit", "Alt+F4")) {
        // Request app exit via GLFW (if window available)
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
      if (ImGui::MenuItem("Undo", "Ctrl+Z"))
        ctx.events.publish(UndoRequestedEvent{});
      if (ImGui::MenuItem("Redo", "Ctrl+Y"))
        ctx.events.publish(RedoRequestedEvent{});
      ImGui::Separator();
      if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
        if (ctx.selection.selectedEntityId != 0)
          ctx.events.publish(
              DuplicateEntityRequestedEvent{ctx.selection.selectedEntityId});
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Entity")) {
      if (ImGui::MenuItem("Create Empty")) {
        ctx.events.publish(CreateEmptyEntityRequestedEvent{});
      }
      if (ImGui::BeginMenu("Create Primitive")) {
        if (ImGui::MenuItem("Cube"))
          ctx.events.publish(SpawnEntityRequestedEvent{"__primitive_cube"});
        if (ImGui::MenuItem("Sphere"))
          ctx.events.publish(SpawnEntityRequestedEvent{"__primitive_sphere"});
        if (ImGui::MenuItem("Plane"))
          ctx.events.publish(SpawnEntityRequestedEvent{"__primitive_plane"});
        ImGui::EndMenu();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Delete Selected", "Delete")) {
        if (ctx.selection.selectedEntityId != 0)
          ctx.events.publish(
              DeleteEntityRequestedEvent{ctx.selection.selectedEntityId});
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
      ImGui::MenuItem("Hierarchy", nullptr, &mShowHierarchy);
      ImGui::MenuItem("Inspector", nullptr, &mShowInspector);
      ImGui::MenuItem("Content Browser", nullptr, &mShowAssets);
      ImGui::MenuItem("Environment", nullptr, &mShowEnvironment);
      ImGui::MenuItem("Console", nullptr, &mShowLog);
      ImGui::MenuItem("Statistics", nullptr, &mShowStats);
      ImGui::Separator();
      ImGui::MenuItem("Lock Layout", nullptr, &mLockLayout);
      if (ImGui::MenuItem("Reset Layout")) {
        mResetLayout = true;
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
      if (ImGui::MenuItem("About glGen Engine")) {
        // TODO: About popup
      }
      ImGui::EndMenu();
    }

    // ── Right-aligned info ──
    float rightW = ImGui::CalcTextSize("FPS: 9999.9").x + 20;
    ImGui::SameLine(ImGui::GetWindowWidth() - rightW);
    ImGui::TextDisabled("FPS: %.1f", 1.0f / ctx.dt);

    ImGui::EndMainMenuBar();
  }
}

// =============================================================================
// Hierarchy (Scene Graph)
// =============================================================================
void EditorUI::drawHierarchy(EditorContext &ctx) {
  ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove;
  if (ImGui::Begin("Hierarchy", &mShowHierarchy, wf)) {
    auto &s = ctx.selection;
    ImGui::InputTextWithHint("##filter", "Search...", s.outlinerFilter, 128);
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
      s.outlinerFilter[0] = 0;
    ImGui::Separator();

    auto &reg = ctx.scene.registry();

    // Sun selection (ID 0)
    bool sunSelected =
        (s.selectedEntityId == 0 && !s.selectedEntities.empty() &&
         s.selectedEntities[0] == 0);
    if (ImGui::Selectable("Sun", sunSelected)) {
      s.selectedEntities.clear();
      s.selectedEntityId = 0;
      s.selectedEntities.push_back(0);
      s.lastClickedEntity = 0;
    }

    // Filter helper
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

    // Iterate Entities
    // TODO: Use HierarchyComponent for tree structure. For now, flat list.
    auto view = reg.view<TransformComponent>();
    for (auto entity : view) {
      uint32_t id = (uint32_t)entity;
      std::string name = "Entity " + std::to_string(id);
      if (reg.has<NameComponent>(entity))
        name = reg.get<NameComponent>(entity).name;

      if (!passFilter(name.c_str()))
        continue;

      bool isSelected = false;
      for (auto selId : s.selectedEntities)
        if (selId == id)
          isSelected = true;

      ImGui::PushID((int)id);
      if (ImGui::Selectable(name.c_str(), isSelected)) {
        if (ImGui::GetIO().KeyCtrl) {
          // Toggle selection
          if (isSelected) {
            s.selectedEntities.erase(std::remove(s.selectedEntities.begin(),
                                                 s.selectedEntities.end(), id),
                                     s.selectedEntities.end());
            if (s.selectedEntityId == id)
              s.selectedEntityId = 0;
          } else {
            s.selectedEntities.push_back(id);
            s.selectedEntityId = id;
          }
        } else {
          // Single selection
          s.selectedEntities.clear();
          s.selectedEntities.push_back(id);
          s.selectedEntityId = id;
        }
        s.lastClickedEntity = id;
      }
      ImGui::PopID();
    }
  }
  ImGui::End();
}

// =============================================================================
// Content Browser
// =============================================================================
void EditorUI::drawAssets(EditorContext &ctx) {
  ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove;
  if (ImGui::Begin("Content Browser", &mShowAssets, wf)) {
    if (mBrowsePath.empty())
      mBrowsePath = ctx.projectConfig.assetPath("");
    if (mBrowsePath.empty())
      mBrowsePath = "assets";

    if (ImGui::Button("Back")) {
      std::filesystem::path p = mBrowsePath;
      if (p.has_parent_path())
        mBrowsePath = p.parent_path().string();
    }
    ImGui::SameLine();
    ImGui::Text("%s", mBrowsePath.c_str());

    ImGui::Separator();

    // File list logic...
    namespace fs = std::filesystem;
    if (fs::exists(mBrowsePath) && fs::is_directory(mBrowsePath)) {
      float padding = 16.0f;
      float thumbnailSize = 64.0f;
      float cellSize = thumbnailSize + padding;
      float panelWidth = ImGui::GetContentRegionAvail().x;
      int columnCount = (int)(panelWidth / cellSize);
      if (columnCount < 1)
        columnCount = 1;

      if (ImGui::BeginTable("ContentTable", columnCount)) {
        for (auto const &dir_entry : fs::directory_iterator(mBrowsePath)) {
          const auto &path = dir_entry.path();
          std::string filename = path.filename().string();
          std::string ext = path.extension().string();
          for (auto &c : ext)
            c = (char)tolower((unsigned char)c);

          bool isDir = dir_entry.is_directory();
          bool isModel = (ext == ".obj" || ext == ".fbx" || ext == ".gltf" ||
                          ext == ".glb");
          bool isImage = (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                          ext == ".hdr");

          if (!isDir && !isModel && !isImage)
            continue;

          ImGui::TableNextColumn();
          ImGui::PushID(filename.c_str());

          // Build a visual button using ImGui drawing API
          ImVec2 pos = ImGui::GetCursorScreenPos();
          ImDrawList *draw_list = ImGui::GetWindowDrawList();

          // Transparent button holding the space
          bool hovered = false, held = false;
          bool clicked = ImGui::InvisibleButton(
              "##AssetBtn", ImVec2(thumbnailSize, thumbnailSize));
          hovered = ImGui::IsItemHovered();
          held = ImGui::IsItemActive();

          ImVec4 colorBg = hovered ? ImVec4(0.3f, 0.3f, 0.3f, 0.5f)
                                   : ImVec4(0.2f, 0.2f, 0.2f, 0.3f);
          draw_list->AddRectFilled(
              pos, ImVec2(pos.x + thumbnailSize, pos.y + thumbnailSize),
              ImGui::ColorConvertFloat4ToU32(colorBg), 4.0f);

          // Draw custom icons based on type
          ImVec2 center = ImVec2(pos.x + thumbnailSize * 0.5f,
                                 pos.y + thumbnailSize * 0.35f);
          float iconR = thumbnailSize * 0.25f;

          if (isDir) {
            // Draw a Folder icon
            ImU32 colFolder =
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.8f, 0.65f, 0.3f, 1.0f));
            draw_list->AddRectFilled(
                ImVec2(center.x - iconR, center.y - iconR + 4),
                ImVec2(center.x + iconR, center.y + iconR), colFolder, 2.0f);
            draw_list->AddRectFilled(
                ImVec2(center.x - iconR, center.y - iconR - 2),
                ImVec2(center.x - iconR + 10, center.y - iconR + 6), colFolder,
                2.0f);

            // Interaction logic
            if (clicked || (hovered && ImGui::IsMouseDoubleClicked(
                                           ImGuiMouseButton_Left))) {
              mBrowsePath = path.string();
            }
          } else if (isModel) {
            // Draw a Cube icon (Model)
            ImU32 colModel =
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.4f, 0.7f, 0.9f, 1.0f));
            draw_list->AddRectFilled(ImVec2(center.x - iconR, center.y - iconR),
                                     ImVec2(center.x + iconR, center.y + iconR),
                                     colModel, 4.0f);
            draw_list->AddLine(ImVec2(center.x - iconR, center.y - iconR),
                               center, IM_COL32(0, 0, 0, 100), 2.0f);
            draw_list->AddLine(ImVec2(center.x + iconR, center.y + iconR),
                               center, IM_COL32(0, 0, 0, 100), 2.0f);
          } else if (isImage) {
            // Draw a Image icon (Picture)
            ImU32 colImg =
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.5f, 0.9f, 0.5f, 1.0f));
            draw_list->AddRectFilled(ImVec2(center.x - iconR, center.y - iconR),
                                     ImVec2(center.x + iconR, center.y + iconR),
                                     colImg, 2.0f);
            draw_list->AddCircleFilled(ImVec2(center.x + 5, center.y - 5), 4.0f,
                                       IM_COL32(255, 255, 255, 255));
          }

          // Text Label
          ImVec2 textSize = ImGui::CalcTextSize(filename.c_str());
          float textX = pos.x + (thumbnailSize - textSize.x) * 0.5f;
          float textY = pos.y + thumbnailSize - textSize.y - 4.0f;
          if (textX < pos.x)
            textX = pos.x; // Keep within bounds

          // Truncate text if too long
          std::string displayTxt = filename;
          if (textSize.x > thumbnailSize) {
            displayTxt = filename.substr(0, 7) + "..";
            textSize = ImGui::CalcTextSize(displayTxt.c_str());
            textX = pos.x + (thumbnailSize - textSize.x) * 0.5f;
          }
          draw_list->AddText(
              ImVec2(textX, textY),
              ImGui::ColorConvertFloat4ToU32(ImVec4(0.9f, 0.9f, 0.92f, 1.0f)),
              displayTxt.c_str());

          // Drag and drop for files (not dirs)
          if (!isDir && ImGui::BeginDragDropSource(
                            ImGuiDragDropFlags_SourceAllowNullID)) {
            std::string fullPath = path.string();
            ImGui::SetDragDropPayload("ASSET_PATH", fullPath.c_str(),
                                      fullPath.size() + 1);
            ImGui::Text("Drop %s into Scene", filename.c_str());
            ImGui::EndDragDropSource();
          }

          if (isModel && clicked && hovered) {
            ctx.events.publish(SpawnEntityRequestedEvent{path.string()});
          }

          ImGui::PopID();
        }
        ImGui::EndTable();
      }
    }
    ImGui::End();
  }
}

// =============================================================================
// Environment Settings
// =============================================================================
void EditorUI::drawEnvironment(EditorContext &ctx) {
  ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove;
  if (ImGui::Begin("Environment", &mShowEnvironment, wf)) {
    if (ImGui::BeginTabBar("EnvTabs")) {
      if (ImGui::BeginTabItem("Game")) {
        ImGui::Text("Player");
        ImGui::DragFloat("Walk Speed", &ctx.walkStep, 0.001f, 0.0f, 1.0f);
        ImGui::DragFloat("Run Mult", &ctx.runMult, 0.1f, 1.0f, 10.0f);
        ImGui::DragFloat("Jump Force", &ctx.jumpStrength, 0.01f, 0.0f, 10.0f);
        ImGui::DragFloat("Gravity", &ctx.gravity, 0.001f, 0.0f, 1.0f);
        ImGui::Checkbox("Freeze Physics", &ctx.freezePhysics);
        ImGui::Separator();
        ImGui::Text("System");
        ImGui::Checkbox("Hot Reload", &ctx.hotReloadEnabled);
        ImGui::Checkbox("Auto Import", &ctx.autoProcessImportQueue);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Sun")) {
        ImGui::DragFloat3("Direction", &ctx.sun.sunDir.x, 0.01f, -1.0f, 1.0f);
        ImGui::ColorEdit3("Color", &ctx.sun.sunColor.x);
        ImGui::SliderFloat("Intensity", &ctx.sun.sunSize, 0.1f, 10.0f);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Sky")) {
        ImGui::ColorEdit3("Horizon", ctx.skyHorizon);
        ImGui::ColorEdit3("Top", ctx.skyTop);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Clouds")) {
        ImGui::SliderFloat("Cover", &ctx.cloud.cover, 0.0f, 1.0f);
        ImGui::SliderFloat("Density", &ctx.cloud.density, 0.0f, 5.0f);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Render")) {
        ImGui::Checkbox("Wireframe", &ctx.wireframe);

        bool enableShadows = !ctx.disableShadows;
        if (ImGui::Checkbox("Enable Shadows", &enableShadows)) {
          ctx.disableShadows = !enableShadows;
        }
        ImGui::Checkbox("Disable Clouds", &ctx.disableClouds);
        ImGui::Checkbox("Disable HDR", &ctx.disableHDR);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::End();
}

// =============================================================================
// Log Console
// =============================================================================
void EditorUI::drawLog(EditorContext &ctx) {
  ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove;
  if (ImGui::Begin("Console", &mShowLog, wf)) {
    // ── Toolbar ──
    if (ImGui::Button("Clear")) {
      // Logger doesn't support clear — just scroll to bottom
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &mConsoleAutoScroll);
    ImGui::SameLine();

    // Level filter toggles
    ImGui::PushStyleColor(ImGuiCol_Button, mFilterInfo
                                               ? ImVec4(0.2f, 0.5f, 0.8f, 1)
                                               : ImVec4(0.2f, 0.2f, 0.2f, 1));
    if (ImGui::SmallButton("Info"))
      mFilterInfo = !mFilterInfo;
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, mFilterWarn
                                               ? ImVec4(0.9f, 0.8f, 0.2f, 1)
                                               : ImVec4(0.2f, 0.2f, 0.2f, 1));
    if (ImGui::SmallButton("Warn"))
      mFilterWarn = !mFilterWarn;
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, mFilterError
                                               ? ImVec4(0.9f, 0.3f, 0.3f, 1)
                                               : ImVec4(0.2f, 0.2f, 0.2f, 1));
    if (ImGui::SmallButton("Error"))
      mFilterError = !mFilterError;
    ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("##ConsoleSearch", mConsoleSearch, sizeof(mConsoleSearch));
    ImGui::SameLine();
    ImGui::TextDisabled("Search");

    ImGui::Separator();

    // ── Log entries ──
    ImGui::BeginChild("LogEntries", ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const auto entries = Logger::instance().recentEntries(200);
    for (const auto &e : entries) {
      // Level filter
      if (e.level == Logger::Level::Info && !mFilterInfo)
        continue;
      if (e.level == Logger::Level::Warn && !mFilterWarn)
        continue;
      if (e.level == Logger::Level::Error && !mFilterError)
        continue;
      if (e.level == Logger::Level::Fatal && !mFilterError)
        continue;

      // Search filter
      if (mConsoleSearch[0] != '\0') {
        if (e.message.find(mConsoleSearch) == std::string::npos &&
            e.category.find(mConsoleSearch) == std::string::npos)
          continue;
      }

      ImVec4 color = ImVec4(0.75f, 0.75f, 0.78f, 1.0f); // Default dim text
      const char *levelTag = "[INFO]";
      if (e.level == Logger::Level::Warn) {
        color = ImVec4(0.95f, 0.78f, 0.2f, 1);
        levelTag = "[WARN]";
      }
      if (e.level == Logger::Level::Error) {
        color = ImVec4(0.92f, 0.3f, 0.3f, 1);
        levelTag = "[ERR] ";
      }
      if (e.level == Logger::Level::Fatal) {
        color = ImVec4(1.0f, 0.15f, 0.15f, 1);
        levelTag = "[FATAL]";
      }

      ImGui::TextColored(color, "%s [%s] %s", levelTag, e.category.c_str(),
                         e.message.c_str());
    }

    if (mConsoleAutoScroll &&
        ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10)
      ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
  }
  ImGui::End();
}

// =============================================================================
// Statistics
// =============================================================================
void EditorUI::drawStats(EditorContext &ctx) {
  ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove;
  if (ImGui::Begin("Statistics", &mShowStats, wf)) {
    float fps = 1.0f / ctx.dt;
    float ms = ctx.dt * 1000.0f;

    // ── FPS History Graph ──
    mFpsHistory[mFpsHistoryIdx] = fps;
    mFpsHistoryIdx = (mFpsHistoryIdx + 1) % kFpsHistorySize;

    char overlay[32];
    std::snprintf(overlay, sizeof(overlay), "%.1f FPS", fps);
    ImGui::PlotLines("##FPS", mFpsHistory, kFpsHistorySize, mFpsHistoryIdx,
                     overlay, 0.0f, 240.0f, ImVec2(0, 50));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Stats Table ──
    if (ImGui::BeginTable("StatsTable", 2, ImGuiTableFlags_SizingStretchProp)) {
      auto row = [](const char *label, const char *fmt, auto val) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", label);
        ImGui::TableNextColumn();
        char buf[64];
        std::snprintf(buf, sizeof(buf), fmt, val);
        ImGui::Text("%s", buf);
      };

      row("Frame Time", "%.2f ms", ms);
      row("Entities", "%d", ctx.entityCount);
      row("Particles", "%d", ctx.particleCount);
      row("Drawn", "%d", ctx.visibleDrawn);
      row("Culled", "%d", ctx.visibleCulled);

      ImGui::EndTable();
    }
  }
  ImGui::End();
}

// =============================================================================
// Component Inspector — Enhanced with Add/Remove, Color XYZ, Reset, all
// editors
// =============================================================================

// Helper: Color-coded XYZ DragFloat3 (Red/Green/Blue for X/Y/Z like Unreal)
static bool DragFloat3Colored(const char *label, float *v, float speed = 0.1f,
                              float vMin = 0.0f, float vMax = 0.0f) {
  bool edited = false;
  ImGui::PushID(label);

  float fullWidth = ImGui::CalcItemWidth();
  float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
  float fieldW = (fullWidth - spacing * 2.0f) / 3.0f;

  // X — Red
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.45f, 0.12f, 0.12f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                        ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                        ImVec4(0.65f, 0.18f, 0.18f, 1.0f));
  ImGui::SetNextItemWidth(fieldW);
  edited |= ImGui::DragFloat("##X", &v[0], speed, vMin, vMax, "X: %.2f");
  ImGui::PopStyleColor(3);

  ImGui::SameLine(0, spacing);

  // Y — Green
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.40f, 0.12f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                        ImVec4(0.15f, 0.50f, 0.15f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                        ImVec4(0.18f, 0.60f, 0.18f, 1.0f));
  ImGui::SetNextItemWidth(fieldW);
  edited |= ImGui::DragFloat("##Y", &v[1], speed, vMin, vMax, "Y: %.2f");
  ImGui::PopStyleColor(3);

  ImGui::SameLine(0, spacing);

  // Z — Blue
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.12f, 0.45f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                        ImVec4(0.15f, 0.15f, 0.55f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                        ImVec4(0.18f, 0.18f, 0.65f, 1.0f));
  ImGui::SetNextItemWidth(fieldW);
  edited |= ImGui::DragFloat("##Z", &v[2], speed, vMin, vMax, "Z: %.2f");
  ImGui::PopStyleColor(3);

  ImGui::SameLine(0, spacing);
  ImGui::TextDisabled("%s", label);

  ImGui::PopID();
  return edited;
}

// Helper: Component header with right-click Remove + Reset button
static bool ComponentHeader(const char *label, bool *open, bool canRemove,
                            bool *wantsRemove, bool *wantsReset,
                            ImGuiTreeNodeFlags extraFlags = 0) {
  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen |
                             ImGuiTreeNodeFlags_Framed |
                             ImGuiTreeNodeFlags_AllowOverlap | extraFlags;

  // Reserve space for reset button
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.18f, 0.22f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                        ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
  *open = ImGui::CollapsingHeader(label, flags);
  ImGui::PopStyleColor(2);

  // Reset button on the same line
  ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
  ImGui::PushID(label);
  if (ImGui::SmallButton("R")) {
    *wantsReset = true;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Reset to defaults");
  ImGui::PopID();

  // Right-click context menu
  if (canRemove && ImGui::BeginPopupContextItem(label)) {
    if (ImGui::MenuItem("Remove Component")) {
      *wantsRemove = true;
    }
    ImGui::EndPopup();
  }

  return *open;
}

bool EditorUI::drawInspector(EditorContext &ctx) {
  bool edited = false;
  ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove;
  if (ImGui::Begin("Inspector", &mShowInspector, wf)) {
    uint32_t selectedEntityId = ctx.selection.selectedEntityId;
    auto &reg = ctx.scene.registry();
    auto &s = ctx.selection;

    if (selectedEntityId == 0) {
      if (!s.selectedEntities.empty() && s.selectedEntities[0] == 0) {
        ImGui::Text("Sun Selected");
        if (ImGui::Button("Open Environment Panel")) {
          mShowEnvironment = true;
        }
      } else {
        ImGui::TextDisabled("No entity selected.");
      }
    } else {
      // ── Entity Header ──────────────────────────────────────────────────
      ImGui::Text("Entity %u", selectedEntityId);
      ImGui::SameLine();
      if (ImGui::Button("Rename")) {
        s.renaming = true;
        std::string name = "Entity " + std::to_string(selectedEntityId);
        if (reg.has<NameComponent>(selectedEntityId))
          name = reg.get<NameComponent>(selectedEntityId).name;
        std::snprintf(s.renameBuf, 128, "%s", name.c_str());
      }
      ImGui::SameLine();
      if (ImGui::Button("Delete")) {
        ctx.events.publish(DeleteEntityRequestedEvent{selectedEntityId});
      }

      ImGui::Separator();

      // ── Name ───────────────────────────────────────────────────────────
      if (reg.has<NameComponent>(selectedEntityId)) {
        auto &nc = reg.get<NameComponent>(selectedEntityId);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", nc.name.c_str());
        if (ImGui::InputText("Name", buf, sizeof(buf))) {
          nc.name = buf;
          edited = true;
        }
      }

      // ── Transform ──────────────────────────────────────────────────────
      if (reg.has<TransformComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Transform", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<TransformComponent>(selectedEntityId);
        } else {
          if (wantReset) {
            reg.get<TransformComponent>(selectedEntityId) =
                TransformComponent{};
            edited = true;
          }
          if (open) {
            auto &tr = reg.get<TransformComponent>(selectedEntityId);
            edited |= DragFloat3Colored("Position", &tr.position.x, 0.1f);
            edited |= DragFloat3Colored("Rotation", &tr.rotation.x, 0.5f);
            edited |=
                DragFloat3Colored("Scale", &tr.scale.x, 0.01f, 0.01f, 100.0f);
          }
        }
      }

      // ── Mesh ───────────────────────────────────────────────────────────
      if (reg.has<MeshComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Mesh", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<MeshComponent>(selectedEntityId);
        } else if (open) {
          auto &mc = reg.get<MeshComponent>(selectedEntityId);
          ImGui::Checkbox("Visible", &mc.visible);
          ImGui::Checkbox("Casts Shadow", &mc.castsShadow);

          if (mc.type == MeshComponent::AssetType::OBJ && mc.objModel) {
            ImGui::Text("Model: OBJ (%zu submeshes)",
                        mc.objModel->submeshCount());
            if (ImGui::TreeNode("Submeshes")) {
              ImGui::Checkbox("Edit selected part", &s.editObjPart);
              auto names = mc.objModel->objectNames();
              if (names.empty()) {
                ImGui::Text("No named parts.");
              } else {
                if (ImGui::BeginCombo("Select Part",
                                      s.selectedObjPartName.c_str())) {
                  for (const auto &n : names) {
                    bool is_selected = (s.selectedObjPartName == n);
                    if (ImGui::Selectable(n.c_str(), is_selected))
                      s.selectedObjPartName = n;
                    if (is_selected)
                      ImGui::SetItemDefaultFocus();
                  }
                  ImGui::EndCombo();
                }
              }
              ImGui::TreePop();
            }
          } else if (mc.type == MeshComponent::AssetType::GLTF &&
                     mc.gltfModel) {
            ImGui::Text("Model: GLTF (%zu submeshes)",
                        mc.gltfModel->submeshCount());
          } else if (mc.type == MeshComponent::AssetType::FBX && mc.ufbxModel) {
            ImGui::Text("Model: True FBX (%zu submeshes)",
                        mc.ufbxModel->submeshCount());
          } else {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Model: null");
          }
        }
      }

      // ── Physics ────────────────────────────────────────────────────────
      if (reg.has<PhysicsComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Physics", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<PhysicsComponent>(selectedEntityId);
        } else {
          if (wantReset) {
            reg.get<PhysicsComponent>(selectedEntityId) = PhysicsComponent{};
            edited = true;
          }
          if (open) {
            auto &ph = reg.get<PhysicsComponent>(selectedEntityId);
            edited |= DragFloat3Colored("Velocity", &ph.velocity.x, 0.01f);
            ImGui::DragFloat("Gravity##Phys", &ph.gravity, 0.001f, 0.0f, 0.1f);
            ImGui::Checkbox("On Ground", &ph.onGround);
          }
        }
      }

      // ── Bounds ─────────────────────────────────────────────────────────
      if (reg.has<BoundsComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Bounds", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<BoundsComponent>(selectedEntityId);
        } else {
          if (wantReset) {
            reg.get<BoundsComponent>(selectedEntityId) = BoundsComponent{};
            edited = true;
          }
          if (open) {
            auto &bc = reg.get<BoundsComponent>(selectedEntityId);
            edited |= ImGui::DragFloat("Radius##Bounds", &bc.radius, 0.1f,
                                       0.01f, 1000.0f);
          }
        }
      }

      // ── LOD ────────────────────────────────────────────────────────────
      if (reg.has<LODComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("LOD", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<LODComponent>(selectedEntityId);
        } else {
          if (wantReset) {
            reg.get<LODComponent>(selectedEntityId) = LODComponent{};
            edited = true;
          }
          if (open) {
            auto &lod = reg.get<LODComponent>(selectedEntityId);
            edited |= ImGui::DragFloat("Min Distance", &lod.minDistance, 1.0f,
                                       0.0f, lod.maxDistance);
            edited |= ImGui::DragFloat("Max Distance", &lod.maxDistance, 1.0f,
                                       lod.minDistance, 100000.0f);
          }
        }
      }

      // ── Lifecycle ──────────────────────────────────────────────────────
      if (reg.has<LifecycleComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Lifecycle", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<LifecycleComponent>(selectedEntityId);
        } else {
          if (wantReset) {
            reg.get<LifecycleComponent>(selectedEntityId) =
                LifecycleComponent{};
            edited = true;
          }
          if (open) {
            auto &lc = reg.get<LifecycleComponent>(selectedEntityId);
            const char *states[] = {"Alive", "Disabled", "PendingDestroy"};
            int stateIdx = static_cast<int>(lc.state);
            if (ImGui::Combo("State##LC", &stateIdx, states, 3)) {
              lc.state = static_cast<EntityLifecycleState>(stateIdx);
              edited = true;
            }
          }
        }
      }

      // ── Hierarchy ──────────────────────────────────────────────────────
      if (reg.has<HierarchyComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Hierarchy", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<HierarchyComponent>(selectedEntityId);
        } else {
          if (open) {
            auto &h = reg.get<HierarchyComponent>(selectedEntityId);
            int parent = static_cast<int>(h.parent);
            if (ImGui::DragInt("Parent ID", &parent, 1, 0, 100000)) {
              h.parent = static_cast<uint32_t>(parent);
              edited = true;
            }
            ImGui::Text("Children: %d", (int)h.children.size());
            if (!h.children.empty() && ImGui::TreeNode("Children List")) {
              for (uint32_t child : h.children) {
                ImGui::BulletText("Entity %u", child);
              }
              ImGui::TreePop();
            }
          }
        }
      }

      // ── Camera ─────────────────────────────────────────────────────────
      if (reg.has<CameraComponent>(selectedEntityId)) {
        bool open = false, wantRemove = false, wantReset = false;
        ComponentHeader("Camera", &open, true, &wantRemove, &wantReset);
        if (wantRemove) {
          reg.removeComponent<CameraComponent>(selectedEntityId);
        } else {
          if (wantReset) {
            reg.get<CameraComponent>(selectedEntityId) = CameraComponent{};
            edited = true;
          }
          if (open) {
            auto &cam = reg.get<CameraComponent>(selectedEntityId);
            edited |=
                ImGui::DragFloat("FOV##Cam", &cam.fov, 0.5f, 10.0f, 170.0f);
            edited |= ImGui::DragFloat("Yaw##Cam", &cam.yaw, 0.5f);
            edited |=
                ImGui::DragFloat("Pitch##Cam", &cam.pitch, 0.5f, -89.0f, 89.0f);
            ImGui::Checkbox("Is Primary", &cam.isPrimary);
            ImGui::Text("Front: (%.2f, %.2f, %.2f)", cam.front.x, cam.front.y,
                        cam.front.z);
          }
        }
      }

      // ── Add Component Button ───────────────────────────────────────────
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      float buttonWidth = ImGui::GetContentRegionAvail().x;
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.15f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            ImVec4(0.20f, 0.45f, 0.20f, 1.0f));
      if (ImGui::Button("+ Add Component", ImVec2(buttonWidth, 28))) {
        ImGui::OpenPopup("AddComponentPopup");
      }
      ImGui::PopStyleColor(2);

      if (ImGui::BeginPopup("AddComponentPopup")) {
        if (!reg.has<TransformComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Transform"))
            reg.emplace<TransformComponent>(selectedEntityId);
        }
        if (!reg.has<MeshComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Mesh"))
            reg.emplace<MeshComponent>(selectedEntityId);
        }
        if (!reg.has<PhysicsComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Physics"))
            reg.emplace<PhysicsComponent>(selectedEntityId);
        }
        if (!reg.has<BoundsComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Bounds"))
            reg.emplace<BoundsComponent>(selectedEntityId);
        }
        if (!reg.has<LODComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("LOD"))
            reg.emplace<LODComponent>(selectedEntityId);
        }
        if (!reg.has<LifecycleComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Lifecycle"))
            reg.emplace<LifecycleComponent>(selectedEntityId);
        }
        if (!reg.has<HierarchyComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Hierarchy"))
            reg.emplace<HierarchyComponent>(selectedEntityId);
        }
        if (!reg.has<CameraComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Camera"))
            reg.emplace<CameraComponent>(selectedEntityId);
        }
        if (!reg.has<NameComponent>(selectedEntityId)) {
          if (ImGui::MenuItem("Name"))
            reg.emplace<NameComponent>(selectedEntityId, "Unnamed");
        }
        ImGui::EndPopup();
      }
    }
  }
  ImGui::End();

  // Rename Popup Logic
  if (ctx.selection.renaming && ctx.selection.selectedEntityId != 0) {
    ImGui::OpenPopup("RenameEntityPopup");
  }

  if (ImGui::BeginPopupModal("RenameEntityPopup", &ctx.selection.renaming,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::InputText("Name", ctx.selection.renameBuf, 128);
    if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
      if (ctx.scene.registry().has<NameComponent>(
              ctx.selection.selectedEntityId)) {
        ctx.scene.registry()
            .get<NameComponent>(ctx.selection.selectedEntityId)
            .name = ctx.selection.renameBuf;
        edited = true;
      }
      ctx.selection.renaming = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      ctx.selection.renaming = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
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

  // Gizmo Logic Helper
  bool hasPrimaryEntity = (s.selectedEntityId != 0 &&
                           reg.has<TransformComponent>(s.selectedEntityId));

  bool sunSelected = (s.selectedEntityId == 0 && !s.selectedEntities.empty() &&
                      s.selectedEntities[0] == 0);

  OBJModel *modelPtr = nullptr;
  if (hasPrimaryEntity && reg.has<MeshComponent>(s.selectedEntityId)) {
    modelPtr = reg.get<MeshComponent>(s.selectedEntityId).objModel;
  }

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
