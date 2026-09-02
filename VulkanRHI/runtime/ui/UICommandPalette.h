#pragma once

#include "UITheme.h"
#include "UIWidgets.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <vector>

// =============================================================================
// Command palette (Ctrl+P)
// =============================================================================
// The old Environment panel grew a search box (EnvRowFilter) because it had
// accumulated ~150 controls across eight tabs and they could no longer be
// found by looking. That filter only searched that one panel.
//
// This replaces it with one search over everything -- actions, panel jumps
// and settings alike. It is what makes it safe to demote rarely-used debug
// toggles out of permanent panel space: nothing becomes unreachable, it just
// stops occupying pixels by default.
// =============================================================================

namespace UI {

struct Command {
  const char *icon = nullptr;
  std::string group;    // "World", "Debug", "File" -- shown dimmed
  std::string title;    // what the user is looking for
  std::string shortcut; // optional, right-aligned
  std::function<void()> run;
};

class CommandPalette {
public:
  void clear() { mCommands.clear(); }

  void add(const char *icon, const std::string &group, const std::string &title,
           std::function<void()> run, const std::string &shortcut = "") {
    mCommands.push_back({icon, group, title, shortcut, std::move(run)});
  }

  void open() {
    mOpen = true;
    mQuery[0] = '\0';
    mSelected = 0;
    mFocusNext = true;
  }

  void close() { mOpen = false; }
  bool isOpen() const { return mOpen; }

  // Scores a subsequence match. Consecutive characters and matches at word
  // starts score higher, so "sui" ranks "Sun Intensity" above an incidental
  // scattering of those letters. Returns -1 when `q` is not a subsequence.
  static int score(const std::string &haystack, const char *q) {
    if (!q || !*q)
      return 0;
    int s = 0, run = 0;
    size_t hi = 0;
    for (const char *p = q; *p; ++p) {
      const char qc = (char)std::tolower((unsigned char)*p);
      bool found = false;
      while (hi < haystack.size()) {
        const char hc = (char)std::tolower((unsigned char)haystack[hi]);
        const bool wordStart =
            hi == 0 || haystack[hi - 1] == ' ' || haystack[hi - 1] == '>';
        ++hi;
        if (hc == qc) {
          s += 1 + run * 3 + (wordStart ? 6 : 0);
          run++;
          found = true;
          break;
        }
        run = 0;
      }
      if (!found)
        return -1;
    }
    return s;
  }

