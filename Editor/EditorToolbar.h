#pragma once

#include "EditorTheme.h"
#include "imgui.h"
#include <cstdint>

// Forward declarations
class EventBus;

// =============================================================================
// Toolbar output — communicates toolbar actions back to the main editor
// =============================================================================
struct ToolbarState {
  // Gizmo operation
  enum GizmoOp : int { Translate = 0, Rotate = 1, Scale = 2 };
  GizmoOp gizmoOp = Translate;

  // Gizmo space
  bool worldSpace = false; // false = local, true = world

  // Snap
  bool snapEnabled = false;
  float snapValue = 1.0f;

  // Viewport shading
  enum ShadingMode : int { Textured = 0, Solid = 1, Wireframe = 2 };
  ShadingMode shadingMode = Textured;

  // Camera speed
  float cameraSpeed = 5.0f;
};

// =============================================================================
// EditorToolbar — Horizontal toolbar below the menu bar
// =============================================================================
namespace EditorToolbar {

// Helper: icon-style toggle button
inline bool ToolButton(const char *label, bool active, float width = 0) {
  if (active) {
    ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
  } else {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.20f, 0.23f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.28f, 0.28f, 0.32f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.70f, 0.75f, 1.0f));
  }
  bool pressed = false;
  if (width > 0) {
    pressed = ImGui::Button(label, ImVec2(width, 0));
  } else {
    pressed = ImGui::Button(label);
  }
  ImGui::PopStyleColor(3);
  return pressed;
}

// Main draw function — renders the toolbar
inline void draw(ToolbarState &state) {
  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove;

  // Get viewport work area (below menu bar)
  ImGuiViewport *vp = ImGui::GetMainViewport();
  float toolbarH = 36.0f;

  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, toolbarH));

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::kBgDark);

  if (ImGui::Begin("##Toolbar", nullptr, flags)) {

    // ── Gizmo Tools ──────────────────────────────────────────────────────
    if (ToolButton("W Move", state.gizmoOp == ToolbarState::Translate))
      state.gizmoOp = ToolbarState::Translate;
    ImGui::SameLine();
    if (ToolButton("E Scale", state.gizmoOp == ToolbarState::Scale))
      state.gizmoOp = ToolbarState::Scale;
    ImGui::SameLine();
    if (ToolButton("R Rotate", state.gizmoOp == ToolbarState::Rotate))
      state.gizmoOp = ToolbarState::Rotate;

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // ── Gizmo Space ──────────────────────────────────────────────────────
    if (ToolButton(state.worldSpace ? "World" : "Local", false))
      state.worldSpace = !state.worldSpace;

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // ── Snap ─────────────────────────────────────────────────────────────
    if (ToolButton("Snap", state.snapEnabled))
      state.snapEnabled = !state.snapEnabled;
    ImGui::SameLine();
    if (state.snapEnabled) {
      ImGui::SetNextItemWidth(60);
      ImGui::DragFloat("##SnapVal", &state.snapValue, 0.1f, 0.1f, 100.0f,
                       "%.1f");
      ImGui::SameLine();
    }

    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // ── Right side: Shading + Camera speed ───────────────────────────────
    float rightX = vp->WorkSize.x - 260;
    ImGui::SameLine(rightX);

    const char *shadingLabels[] = {"Textured", "Solid", "Wire"};
    if (ToolButton(shadingLabels[state.shadingMode], false)) {
      state.shadingMode =
          (ToolbarState::ShadingMode)((state.shadingMode + 1) % 3);
    }
    ImGui::SameLine();

    ImGui::TextDisabled("Cam:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::SliderFloat("##CamSpeed", &state.cameraSpeed, 0.5f, 50.0f, "%.1f");
  }
  ImGui::End();

  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

// Process keyboard shortcuts for gizmo switching
inline void processShortcuts(ToolbarState &state) {
  if (ImGui::GetIO().WantCaptureKeyboard)
    return;

  if (ImGui::IsKeyPressed(ImGuiKey_W, false))
    state.gizmoOp = ToolbarState::Translate;
  if (ImGui::IsKeyPressed(ImGuiKey_E, false))
    state.gizmoOp = ToolbarState::Scale;
  if (ImGui::IsKeyPressed(ImGuiKey_R, false))
    state.gizmoOp = ToolbarState::Rotate;
}

} // namespace EditorToolbar
