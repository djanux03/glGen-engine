#pragma once

#include "UIFonts.h"
#include "UIIcons.h"
#include "UITheme.h"
#include "imgui.h"
#include "imgui_internal.h" // ImRect / ItemSize / splitter behavior

#include <algorithm>
#include <cstdio>
#include <cstring>

// =============================================================================
// Shared UI primitives
// =============================================================================
// Every panel is built from these. That is the point: the old editor had each
// panel hand-roll its own labels, spacing and column widths, which is why
// nothing lined up across panels. Add a primitive here rather than styling at
// a call site.
//
// The property helpers all follow the same shape:
//
//   if (UI::BeginProps("transform")) {
//     UI::PropDrag("Position", &pos.x);
//     UI::PropCheck("Visible", &visible);
//     UI::EndProps();
//   }
//
// Labels live in a fixed left column, controls stretch to fill the right --
// so a panel reads as a form, not as a stack of differently-sized widgets.
// =============================================================================

namespace UI {

// ── Text helpers ───────────────────────────────────────────────────────────

inline void TextMuted(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ImGui::PushStyleColor(ImGuiCol_Text, UITheme::kTextMuted);
  ImGui::TextV(fmt, args);
  ImGui::PopStyleColor();
  va_end(args);
}

inline void TextFaint(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ImGui::PushStyleColor(ImGuiCol_Text, UITheme::kTextFaint);
  ImGui::TextV(fmt, args);
  ImGui::PopStyleColor();
  va_end(args);
}

// Hover help. Attach right after the widget it describes.
inline void Help(const char *text) {
  if (!text || !*text)
    return;
  ImGui::SameLine(0.0f, UITheme::kSpace1);
  ImGui::PushStyleColor(ImGuiCol_Text, UITheme::kTextFaint);
  ImGui::TextUnformatted("(?)");
  ImGui::PopStyleColor();
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

inline void Tip(const char *text) {
  if (text && *text)
    ImGui::SetItemTooltip("%s", text);
}

// ── Buttons ────────────────────────────────────────────────────────────────

namespace detail {
inline bool ButtonColored(const char *label, ImVec4 bg, ImVec4 hov, ImVec4 act,
                          ImVec4 fg, const ImVec2 &size) {
  ImGui::PushStyleColor(ImGuiCol_Button, bg);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, act);
  ImGui::PushStyleColor(ImGuiCol_Text, fg);
  const bool r = ImGui::Button(label, size);
  ImGui::PopStyleColor(4);
  return r;
}
} // namespace detail

// The one call-to-action per panel. Use sparingly -- if everything is accent,
// nothing is.
inline bool AccentButton(const char *label, const ImVec2 &size = ImVec2(0, 0)) {
  return detail::ButtonColored(label, UITheme::kAccent, UITheme::kAccentHover,
                               UITheme::kAccentActive, ImVec4(1, 1, 1, 1),
                               size);
}

inline bool DangerButton(const char *label, const ImVec2 &size = ImVec2(0, 0)) {
  return detail::ButtonColored(label, UITheme::withAlpha(UITheme::kDanger, 0.16f),
                               UITheme::withAlpha(UITheme::kDanger, 0.30f),
                               UITheme::kDanger, UITheme::kDanger, size);
}

// Borderless button for toolbars and row affordances.
inline bool GhostButton(const char *label, const ImVec2 &size = ImVec2(0, 0)) {
  return detail::ButtonColored(label, ImVec4(0, 0, 0, 0), UITheme::kBg4,
                               UITheme::kAccentDim, UITheme::kTextMuted, size);
}

// Square icon button sized to the current frame height.
inline bool IconButton(const char *icon, const char *tooltip = nullptr,
                       bool active = false) {
  const float h = ImGui::GetFrameHeight();
  bool r;
  if (active)
    r = detail::ButtonColored(icon, UITheme::kAccentDim, UITheme::kAccentDim,
                              UITheme::kAccent, UITheme::kAccentHover,
                              ImVec2(h, h));
  else
    r = detail::ButtonColored(icon, ImVec4(0, 0, 0, 0), UITheme::kBg4,
                              UITheme::kAccentDim, UITheme::kTextMuted,
                              ImVec2(h, h));
  Tip(tooltip);
  return r;
}

// ── Badge ──────────────────────────────────────────────────────────────────

inline void Badge(const char *text, ImVec4 color) {
  const ImVec2 pad(UITheme::kSpace2, 2.0f);
  const ImVec2 sz = ImGui::CalcTextSize(text);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 total(sz.x + pad.x * 2.0f, sz.y + pad.y * 2.0f);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + total.x, p.y + total.y),
                    ImGui::GetColorU32(UITheme::withAlpha(color, 0.16f)),
                    total.y * 0.5f);
  dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), ImGui::GetColorU32(color),
              text);
  ImGui::Dummy(total);
}

