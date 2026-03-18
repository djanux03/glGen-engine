#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "AppState.h"
#include "EditorSubsystem.h"
#include "EditorTheme.h"
#include "Logger.h"
#include <filesystem>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

EditorSubsystem::EditorSubsystem(AppState &state) : mState(state) {}

EditorSubsystem::~EditorSubsystem() = default;

bool EditorSubsystem::initialize() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
#ifdef IMGUI_HAS_DOCK
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#endif
  io.IniFilename = "imgui.ini";

  io.Fonts->AddFontDefault();
  const std::string iconPathTtf =
      mState.projectConfig.assetPath("fonts/fa-solid-900.ttf");
  const std::string iconPathOtf =
      mState.projectConfig.assetPath("fonts/fa-solid-900.otf");
  const std::string iconPath =
      std::filesystem::exists(iconPathTtf) ? iconPathTtf : iconPathOtf;
  if (!iconPath.empty() && std::filesystem::exists(iconPath)) {
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    cfg.GlyphMinAdvanceX = 14.0f;
    static const ImWchar ranges[] = {0xf000, 0xf8ff, 0};
    if (io.Fonts->AddFontFromFileTTF(iconPath.c_str(), 14.0f, &cfg, ranges))
      mState.iconFontLoaded = true;
  } else {
    LOG_WARN("Editor",
             "Font Awesome not found: " + iconPathTtf + " or " + iconPathOtf);
  }

  EditorTheme::applyAATheme();
  ImGui_ImplGlfw_InitForOpenGL(mState.window, true);
  ImGui_ImplOpenGL3_Init("#version 330");
  return true;
}

void EditorSubsystem::shutdown() {
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
}

void EditorSubsystem::beginFrame() {
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
}

void EditorSubsystem::drawDockspace() {
#ifdef IMGUI_HAS_DOCK
  ImGuiDockNodeFlags dockspaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;
  ImGui::DockSpaceOverViewport(ImGui::GetMainViewport(), dockspaceFlags);
#endif
}

void EditorSubsystem::endFrame() {
  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
