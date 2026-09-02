#pragma once

#include "imgui.h"

// =============================================================================
// Toolbar state
// =============================================================================
// The gizmo mode the transform tools operate in, shared between the toolbar
// strip (VkEditor::drawToolbar) and the gizmo itself (VkEditor::drawGizmo).
//
// This header used to also draw the toolbar: a row of text buttons ("W Move",
// "E Scale") in its own borderless window, styled from EditorTheme. That strip
// is gone -- the toolbar is now part of the editor shell (see UIShell.h), so
// only the shared state and the keyboard shortcuts live here.
// =============================================================================

struct ToolbarState {
  enum GizmoOp : int { Translate = 0, Rotate = 1, Scale = 2 };
  GizmoOp gizmoOp = Translate;

  bool worldSpace = false; // false = local, true = world

  bool snapEnabled = false;
  float snapValue = 1.0f;
};

namespace EditorToolbar {

// W / E / R, matching the toolbar's left-hand segmented control. Callers are
// responsible for skipping this while a text field has focus or the viewport
// is being mouse-looked.
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