// ── Section header: a collapsible group inside a panel ─────────────────────
// Returns true when the body should be drawn. Always pair with EndSection()
// on a true return.

inline bool BeginSection(const char *id, const char *icon, const char *label,
                         bool defaultOpen = true) {
  ImGui::PushID(id);
  ImGuiStorage *st = ImGui::GetStateStorage();
  const ImGuiID key = ImGui::GetID("##open");
  bool open = st->GetBool(key, defaultOpen);

  const float h = ImGui::GetFrameHeight();
  const float w = ImGui::GetContentRegionAvail().x;
  const ImVec2 p = ImGui::GetCursorScreenPos();

  if (ImGui::InvisibleButton("##hdr", ImVec2(w, h))) {
    open = !open;
    st->SetBool(key, open);
  }
  const bool hovered = ImGui::IsItemHovered();

  ImDrawList *dl = ImGui::GetWindowDrawList();
  if (hovered)
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                      ImGui::GetColorU32(UITheme::kBg2), 4.0f);

  const float ty = p.y + (h - ImGui::GetFontSize()) * 0.5f;
  float x = p.x + UITheme::kSpace1;
  dl->AddText(ImVec2(x, ty),
              ImGui::GetColorU32(UITheme::kTextFaint),
              open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT);
  x += 16.0f;
  if (icon && *icon) {
    dl->AddText(ImVec2(x, ty), ImGui::GetColorU32(UITheme::kAccent), icon);
    x += ImGui::CalcTextSize(icon).x + UITheme::kSpace2;
  }
  dl->AddText(ImVec2(x, ty), ImGui::GetColorU32(UITheme::kText), label);

  if (open)
    ImGui::Indent(UITheme::kSpace2);
  else
    ImGui::PopID();
  return open;
}

inline void EndSection() {
  ImGui::Unindent(UITheme::kSpace2);
  ImGui::PopID();
  ImGui::Dummy(ImVec2(0.0f, UITheme::kSpace1));
}

// ── Table-free property row ────────────────────────────────────────────────
// Same two-column rhythm as BeginProps/PropLabel, but usable anywhere -- no
// enclosing table required. Emits the label and leaves the cursor on the
// control half with the item width already stretched.
inline void RowLabel(const char *label, const char *help = nullptr) {
  const float labelW = ImGui::GetContentRegionAvail().x * UITheme::kLabelFrac;
  ImGui::AlignTextToFramePadding();
  ImGui::PushStyleColor(ImGuiCol_Text, UITheme::kTextMuted);
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
  if (help && *help && ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
    ImGui::TextUnformatted(help);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  ImGui::SameLine(labelW);
  ImGui::SetNextItemWidth(-FLT_MIN);
}

// ── Property grid ──────────────────────────────────────────────────────────

inline bool BeginProps(const char *id) {
  const ImGuiTableFlags f = ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_PadOuterX |
                            ImGuiTableFlags_NoSavedSettings;
  if (!ImGui::BeginTable(id, 2, f))
    return false;
  ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthStretch,
                          UITheme::kLabelFrac);
  ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthStretch,
                          1.0f - UITheme::kLabelFrac);
  return true;
}

inline void EndProps() { ImGui::EndTable(); }

// Starts a row and leaves the cursor in the control cell with the item width
// already stretched. Call exactly one widget after this.
inline void PropLabel(const char *label, const char *help = nullptr) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::PushStyleColor(ImGuiCol_Text, UITheme::kTextMuted);
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
  if (help && *help) {
    if (ImGui::BeginItemTooltip()) {
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
      ImGui::TextUnformatted(help);
      ImGui::PopTextWrapPos();
      ImGui::EndTooltip();
    }
  }
  ImGui::TableSetColumnIndex(1);
  ImGui::SetNextItemWidth(-FLT_MIN);
}

