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
  io.Fonts->Clear();

  std::vector<std::string> mainFontCandidates = {
      mState.projectConfig.assetPath("fonts/Inter-Regular.ttf"),
      mState.projectConfig.assetPath("fonts/Inter-Medium.ttf")};

#if defined(_WIN32)
  mainFontCandidates.push_back("C:/Windows/Fonts/segoeui.ttf");
  mainFontCandidates.push_back("C:/Windows/Fonts/arial.ttf");
  mainFontCandidates.push_back("C:/Windows/Fonts/calibri.ttf");
#elif defined(__APPLE__)
  mainFontCandidates.push_back(
      "/System/Library/Fonts/Supplemental/Helvetica.ttc");
  mainFontCandidates.push_back(
      "/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
  mainFontCandidates.push_back("/Library/Fonts/Arial.ttf");
#else
  mainFontCandidates.push_back(
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
  mainFontCandidates.push_back("/usr/share/fonts/TTF/DejaVuSans.ttf");
  mainFontCandidates.push_back(
      "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf");
#endif

  ImFont *mainFont = nullptr;
  for (const std::string &fontPath : mainFontCandidates) {
    if (fontPath.empty() || !std::filesystem::exists(fontPath))
      continue;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = false;
    cfg.RasterizerMultiply = 1.1f;
    mainFont = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f, &cfg);
    if (mainFont != nullptr)
      break;
  }
  if (mainFont == nullptr) {
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = false;
    cfg.RasterizerMultiply = 1.1f;
    mainFont = io.Fonts->AddFontDefault(&cfg);
  }

  const std::string iconPathTtf =
      mState.projectConfig.assetPath("fonts/fa-solid-900.ttf");
  const std::string iconPathOtf =
      mState.projectConfig.assetPath("fonts/fa-solid-900.otf");
  const std::string iconPath =
      std::filesystem::exists(iconPathTtf) ? iconPathTtf : iconPathOtf;
  if (!iconPath.empty() && std::filesystem::exists(iconPath)) {
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = false;
    cfg.GlyphMinAdvanceX = 14.0f;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    static const ImWchar ranges[] = {0xf000, 0xf8ff, 0};
    if (io.Fonts->AddFontFromFileTTF(iconPath.c_str(), 14.0f, &cfg, ranges))
      mState.iconFontLoaded = true;
  } else {
    LOG_WARN("Editor",
             "Font Awesome not found: " + iconPathTtf + " or " + iconPathOtf);
  }

  io.FontDefault = mainFont;

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
