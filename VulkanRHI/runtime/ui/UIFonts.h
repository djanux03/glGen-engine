#pragma once

#include "UIIcons.h"
#include "imgui.h"

#include <cstdio>
#include <string>

// =============================================================================
// Editor typography
// =============================================================================
// The runtime previously loaded no font at all, so the whole editor rendered
// in ImGui's built-in 13px ProggyClean bitmap -- which is the single biggest
// reason the old UI read as unfinished.
//
// This loads a real UI typeface at several sizes and merges Font Awesome over
// each one, so ICON_* literals from UIIcons.h can be used inline in any label.
//
// Everything degrades gracefully: a missing text font falls back to ImGui's
// embedded *vector* default (1.92's AddFontDefaultVector, not the old bitmap),
// and a missing icon font simply means ICON_* literals draw nothing. Neither
// case asserts or crashes, because these files live outside the repo on the
// text-font side and the editor must still boot on a machine without them.
// =============================================================================

namespace UIFonts {

struct Set {
  ImFont *body = nullptr;  // default UI text
  ImFont *small = nullptr; // secondary/meta text, table cells
  ImFont *title = nullptr; // panel + section headings
  ImFont *mono = nullptr;  // console output, numeric readouts
};

inline Set &get() {
  static Set s;
  return s;
}

namespace detail {

inline bool exists(const char *path) {
  if (!path || !*path)
    return false;
  if (std::FILE *f = std::fopen(path, "rb")) {
    std::fclose(f);
    return true;
  }
  return false;
}

// Picks the first font file that is actually present. Returns nullptr when
// none are, which callers treat as "use the embedded default".
inline const char *firstPresent(const char *const *candidates, int count) {
  for (int i = 0; i < count; ++i)
    if (exists(candidates[i]))
      return candidates[i];
  return nullptr;
}

// Merges Font Awesome Solid onto whichever font was added last.
//
// The range is static (not a local) because ImFontConfig::GlyphRanges is
// documented as legacy storage the atlas keeps a bare pointer to -- a stack
// array here would dangle until the first atlas build and render garbage.
inline void mergeIcons(const char *iconPath, float size) {
  if (!iconPath)
    return;
  static const ImWchar kRange[] = {(ImWchar)UIIcons::kIconMin,
                                   (ImWchar)UIIcons::kIconMax, 0};
  ImFontConfig cfg;
  cfg.MergeMode = true;
  cfg.PixelSnapH = true;
  // Icons render slightly below the text baseline and a touch smaller than
  // the surrounding glyphs, which is what makes "icon + label" read as one
  // word rather than as a picture next to some text.
  cfg.GlyphOffset = ImVec2(0.0f, 1.0f);
  cfg.GlyphMinAdvanceX = size; // uniform advance == icons line up in columns
  ImGui::GetIO().Fonts->AddFontFromFileTTF(iconPath, size * 0.86f, &cfg,
                                           kRange);
}

inline ImFont *addFace(const char *textPath, float size, const char *iconPath) {
  ImGuiIO &io = ImGui::GetIO();
  ImFont *f = nullptr;
  if (textPath) {
    f = io.Fonts->AddFontFromFileTTF(textPath, size);
  } else {
    ImFontConfig cfg;
    cfg.SizePixels = size;
    f = io.Fonts->AddFontDefaultVector(&cfg);
  }
  mergeIcons(iconPath, size);
  return f;
}

} // namespace detail

// Builds the font set. Call once after ImGui::CreateContext().
// `assetDir` locates the bundled Font Awesome; text fonts come from the OS
// since the repo ships no body typeface.
inline void load(const std::string &assetDir, float scale = 1.0f) {
  ImGuiIO &io = ImGui::GetIO();

  static const char *kTextCandidates[] = {
      "C:/Windows/Fonts/segoeui.ttf",
      "C:/Windows/Fonts/tahoma.ttf",
      "/System/Library/Fonts/SFNS.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
  };
  static const char *kMonoCandidates[] = {
      "C:/Windows/Fonts/CascadiaMono.ttf",
      "C:/Windows/Fonts/consola.ttf",
      "/System/Library/Fonts/Menlo.ttc",
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
  };

  const char *textPath = detail::firstPresent(kTextCandidates, 4);
  const char *monoPath = detail::firstPresent(kMonoCandidates, 4);

  const std::string iconStr =
      assetDir + "/fonts/Font Awesome 7 Free-Solid-900.otf";
  const char *iconPath = detail::exists(iconStr.c_str()) ? iconStr.c_str()
                                                         : nullptr;

  Set &s = get();
  s.body = detail::addFace(textPath, 15.0f * scale, iconPath);
  s.small = detail::addFace(textPath, 12.0f * scale, iconPath);
  s.title = detail::addFace(textPath, 17.0f * scale, iconPath);

  if (monoPath)
    s.mono = io.Fonts->AddFontFromFileTTF(monoPath, 13.0f * scale);
  else
    s.mono = s.small;

  io.FontDefault = s.body;
}

// Scoped font push that tolerates a null face (any of the above can fail to
// load), so call sites don't each need their own null check.
struct Scoped {
  bool pushed = false;
  explicit Scoped(ImFont *f) {
    if (f) {
      ImGui::PushFont(f, f->LegacySize);
      pushed = true;
    }
  }
  ~Scoped() {
    if (pushed)
      ImGui::PopFont();
  }
  Scoped(const Scoped &) = delete;
  Scoped &operator=(const Scoped &) = delete;
};

} // namespace UIFonts
