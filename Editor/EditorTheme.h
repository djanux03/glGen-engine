#pragma once
#include "imgui.h"

// =============================================================================
// glGen Engine — AAA Dark Theme
// =============================================================================
// Inspired by Unreal/Godot dark themes. Call applyAATheme() once during init.
// =============================================================================

namespace EditorTheme {

// ── Color Palette ────────────────────────────────────────────────────────────
inline constexpr ImVec4 kBgDarkest = {0.086f, 0.086f, 0.094f, 1.0f}; // #16161a
inline constexpr ImVec4 kBgDark = {0.110f, 0.110f, 0.125f, 1.0f};    // #1c1c20
inline constexpr ImVec4 kBgPanel = {0.145f, 0.145f, 0.165f, 1.0f};   // #25252a
inline constexpr ImVec4 kBgChild = {0.160f, 0.160f, 0.180f, 1.0f};   // #29292e
inline constexpr ImVec4 kBgPopup = {0.125f, 0.125f, 0.145f, 1.0f};   // #202025

inline constexpr ImVec4 kBorder = {0.220f, 0.220f, 0.260f, 0.60f};
inline constexpr ImVec4 kBorderLight = {0.300f, 0.300f, 0.350f, 0.40f};

inline constexpr ImVec4 kText = {0.900f, 0.900f, 0.920f, 1.0f};
inline constexpr ImVec4 kTextDim = {0.550f, 0.550f, 0.600f, 1.0f};
inline constexpr ImVec4 kTextDisabled = {0.380f, 0.380f, 0.420f, 1.0f};

// Accent: Soft blue (like Unreal Engine)
inline constexpr ImVec4 kAccent = {0.260f, 0.590f, 0.980f, 1.0f}; // #4297fa
inline constexpr ImVec4 kAccentHover = {0.340f, 0.660f, 1.000f,
                                        1.0f}; // #57a8ff
inline constexpr ImVec4 kAccentActive = {0.200f, 0.480f, 0.850f,
                                         1.0f}; // #337ad9
inline constexpr ImVec4 kAccentDim = {0.200f, 0.400f, 0.700f, 0.30f};

// Header / Collapsable
inline constexpr ImVec4 kHeader = {0.180f, 0.180f, 0.210f, 1.0f};
inline constexpr ImVec4 kHeaderHover = {0.240f, 0.240f, 0.280f, 1.0f};
inline constexpr ImVec4 kHeaderActive = {0.200f, 0.200f, 0.250f, 1.0f};

// Tab
inline constexpr ImVec4 kTab = {0.130f, 0.130f, 0.150f, 1.0f};
inline constexpr ImVec4 kTabHover = {0.220f, 0.220f, 0.260f, 1.0f};
inline constexpr ImVec4 kTabActive = {0.180f, 0.180f, 0.210f, 1.0f};
inline constexpr ImVec4 kTabUnfocused = {0.100f, 0.100f, 0.120f, 1.0f};

// Scrollbar
inline constexpr ImVec4 kScrollbar = {0.160f, 0.160f, 0.180f, 1.0f};
inline constexpr ImVec4 kScrollGrab = {0.300f, 0.300f, 0.340f, 1.0f};
inline constexpr ImVec4 kScrollHover = {0.380f, 0.380f, 0.430f, 1.0f};

// Semantic colors
inline constexpr ImVec4 kSuccess = {0.300f, 0.780f, 0.400f, 1.0f}; // Green
inline constexpr ImVec4 kWarning = {0.950f, 0.780f, 0.200f, 1.0f}; // Yellow
inline constexpr ImVec4 kError = {0.920f, 0.300f, 0.300f, 1.0f};   // Red
inline constexpr ImVec4 kInfo = {0.400f, 0.700f, 0.950f, 1.0f};    // Light blue

// XYZ axis colors (for transform gizmos & property editors)
inline constexpr ImVec4 kAxisX = {0.920f, 0.250f, 0.300f, 1.0f}; // Red
inline constexpr ImVec4 kAxisY = {0.350f, 0.800f, 0.300f, 1.0f}; // Green
inline constexpr ImVec4 kAxisZ = {0.250f, 0.500f, 0.950f, 1.0f}; // Blue

// ── Theme Application ────────────────────────────────────────────────────────
inline void applyAATheme() {
  ImGuiStyle &style = ImGui::GetStyle();

  // ── Sizing & Rounding ─────────────────────────────────────────────────
  style.WindowPadding = ImVec2(10, 10);
  style.FramePadding = ImVec2(8, 4);
  style.CellPadding = ImVec2(6, 3);
  style.ItemSpacing = ImVec2(8, 5);
  style.ItemInnerSpacing = ImVec2(6, 4);
  style.IndentSpacing = 18.0f;
  style.ScrollbarSize = 13.0f;
  style.GrabMinSize = 8.0f;

  style.WindowBorderSize = 1.0f;
  style.ChildBorderSize = 1.0f;
  style.PopupBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
  style.TabBorderSize = 0.0f;

  style.WindowRounding = 4.0f;
  style.ChildRounding = 3.0f;
  style.FrameRounding = 3.0f;
  style.PopupRounding = 4.0f;
  style.ScrollbarRounding = 6.0f;
  style.GrabRounding = 2.0f;
  style.TabRounding = 4.0f;

  style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
  style.WindowMenuButtonPosition = ImGuiDir_None; // No collapse button

  // ── Colors ────────────────────────────────────────────────────────────
  ImVec4 *c = style.Colors;

  c[ImGuiCol_Text] = kText;
  c[ImGuiCol_TextDisabled] = kTextDisabled;
  c[ImGuiCol_WindowBg] = kBgPanel;
  c[ImGuiCol_ChildBg] = kBgChild;
  c[ImGuiCol_PopupBg] = kBgPopup;
  c[ImGuiCol_Border] = kBorder;
  c[ImGuiCol_BorderShadow] = {0, 0, 0, 0};

  c[ImGuiCol_FrameBg] = {0.180f, 0.180f, 0.200f, 1.0f};
  c[ImGuiCol_FrameBgHovered] = {0.220f, 0.220f, 0.250f, 1.0f};
  c[ImGuiCol_FrameBgActive] = {0.250f, 0.250f, 0.280f, 1.0f};

  c[ImGuiCol_TitleBg] = kBgDarkest;
  c[ImGuiCol_TitleBgActive] = kBgDark;
  c[ImGuiCol_TitleBgCollapsed] = kBgDarkest;

  c[ImGuiCol_MenuBarBg] = kBgDark;
  c[ImGuiCol_ScrollbarBg] = kScrollbar;
  c[ImGuiCol_ScrollbarGrab] = kScrollGrab;
  c[ImGuiCol_ScrollbarGrabHovered] = kScrollHover;
  c[ImGuiCol_ScrollbarGrabActive] = kAccent;

  c[ImGuiCol_CheckMark] = kAccent;
  c[ImGuiCol_SliderGrab] = kAccent;
  c[ImGuiCol_SliderGrabActive] = kAccentActive;

  c[ImGuiCol_Button] = {0.200f, 0.200f, 0.230f, 1.0f};
  c[ImGuiCol_ButtonHovered] = kAccentHover;
  c[ImGuiCol_ButtonActive] = kAccentActive;

  c[ImGuiCol_Header] = kHeader;
  c[ImGuiCol_HeaderHovered] = kHeaderHover;
  c[ImGuiCol_HeaderActive] = kHeaderActive;

  c[ImGuiCol_Separator] = kBorder;
  c[ImGuiCol_SeparatorHovered] = kAccent;
  c[ImGuiCol_SeparatorActive] = kAccentActive;

  c[ImGuiCol_ResizeGrip] = {0.26f, 0.59f, 0.98f, 0.15f};
  c[ImGuiCol_ResizeGripHovered] = {0.26f, 0.59f, 0.98f, 0.50f};
  c[ImGuiCol_ResizeGripActive] = {0.26f, 0.59f, 0.98f, 0.85f};

  c[ImGuiCol_Tab] = kTab;
  c[ImGuiCol_TabHovered] = kTabHover;
  c[ImGuiCol_TabActive] = kTabActive;
  c[ImGuiCol_TabUnfocused] = kTabUnfocused;
  c[ImGuiCol_TabUnfocusedActive] = kTab;

#ifdef IMGUI_HAS_DOCK
  c[ImGuiCol_DockingPreview] = {kAccent.x, kAccent.y, kAccent.z, 0.5f};
  c[ImGuiCol_DockingEmptyBg] = kBgDarkest;
#endif

  c[ImGuiCol_PlotLines] = kAccent;
  c[ImGuiCol_PlotLinesHovered] = kAccentHover;
  c[ImGuiCol_PlotHistogram] = kAccent;
  c[ImGuiCol_PlotHistogramHovered] = kAccentHover;

  c[ImGuiCol_TableHeaderBg] = kHeader;
  c[ImGuiCol_TableBorderStrong] = kBorder;
  c[ImGuiCol_TableBorderLight] = kBorderLight;
  c[ImGuiCol_TableRowBg] = {0, 0, 0, 0};
  c[ImGuiCol_TableRowBgAlt] = {1, 1, 1, 0.02f};

  c[ImGuiCol_TextSelectedBg] = kAccentDim;
  c[ImGuiCol_DragDropTarget] = kAccentHover;
  c[ImGuiCol_NavHighlight] = kAccent;
  c[ImGuiCol_NavWindowingHighlight] = {1, 1, 1, 0.70f};
  c[ImGuiCol_NavWindowingDimBg] = {0.80f, 0.80f, 0.80f, 0.20f};
  c[ImGuiCol_ModalWindowDimBg] = {0, 0, 0, 0.60f};
}

} // namespace EditorTheme