inline bool PropSlider(const char *label, float *v, float lo, float hi,
                       const char *fmt = "%.3f", const char *help = nullptr) {
  PropLabel(label, help);
  ImGui::PushID(label);
  const bool r = ImGui::SliderFloat("##v", v, lo, hi, fmt);
  ImGui::PopID();
  return r;
}

inline bool PropSliderInt(const char *label, int *v, int lo, int hi,
                          const char *help = nullptr) {
  PropLabel(label, help);
  ImGui::PushID(label);
  const bool r = ImGui::SliderInt("##v", v, lo, hi);
  ImGui::PopID();
  return r;
}

inline bool PropDrag(const char *label, float *v, float speed = 0.1f,
                     float lo = 0.0f, float hi = 0.0f,
                     const char *fmt = "%.3f", const char *help = nullptr) {
  PropLabel(label, help);
  ImGui::PushID(label);
  const bool r = ImGui::DragFloat("##v", v, speed, lo, hi, fmt);
  ImGui::PopID();
  return r;
}

inline bool PropCheck(const char *label, bool *v, const char *help = nullptr) {
  PropLabel(label, help);
  ImGui::PushID(label);
  const bool r = ImGui::Checkbox("##v", v);
  ImGui::PopID();
  return r;
}

inline bool PropColor(const char *label, float *rgb,
                      const char *help = nullptr) {
  PropLabel(label, help);
  ImGui::PushID(label);
  const bool r = ImGui::ColorEdit3("##v", rgb,
                                   ImGuiColorEditFlags_NoInputs |
                                       ImGuiColorEditFlags_NoLabel);
  ImGui::PopID();
  return r;
}

inline bool PropCombo(const char *label, int *v, const char *const items[],
                      int count, const char *help = nullptr) {
  PropLabel(label, help);
  ImGui::PushID(label);
  const bool r = ImGui::Combo("##v", v, items, count);
  ImGui::PopID();
  return r;
}

inline bool PropText(const char *label, char *buf, size_t bufSize,
                     const char *help = nullptr) {
  PropLabel(label, help);
  ImGui::PushID(label);
  const bool r = ImGui::InputText("##v", buf, bufSize);
  ImGui::PopID();
  return r;
}

// Read-only value row -- pairs with the editable rows above so a panel can
// mix "here is a fact" and "here is a control" without changing shape.
inline void PropValue(const char *label, const char *fmt, ...) {
  PropLabel(label);
  va_list args;
  va_start(args, fmt);
  ImGui::TextV(fmt, args);
  va_end(args);
}

// ── Vec3 with colored axis chips ───────────────────────────────────────────
// The colored bar on each field is what makes X/Y/Z scannable without
// reading the letters. Colors come from UITheme::kAxis* so they match the
// gizmo exactly.

inline bool Vec3Control(const char *label, float *v, float speed = 0.1f,
                        float lo = 0.0f, float hi = 0.0f,
                        const char *help = nullptr) {
  PropLabel(label, help);
  ImGui::PushID(label);

  const ImVec4 axisCol[3] = {UITheme::kAxisX, UITheme::kAxisY, UITheme::kAxisZ};
  const char *axisId[3] = {"##x", "##y", "##z"};
  const float spacing = UITheme::kSpace1;
  const float total = ImGui::GetContentRegionAvail().x;
  const float fieldW = (total - spacing * 2.0f) / 3.0f;
  const float barW = 2.0f;

  bool changed = false;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, 0.0f));
  for (int i = 0; i < 3; ++i) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::DragFloat(axisId[i], &v[i], speed, lo, hi, "%.2f"))
      changed = true;
    // Accent bar hugging the left edge of the field.
    dl->AddRectFilled(p, ImVec2(p.x + barW, p.y + ImGui::GetFrameHeight()),
                      ImGui::GetColorU32(axisCol[i]), 1.0f);
    if (i < 2)
      ImGui::SameLine();
  }
  ImGui::PopStyleVar();
  ImGui::PopID();
  return changed;
}

// ── Search field with a leading icon ───────────────────────────────────────

inline bool SearchField(const char *id, char *buf, size_t bufSize,
                        const char *hint = "Search", float width = -FLT_MIN) {
  ImGui::PushID(id);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      ImVec2(UITheme::kSpace2 + 16.0f,
                             ImGui::GetStyle().FramePadding.y));
  ImGui::SetNextItemWidth(width);
  const bool changed = ImGui::InputTextWithHint("##q", hint, buf, bufSize);
  ImGui::PopStyleVar();

  const ImVec2 mn = ImGui::GetItemRectMin();
  ImGui::GetWindowDrawList()->AddText(
      ImVec2(mn.x + UITheme::kSpace2,
             mn.y + (ImGui::GetFrameHeight() - ImGui::GetFontSize()) * 0.5f),
      ImGui::GetColorU32(UITheme::kTextFaint), ICON_SEARCH);
  ImGui::PopID();
  return changed;
}

