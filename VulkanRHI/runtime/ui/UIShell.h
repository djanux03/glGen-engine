#pragma once

#include "UITheme.h"
#include "UIWidgets.h"
#include "imgui.h"

#include <algorithm>

// =============================================================================
// UIShell — the editor frame
// =============================================================================
// The old editor drew seven independent ImGui windows at hand-computed
// positions and let imgui_glgenvk.ini remember wherever the user dragged
// them. Windows overlapped the 3D view, drifted out of alignment, and
// "Reset Layout" existed because the layout could not be trusted.
//
// This replaces all of that with one fixed frame:
//
//   +--------------------------------------------------+
//   | menu bar                                         |
//   | toolbar:  tools | transport |            search  |
//   +--------+--------------------------+--------------+
//   | scene  |         VIEWPORT         |  properties  |
//   |  rail  |    (the 3D scene owns    |     rail     |
//   |        |     this rectangle)      |              |
//   +--------+--------------------------+--------------+
//   | drawer (console / assets) - collapsed by default |
//   +--------------------------------------------------+
//
// Regions never overlap, so the viewport rect is exactly the area the scene
// is visible in. Rails are resizable via splitters and collapsible; their
// sizes live in State (persisted by the editor's own settings file, not by
// imgui.ini) so the layout is reproducible.
// =============================================================================

namespace UIShell {

struct State {
  float leftW = UITheme::kLeftRailW;
  float rightW = UITheme::kRightRailW;
  float drawerH = UITheme::kDrawerH;
  bool leftOpen = true;
  bool rightOpen = true;
  bool drawerOpen = false;
  int drawerTab = 0; // 0 = Console, 1 = Assets
};

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  ImVec2 pos() const { return ImVec2(x, y); }
  ImVec2 size() const { return ImVec2(w, h); }
  bool contains(const ImVec2 &p) const {
    return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
  }
};

struct Layout {
  Rect toolbar;
  Rect left;
  Rect right;
  Rect drawer;
  Rect viewport; // what the 3D scene actually occupies
  Rect leftSplit;
  Rect rightSplit;
  Rect drawerSplit;
};

// Divides the main viewport into non-overlapping regions.
inline Layout compute(const State &s) {
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  const float x0 = vp->WorkPos.x;
  const float y0 = vp->WorkPos.y;
  const float W = vp->WorkSize.x;
  const float H = vp->WorkSize.y;

  Layout L;
  L.toolbar = {x0, y0, W, UITheme::kToolbarH};

  const float bodyY = y0 + UITheme::kToolbarH;
  const float drawerH = s.drawerOpen ? std::min(s.drawerH, H * 0.6f) : 0.0f;
  const float bodyH = std::max(120.0f, H - UITheme::kToolbarH - drawerH);

  const float lw = s.leftOpen ? s.leftW : 0.0f;
  const float rw = s.rightOpen ? s.rightW : 0.0f;
  const float sp = UITheme::kSplitterW;

  L.left = {x0, bodyY, lw, bodyH};
  if (s.leftOpen)
    L.leftSplit = {x0 + lw, bodyY, sp, bodyH};

  const float rightX = x0 + W - rw;
  L.right = {rightX, bodyY, rw, bodyH};
  if (s.rightOpen)
    L.rightSplit = {rightX - sp, bodyY, sp, bodyH};

  const float vx = x0 + lw + (s.leftOpen ? sp : 0.0f);
  const float vw = std::max(80.0f, (rightX - (s.rightOpen ? sp : 0.0f)) - vx);
  L.viewport = {vx, bodyY, vw, bodyH};

  if (s.drawerOpen) {
    L.drawerSplit = {x0, bodyY + bodyH, W, sp};
    L.drawer = {x0, bodyY + bodyH + sp, W, std::max(0.0f, drawerH - sp)};
  }
  return L;
}

// A chrome region: fixed, borderless, non-interactive to window dragging.
// Everything the shell draws goes inside one of these.
inline bool BeginRegion(const char *name, const Rect &r, ImVec4 bg,
                        bool padded = true) {
  ImGui::SetNextWindowPos(r.pos());
  ImGui::SetNextWindowSize(r.size());
  // WindowMinSize defaults to 32x32 and ImGui silently clamps to it. The
  // splitters are 4px strips, so without this they render 32px wide and
  // paint their background over the edge of the rail beside them -- which
  // looks exactly like text being clipped.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
  if (!padded)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  const ImGuiWindowFlags f =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
      ImGuiWindowFlags_NoSavedSettings;
  const bool open = ImGui::Begin(name, nullptr, f);
  if (!open) {
    ImGui::End();
    if (!padded)
      ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(); // WindowMinSize
  }
  return open;
}

inline void EndRegion(bool padded = true) {
  ImGui::End();
  if (!padded)
    ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(); // WindowMinSize
}

// Panel title bar drawn inside a region. Leaves the cursor on the same line
// so callers can right-align their own affordances after it.
inline void PanelHeader(const char *icon, const char *title) {
  if (icon && *icon) {
    ImGui::PushStyleColor(ImGuiCol_Text, UITheme::kTextFaint);
    ImGui::TextUnformatted(icon);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, UITheme::kSpace2);
  }
  UIFonts::Scoped f(UIFonts::get().title);
  ImGui::TextUnformatted(title);
}

// Thin separator line spanning the current content width.
inline void Rule(float alphaMul = 1.0f) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y),
              ImGui::GetColorU32(
                  UITheme::withAlpha(UITheme::kLine, alphaMul)),
              1.0f);
  ImGui::Dummy(ImVec2(0.0f, 1.0f));
}

} // namespace UIShell
