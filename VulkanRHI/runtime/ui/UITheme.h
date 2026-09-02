#pragma once
#include "imgui.h"

// =============================================================================
// glGen editor UI — "Graphite & Iris"
// =============================================================================
// Replaces EditorTheme.h's burnt-amber-on-warm-grey look.
//
// Two rules drive the palette:
//
//  1. Neutrals are cool (a blue cast, not a warm one). The viewport renders
//     terrain, foliage and sky -- warm chrome fought that content for
//     attention. Cool graphite recedes behind it.
//
//  2. The accent is iris (violet-blue). Deliberately NOT red, green or blue:
//     those three are spoken for by the XYZ axis colors in every transform
//     row and by the gizmo itself. An accent that collided with an axis
//     color made "selected" and "the Y axis" read as the same thing.
//
// Everything is spaced on a 4px grid (kSpace*) so panels stay on rhythm
// without each call site inventing its own padding.
// =============================================================================

namespace UITheme {

// ── Neutral ramp (bg0 = furthest back, bg4 = nearest/hover) ────────────────
inline constexpr ImVec4 kBg0 = {0.039f, 0.043f, 0.055f, 1.0f}; // #0a0b0e app
inline constexpr ImVec4 kBg1 = {0.063f, 0.071f, 0.090f, 1.0f}; // #101217 rails
inline constexpr ImVec4 kBg2 = {0.086f, 0.098f, 0.133f, 1.0f}; // #161922 cards
inline constexpr ImVec4 kBg3 = {0.114f, 0.125f, 0.161f, 1.0f}; // #1d2029 inputs
inline constexpr ImVec4 kBg4 = {0.145f, 0.161f, 0.212f, 1.0f}; // #252936 hover

inline constexpr ImVec4 kLine = {0.165f, 0.184f, 0.235f, 1.0f};     // #2a2f3c
inline constexpr ImVec4 kLineSoft = {0.125f, 0.141f, 0.180f, 1.0f}; // #20242e

// ── Text ───────────────────────────────────────────────────────────────────
inline constexpr ImVec4 kText = {0.906f, 0.918f, 0.941f, 1.0f};  // #e7eaf0
inline constexpr ImVec4 kTextMuted = {0.596f, 0.627f, 0.690f, 1.0f}; // #98a0b0
inline constexpr ImVec4 kTextFaint = {0.396f, 0.427f, 0.494f, 1.0f}; // #656d7e

// ── Accent: iris ───────────────────────────────────────────────────────────
inline constexpr ImVec4 kAccent = {0.424f, 0.424f, 0.961f, 1.0f};      // #6c6cf5
inline constexpr ImVec4 kAccentHover = {0.518f, 0.518f, 1.000f, 1.0f}; // #8484ff
inline constexpr ImVec4 kAccentActive = {0.341f, 0.341f, 0.851f, 1.0f};// #5757d9
inline constexpr ImVec4 kAccentDim = {0.424f, 0.424f, 0.961f, 0.22f};
inline constexpr ImVec4 kAccentFaint = {0.424f, 0.424f, 0.961f, 0.10f};

// ── Semantic ───────────────────────────────────────────────────────────────
inline constexpr ImVec4 kSuccess = {0.243f, 0.812f, 0.557f, 1.0f}; // #3ecf8e
inline constexpr ImVec4 kWarning = {0.961f, 0.647f, 0.141f, 1.0f}; // #f5a524
inline constexpr ImVec4 kDanger = {0.949f, 0.333f, 0.353f, 1.0f};  // #f2555a
inline constexpr ImVec4 kInfo = {0.298f, 0.761f, 1.000f, 1.0f};    // #4cc2ff

// ── Viewport selection highlight ───────────────────────────────────────────
// The bracket box drawn around selected entities. Defaults to the accent so
// "selected" looks the same in the scene as it does in the panels. The old
// editor used amber (255,200,0) here -- swap these two lines for that look.
inline constexpr ImVec4 kSelection = kAccentHover;
inline constexpr ImVec4 kSelectionSecondary = {0.424f, 0.424f, 0.961f, 0.55f};

// ── XYZ axis colors (transform rows + gizmo) ───────────────────────────────
inline constexpr ImVec4 kAxisX = {0.949f, 0.333f, 0.353f, 1.0f}; // #f2555a
inline constexpr ImVec4 kAxisY = {0.478f, 0.831f, 0.302f, 1.0f}; // #7ad44d
inline constexpr ImVec4 kAxisZ = {0.298f, 0.553f, 1.000f, 1.0f}; // #4c8dff

// ── 4px spacing grid ───────────────────────────────────────────────────────
inline constexpr float kSpace1 = 4.0f;
inline constexpr float kSpace2 = 8.0f;
inline constexpr float kSpace3 = 12.0f;
inline constexpr float kSpace4 = 16.0f;
inline constexpr float kSpace6 = 24.0f;

// ── Chrome metrics (UIShell lays the frame out from these) ─────────────────
inline constexpr float kToolbarH = 40.0f;
inline constexpr float kRailMinW = 200.0f;
inline constexpr float kRailMaxW = 520.0f;
inline constexpr float kLeftRailW = 260.0f;
inline constexpr float kRightRailW = 320.0f;
inline constexpr float kDrawerH = 220.0f;
inline constexpr float kSplitterW = 4.0f;
inline constexpr float kRowHeight = 22.0f;
// Fraction of a property row given to the label; the control takes the rest.
inline constexpr float kLabelFrac = 0.42f;

inline ImVec4 withAlpha(const ImVec4 &c, float a) {
  return ImVec4(c.x, c.y, c.z, a);
}

// ── Style application ──────────────────────────────────────────────────────
inline void apply() {
  ImGuiStyle &s = ImGui::GetStyle();

  // Sizing: tighter and more regular than the old theme, all multiples of 4.
  s.WindowPadding = ImVec2(kSpace3, kSpace3);
  s.FramePadding = ImVec2(kSpace2, kSpace1 + 1.0f);
  s.CellPadding = ImVec2(kSpace2, kSpace1);
  s.ItemSpacing = ImVec2(kSpace2, kSpace1 + 2.0f);
  s.ItemInnerSpacing = ImVec2(kSpace2, kSpace1);
  s.IndentSpacing = 16.0f;
  s.ScrollbarSize = 10.0f;
  s.GrabMinSize = 10.0f;

  s.WindowBorderSize = 0.0f; // the shell draws its own separators
  s.ChildBorderSize = 1.0f;
  s.PopupBorderSize = 1.0f;
  s.FrameBorderSize = 1.0f;
  s.TabBorderSize = 0.0f;

  // Rounded, but restrained -- 4 on controls, 6 on containers.
  s.WindowRounding = 0.0f;
  s.ChildRounding = 6.0f;
  s.FrameRounding = 4.0f;
  s.PopupRounding = 8.0f;
  s.ScrollbarRounding = 8.0f;
  s.GrabRounding = 4.0f;
  s.TabRounding = 5.0f;

  s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
  s.WindowMenuButtonPosition = ImGuiDir_None;
  s.SeparatorTextBorderSize = 1.0f;
  s.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
  s.SeparatorTextPadding = ImVec2(0.0f, kSpace2);

  s.AntiAliasedLines = true;
  s.AntiAliasedLinesUseTex = true;
  s.AntiAliasedFill = true;

  ImVec4 *c = s.Colors;
  c[ImGuiCol_Text] = kText;
  c[ImGuiCol_TextDisabled] = kTextFaint;
  c[ImGuiCol_WindowBg] = kBg1;
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = kBg2;
  c[ImGuiCol_Border] = kLine;
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

  c[ImGuiCol_FrameBg] = kBg3;
  c[ImGuiCol_FrameBgHovered] = kBg4;
  c[ImGuiCol_FrameBgActive] = kBg4;

  c[ImGuiCol_TitleBg] = kBg0;
  c[ImGuiCol_TitleBgActive] = kBg0;
  c[ImGuiCol_TitleBgCollapsed] = kBg0;
  c[ImGuiCol_MenuBarBg] = kBg0;

  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = kBg4;
  c[ImGuiCol_ScrollbarGrabHovered] = kLine;
  c[ImGuiCol_ScrollbarGrabActive] = kAccent;

  c[ImGuiCol_CheckMark] = kAccent;
  c[ImGuiCol_SliderGrab] = kAccent;
  c[ImGuiCol_SliderGrabActive] = kAccentHover;

  // Buttons default to quiet; emphasis is opt-in via UIWidgets' variants.
  c[ImGuiCol_Button] = kBg3;
  c[ImGuiCol_ButtonHovered] = kBg4;
  c[ImGuiCol_ButtonActive] = kAccentActive;

  c[ImGuiCol_Header] = kAccentDim;
  c[ImGuiCol_HeaderHovered] = kAccentFaint;
  c[ImGuiCol_HeaderActive] = kAccentDim;

  c[ImGuiCol_Separator] = kLineSoft;
  c[ImGuiCol_SeparatorHovered] = kAccent;
  c[ImGuiCol_SeparatorActive] = kAccentHover;

  c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ResizeGripHovered] = kAccentDim;
  c[ImGuiCol_ResizeGripActive] = kAccent;

  c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TabHovered] = kAccentFaint;
  c[ImGuiCol_TabSelected] = kBg3;
  c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TabDimmedSelected] = kBg2;

  c[ImGuiCol_PlotLines] = kAccent;
  c[ImGuiCol_PlotLinesHovered] = kAccentHover;
  c[ImGuiCol_PlotHistogram] = kAccent;
  c[ImGuiCol_PlotHistogramHovered] = kAccentHover;

  c[ImGuiCol_TableHeaderBg] = kBg2;
  c[ImGuiCol_TableBorderStrong] = kLine;
  c[ImGuiCol_TableBorderLight] = kLineSoft;
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.016f);

  c[ImGuiCol_TextSelectedBg] = kAccentDim;
  c[ImGuiCol_DragDropTarget] = kAccentHover;
  c[ImGuiCol_NavCursor] = kAccent;
  c[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.70f);
  c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.05f, 0.05f, 0.08f, 0.40f);
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.02f, 0.02f, 0.04f, 0.70f);
}

} // namespace UITheme