// ── Segmented control ──────────────────────────────────────────────────────
// A connected row of mutually exclusive options. Replaces the old toolbar's
// three separate toggle buttons and the radio-button stacks.

inline bool Segmented(const char *id, int *value, const char *const labels[],
                      int count, float width = 0.0f) {
  ImGui::PushID(id);
  const float avail = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
  const float h = ImGui::GetFrameHeight();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(origin, ImVec2(origin.x + avail, origin.y + h),
                    ImGui::GetColorU32(UITheme::kBg3), 5.0f);

  const float seg = avail / (float)count;
  bool changed = false;
  for (int i = 0; i < count; ++i) {
    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + seg * (float)i, origin.y));
    if (ImGui::InvisibleButton("##s", ImVec2(seg, h))) {
      *value = i;
      changed = true;
    }
    const bool on = (*value == i);
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 a(origin.x + seg * (float)i + 2.0f, origin.y + 2.0f);
    const ImVec2 b(origin.x + seg * (float)(i + 1) - 2.0f, origin.y + h - 2.0f);
    if (on)
      dl->AddRectFilled(a, b, ImGui::GetColorU32(UITheme::kAccent), 4.0f);
    else if (hov)
      dl->AddRectFilled(a, b, ImGui::GetColorU32(UITheme::kBg4), 4.0f);
    const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
    dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f),
                ImGui::GetColorU32(on ? ImVec4(1, 1, 1, 1)
                                      : UITheme::kTextMuted),
                labels[i]);
    ImGui::PopID();
  }
  // Reserve the whole strip as ONE item of the correct height. The
  // InvisibleButtons above were placed with SetCursorScreenPos, which leaves
  // ImGui's line-height bookkeeping describing only the last segment -- a
  // following SameLine() would then align to the wrong baseline and push
  // subsequent widgets down out of their region.
  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(ImVec2(avail, h));
  ImGui::PopID();
  return changed;
}

// ── Splitter ───────────────────────────────────────────────────────────────
// Draggable divider between two shell regions. `size` is the tracked width
// (vertical splitter) or height (horizontal), clamped to [lo, hi].

inline bool Splitter(const char *id, bool vertical, float thickness,
                     float *size, float lo, float hi, float length) {
  ImGui::PushID(id);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 sz = vertical ? ImVec2(thickness, length)
                             : ImVec2(length, thickness);
  ImGui::InvisibleButton("##sp", sz);
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  if (hovered || active)
    ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW
                                   : ImGuiMouseCursor_ResizeNS);
  bool changed = false;
  if (active) {
    const float d = vertical ? ImGui::GetIO().MouseDelta.x
                             : ImGui::GetIO().MouseDelta.y;
    if (d != 0.0f) {
      *size = std::clamp(*size + d, lo, hi);
      changed = true;
    }
  }
  if (hovered || active) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y),
                      ImGui::GetColorU32(active ? UITheme::kAccent
                                                : UITheme::kAccentDim));
  }
  ImGui::PopID();
  return changed;
}

// ── Empty state ────────────────────────────────────────────────────────────
// Shown where a panel would otherwise be blank. The old editor printed a bare
// "No entity selected." -- this says what to do about it.

inline void EmptyState(const char *icon, const char *title, const char *hint) {
  const float avail = ImGui::GetContentRegionAvail().y;
  if (avail > 80.0f)
    ImGui::Dummy(ImVec2(0.0f, avail * 0.28f));

  auto centered = [](const char *s, ImVec4 col, ImFont *font) {
    UIFonts::Scoped f(font);
    const float w = ImGui::CalcTextSize(s).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, (ImGui::GetContentRegionAvail().x - w) *
                                            0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
  };
  if (icon && *icon)
    centered(icon, UITheme::kBg4, UIFonts::get().title);
  centered(title, UITheme::kTextMuted, nullptr);
  if (hint && *hint)
    centered(hint, UITheme::kTextFaint, UIFonts::get().small);
}

} // namespace UI