  // Draws the overlay when open. Call once per frame, late, so it paints
  // over the rest of the editor.
  void draw() {
    if (!mOpen)
      return;

    // Rank matches.
    struct Hit {
      int score;
      int index;
    };
    std::vector<Hit> hits;
    hits.reserve(mCommands.size());
    for (int i = 0; i < (int)mCommands.size(); ++i) {
      const std::string full = mCommands[i].group + " > " + mCommands[i].title;
      const int s = score(full, mQuery);
      if (s >= 0)
        hits.push_back({s, i});
    }
    std::stable_sort(hits.begin(), hits.end(),
                     [](const Hit &a, const Hit &b) { return a.score > b.score; });
    if (mSelected >= (int)hits.size())
      mSelected = hits.empty() ? 0 : (int)hits.size() - 1;
    if (mSelected < 0)
      mSelected = 0;

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float w = std::min(620.0f, vp->WorkSize.x - 80.0f);
    const float rowH = ImGui::GetFrameHeight() + UITheme::kSpace1;
    const int visible = std::min((int)hits.size(), 9);
    // Height must account for the window's own padding (Begin() lays content
    // out inside it) or the last row is clipped off the bottom.
    const float listH = visible > 0 ? visible * rowH + UITheme::kSpace2 : 0.0f;
    const float inputH = ImGui::GetFrameHeight() + UITheme::kSpace2 * 2.0f;
    const float h = UITheme::kSpace2 * 2.0f + inputH + listH;

    // Dim the editor behind it.
    ImGui::GetForegroundDrawList()->AddRectFilled(
        vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
        ImGui::GetColorU32(ImVec4(0.02f, 0.02f, 0.04f, 0.55f)));

    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + (vp->WorkSize.x - w) * 0.5f,
               vp->WorkPos.y + vp->WorkSize.y * 0.18f));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(UITheme::kSpace2, UITheme::kSpace2));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, UITheme::kBg2);
    ImGui::PushStyleColor(ImGuiCol_Border, UITheme::kLine);

    const ImGuiWindowFlags f =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##palette", nullptr, f)) {
      if (mFocusNext) {
        ImGui::SetKeyboardFocusHere();
        mFocusNext = false;
      }
      ImGui::SetNextItemWidth(-FLT_MIN);
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                          ImVec2(UITheme::kSpace3, UITheme::kSpace2));
      ImGui::PushStyleColor(ImGuiCol_FrameBg, UITheme::kBg1);
      ImGui::InputTextWithHint("##q", "Search commands and settings...", mQuery,
                               sizeof(mQuery));
      ImGui::PopStyleColor();
      ImGui::PopStyleVar();

      const bool inputActive = ImGui::IsItemActive();
      (void)inputActive;

      if (!hits.empty()) {
        ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace1));
        ImDrawList *dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < visible; ++i) {
          const Command &c = mCommands[hits[i].index];
          const bool sel = (i == mSelected);
          const ImVec2 p = ImGui::GetCursorScreenPos();
          const float rw = ImGui::GetContentRegionAvail().x;

          ImGui::PushID(i);
          if (ImGui::InvisibleButton("##row", ImVec2(rw, rowH)))
            fire(c);
          if (ImGui::IsItemHovered())
            mSelected = i;
          ImGui::PopID();

          if (sel)
            dl->AddRectFilled(p, ImVec2(p.x + rw, p.y + rowH),
                              ImGui::GetColorU32(UITheme::kAccentDim), 6.0f);

          const float ty = p.y + (rowH - ImGui::GetFontSize()) * 0.5f;
          float x = p.x + UITheme::kSpace2;
          if (c.icon && *c.icon) {
            dl->AddText(ImVec2(x, ty),
                        ImGui::GetColorU32(sel ? UITheme::kAccentHover
                                               : UITheme::kTextFaint),
                        c.icon);
          }
          x += 22.0f;
          if (!c.group.empty()) {
            const std::string g = c.group + "  ";
            dl->AddText(ImVec2(x, ty), ImGui::GetColorU32(UITheme::kTextFaint),
                        g.c_str());
            x += ImGui::CalcTextSize(g.c_str()).x;
          }
          dl->AddText(ImVec2(x, ty), ImGui::GetColorU32(UITheme::kText),
                      c.title.c_str());
          if (!c.shortcut.empty()) {
            const float sw = ImGui::CalcTextSize(c.shortcut.c_str()).x;
            dl->AddText(ImVec2(p.x + rw - sw - UITheme::kSpace2, ty),
                        ImGui::GetColorU32(UITheme::kTextFaint),
                        c.shortcut.c_str());
          }
        }
      } else {
        ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace2));
        UI::TextFaint("   No matching command.");
      }

      // Keyboard: arrows move, Enter runs, Escape dismisses.
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
        mSelected = hits.empty() ? 0 : (mSelected + 1) % (int)hits.size();
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
        mSelected = hits.empty()
                        ? 0
                        : (mSelected + (int)hits.size() - 1) % (int)hits.size();
      if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !hits.empty() &&
          mSelected < (int)hits.size())
        fire(mCommands[hits[mSelected].index]);
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        mOpen = false;
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
  }

private:
  // Takes the callable by value: a command is free to re-register the
  // palette's contents (or clear it), which would otherwise destroy the
  // std::function out from under the call in progress.
  void fire(const Command &c) {
    mOpen = false;
    auto fn = c.run;
    if (fn)
      fn();
  }

  std::vector<Command> mCommands;
  char mQuery[128] = "";
  bool mOpen = false;
  bool mFocusNext = false;
  int mSelected = 0;
};

} // namespace UI
