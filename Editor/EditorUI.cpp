#include "EditorUI.h"
#include "ECS/Components.h"
#include "FireFX.h"
#include "HDRSky.h"
#include "OBJModel.h" // Must have full definition for part selection
#include "Scene.h" // Must have full definition of Scene & Entity for outliner
#include "SunFX.h"

#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>

// -----------------------------------------------------------------------------
// Helper: Focus Camera
// -----------------------------------------------------------------------------
static void focusCameraOn(glm::vec3 &camPos, float &pitch, float &yaw,
                          glm::vec3 &camFront, const glm::vec3 &target,
                          float distance) {
  glm::vec3 dir = glm::normalize(target - camPos);
  if (glm::length(dir) < 0.0001f)
    dir = glm::vec3(0, 0, -1);

  pitch = glm::degrees(asinf(glm::clamp(dir.y, -1.0f, 1.0f)));
  yaw = glm::degrees(atan2f(dir.z, dir.x));
  camFront = dir;

  camPos = target - dir * distance;
}

static auto buildTRS = [](const glm::vec3 &pos, const glm::vec3 &rotDeg,
                          const glm::vec3 &scale) {
  glm::mat4 m(1.0f);
  m = glm::translate(m, pos);
  m = glm::rotate(m, glm::radians(rotDeg.y), glm::vec3(0, 1, 0));
  m = glm::rotate(m, glm::radians(rotDeg.x), glm::vec3(1, 0, 0));
  m = glm::rotate(m, glm::radians(rotDeg.z), glm::vec3(0, 0, 1));
  m = glm::scale(m, scale);
  return m;
};

// -----------------------------------------------------------------------------
// Main Editor Panel (Parameters)
// -----------------------------------------------------------------------------
EditorUIOutput EditorUI::draw(bool &uiMode, float &walkStep, float &runMult,
                              float &jumpStrength, float &gravity,
                              bool &freezePhysics, float &mouseSensitivity,
                              float &fov, SunFX &sun, FireFX &fire,
                              int &terrainSize, float &terrainSpacing,
                              glm::vec3 &treePos, glm::vec3 &treeScale) {
  EditorUIOutput out{};

  // 1. CRITICAL: This check must be BEFORE ImGui::Begin
  if (!uiMode)
    return out;

  // 2. Start the window
  ImGui::Begin("glGen Editor");

  // 3. Save/Load Buttons
  if (ImGui::Button("Save Config"))
    out.saveRequested = true;
  ImGui::SameLine();
  if (ImGui::Button("Load Config"))
    out.loadRequested = true;

  ImGui::Separator();
  ImGui::SeparatorText("Terrain");

  bool changed = false;
  changed |= ImGui::InputInt("Size", &terrainSize);
  changed |= ImGui::InputFloat("Spacing", &terrainSpacing);

  if (ImGui::Button("+ Size")) {
    terrainSize += 1;
    changed = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("- Size")) {
    terrainSize -= 1;
    changed = true;
  }

  if (terrainSize < 1)
    terrainSize = 1;
  if (terrainSize > 200)
    terrainSize = 200;

  out.terrainDirty |= changed;

  ImGui::Text("UI mode: %s", uiMode ? "ON (cursor free)" : "OFF (mouse look)");
  ImGui::Text("ESC toggles UI mode");

  ImGui::SeparatorText("Sun Transform");
  ImGui::DragFloat3("Sun position", &sun.sunPos.x, 0.1f);

  ImGui::SeparatorText("Sun FX");
  ImGui::SliderFloat("Sun size", &sun.sunSize, 0.2f, 10.0f);
  ImGui::SliderFloat("Halo mult", &sun.haloSizeMult, 1.0f, 3.0f);
  ImGui::SliderFloat("Glow strength", &sun.glowStrength, 0.0f, 12.0f);

  ImGui::SeparatorText("Sun Lighting");
  ImGui::DragFloat3("Sun direction", &sun.sunDir.x, 0.01f, -1.0f, 1.0f);

  if (glm::length(sun.sunDir) < 0.0001f)
    sun.sunDir = glm::vec3(0.0f, -1.0f, 0.0f);
  sun.sunDir = glm::normalize(sun.sunDir);

  ImGui::ColorEdit3("Sun color", &sun.sunColor.x);
  ImGui::SliderFloat("Ambient", &sun.ambientStrength, 0.0f, 1.0f);

  ImGui::SeparatorText("Sun Particles");
  ImGui::SliderFloat("Emit rate", &sun.emitRate, 0.0f, 3000.0f);
  ImGui::SliderInt("Max particles", &sun.maxParticles, 0, 8000);
  ImGui::SliderFloat("Particle speed", &sun.particleSpeed, 0.0f, 25.0f);
  ImGui::SliderFloat("Particle life", &sun.particleLife, 0.05f, 6.0f);
  ImGui::SliderFloat("Particle size", &sun.particleSize, 0.01f, 1.0f);

  ImGui::SeparatorText("Fire effect");
  ImGui::Checkbox("Enabled", &fire.params().enabled);
  ImGui::DragFloat3("Offset", &fire.params().offset.x, 0.01f);
  ImGui::SliderFloat("Size", &fire.params().size, 0.1f, 10.0f);
  ImGui::SliderFloat("Fire intensity", &fire.params().intensity, 0.0f, 5.0f);

  ImGui::Separator();
  ImGui::SliderFloat("Smoke opacity", &fire.params().smokeOpacity, 0.0f, 1.0f);
  ImGui::SliderFloat("Smoke width", &fire.params().smokeScaleXY, 0.5f, 4.0f);
  ImGui::SliderFloat("Smoke height", &fire.params().smokeScaleY, 0.5f, 6.0f);
  ImGui::SliderFloat("Smoke lift", &fire.params().smokeLift, 0.0f, 3.0f);

  ImGui::SeparatorText("Player");
  ImGui::SliderFloat("Walk speed", &walkStep, 0.005f, 0.20f);
  ImGui::SliderFloat("Run multiplier", &runMult, 1.0f, 6.0f);

  ImGui::SeparatorText("Jump");
  ImGui::SliderFloat("Jump strength", &jumpStrength, 0.01f, 0.60f);
  ImGui::SliderFloat("Gravity", &gravity, 0.001f, 0.05f);
  ImGui::Checkbox("Freeze physics", &freezePhysics);

  ImGui::SeparatorText("Camera");
  ImGui::SliderFloat("Mouse sensitivity", &mouseSensitivity, 0.01f, 1.0f);
  ImGui::SliderFloat("FOV", &fov, 30.0f, 90.0f);

  ImGui::SeparatorText("Sky");
  static glm::vec3 skyRotDeg(0.0f);
  ImGui::TextUnformatted("Sky rot X");
  ImGui::SliderFloat("##SkyRotX", &skyRotDeg.x, -180.0f, 180.0f);
  ImGui::TextUnformatted("Sky rot Y");
  ImGui::SliderFloat("##SkyRotY", &skyRotDeg.y, -180.0f, 180.0f);
  ImGui::TextUnformatted("Sky rot Z");
  ImGui::SliderFloat("##SkyRotZ", &skyRotDeg.z, -180.0f, 180.0f);
  out.skyRotDeg = skyRotDeg;

  ImGui::SeparatorText("Tree");
  ImGui::DragFloat3("Tree position", &treePos.x, 0.1f);
  ImGui::DragFloat3("Tree scale", &treeScale.x, 0.01f);

  // 4. CRITICAL: This must always run if Begin() ran
  ImGui::End();

  // 5. Output capture state
  ImGuiIO &io = ImGui::GetIO();
  out.wantCaptureMouse = io.WantCaptureMouse;
  out.wantCaptureKeyboard = io.WantCaptureKeyboard;
  return out;
}

// -----------------------------------------------------------------------------
// Gizmo & Outliner Window
// -----------------------------------------------------------------------------
void EditorUI::drawGizmo(bool uiMode, const glm::mat4 &view,
                         const glm::mat4 &projection, Scene &scene, SunFX &sun,
                         EditorSelectionState &s, glm::vec3 &cameraPos) {
  if (!uiMode)
    return;

  ImGuizmo::BeginFrame();
  ImGuizmo::SetOrthographic(false);
  ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());

  ImGuiIO &io = ImGui::GetIO();
  ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);

  auto &reg = scene.registry();

  // --- Helpers ---
  auto passFilter = [&](const char *name) -> bool {
    if (s.outlinerFilter[0] == 0)
      return true;
    std::string a = name ? name : "";
    std::string b = s.outlinerFilter;
    for (auto &c : a)
      c = (char)tolower((unsigned char)c);
    for (auto &c : b)
      c = (char)tolower((unsigned char)c);
    return a.find(b) != std::string::npos;
  };

  auto isSelected = [&](uint32_t id) {
    for (uint32_t v : s.selectedEntities)
      if (v == id)
        return true;
    return false;
  };

  auto addToSelection = [&](uint32_t id) {
    if (!isSelected(id))
      s.selectedEntities.push_back(id);
  };

  auto removeFromSelection = [&](uint32_t id) {
    for (int i = 0; i < (int)s.selectedEntities.size(); ++i) {
      if (s.selectedEntities[i] == id) {
        s.selectedEntities.erase(s.selectedEntities.begin() + i);
        break;
      }
    }
  };

  auto clearSelection = [&]() {
    s.selectedEntities.clear();
    s.selectedEntityId = 0; // 0 is invalid
    s.lastClickedEntity = 0;
    s.editObjPart = false;
    s.selectedObjPartName.clear();
    s.renaming = false;
  };

  // --- Gizmo Tools Window ---
  ImGui::Begin("Gizmo");
  if (ImGui::RadioButton("Translate", s.gizmoOp == ImGuizmo::TRANSLATE))
    s.gizmoOp = ImGuizmo::TRANSLATE;
  ImGui::SameLine();
  if (ImGui::RadioButton("Rotate", s.gizmoOp == ImGuizmo::ROTATE))
    s.gizmoOp = ImGuizmo::ROTATE;
  ImGui::SameLine();
  if (ImGui::RadioButton("Scale", s.gizmoOp == ImGuizmo::SCALE))
    s.gizmoOp = ImGuizmo::SCALE;
  ImGui::Separator();
  ImGui::DragFloat("Focus distance", &s.focusDistance, 0.25f, 1.0f, 200.0f);
  ImGui::End();

  // --- Outliner Window ---
  ImGui::Begin("Outliner");
  ImGui::InputTextWithHint("##filter", "Search...", s.outlinerFilter, 128);
  ImGui::SameLine();
  if (ImGui::Button("Clear"))
    s.outlinerFilter[0] = 0;
  ImGui::Separator();

  // Sun selection (Pseudo-entity 0? No, Sun is 0 in legacy logic maybe? Let's
  // treat Sun specially) Legacy had s.selectedEntityIndex == -1 for Sun. We'll
  // use ID 0 for Sun / None, but wait, 0 is invalid usually. Let's rely on a
  // specific check or specific ID. Actually, let's keep Sun as a special case.
  // If selectedEntityId == ? Maybe we use a specific flag or just say if
  // selectedEntityId == 0 and selectedEntities is empty?

  // For now, let's make Sun selectable as "Sun" item
  bool sunSelected = (s.selectedEntityId == 0 && !s.selectedEntities.empty() &&
                      s.selectedEntities[0] == 0);
  // Actually, simpler: if the user clicks "Sun", we clear selection and set a
  // specialized state? Or we just reserve ID 0 for Sun? ECS Registry starts at
  // 1 usually. Let's reserve 0 for Sun for now in the logic of
  // "selectedEntityId".

  if (ImGui::Selectable("Sun", s.selectedEntityId == 0)) // Treated as ID 0
  {
    clearSelection();
    s.selectedEntityId = 0;
    s.selectedEntities.push_back(0); // 0 = sun
  }
  ImGui::Separator();

  // Iterate ECS entities
  // We need TransformComponent to be considered an "Object"
  auto viewTr = reg.view<TransformComponent>();

  for (auto entity : viewTr) {
    uint32_t id = (uint32_t)entity;

    // Name
    std::string name = "Entity " + std::to_string(id);
    if (reg.has<NameComponent>(entity)) {
      name = reg.get<NameComponent>(entity).name;
    }

    if (!passFilter(name.c_str()))
      continue;

    bool selected = isSelected(id);
    const bool primary = (id == s.selectedEntityId);
    bool pushedPrimary = false;

    if (primary) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 0.4f, 1));
      pushedPrimary = true;
    }

    // Use ID as ID for ImGui
    ImGui::PushID((int)id);
    if (ImGui::Selectable(name.c_str(), selected)) {
      const bool ctrl = ImGui::GetIO().KeyCtrl;
      // Shift select is hard with unordered map/view iteration. Skip for now.
      // const bool shift = ImGui::GetIO().KeyShift;

      if (ctrl) {
        if (selected)
          removeFromSelection(id);
        else
          addToSelection(id);
        s.lastClickedEntity = id;
      } else {
        clearSelection();
        s.selectedEntities.push_back(id); // Re-add current
        s.selectedEntityId = id;
        s.lastClickedEntity = id;
      }

      // If we just selected this, make it primary
      if (s.selectedEntityId == 0 || !isSelected(s.selectedEntityId)) {
        s.selectedEntityId = id;
      }
    }
    ImGui::PopID();

    if (pushedPrimary)
      ImGui::PopStyleColor();
  }

  ImGui::Separator();
  bool hasPrimaryEntity = (s.selectedEntityId != 0 &&
                           reg.has<TransformComponent>(s.selectedEntityId));

  if (ImGui::Button("Focus") && hasPrimaryEntity) {
    if (reg.has<TransformComponent>(s.selectedEntityId)) {
      auto &tr = reg.get<TransformComponent>(s.selectedEntityId);
      cameraPos = tr.position + glm::vec3(0, 5, 10); // Simple focus
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Rename") && hasPrimaryEntity) {
    s.renaming = true;
    std::string name = "Entity " + std::to_string(s.selectedEntityId);
    if (reg.has<NameComponent>(s.selectedEntityId))
      name = reg.get<NameComponent>(s.selectedEntityId).name;
    std::snprintf(s.renameBuf, 128, "%s", name.c_str());
  }
  ImGui::SameLine();
  if (ImGui::Button("Duplicate")) {
  } // Stub for ECS duplicate?

  bool justCleared = false;
  ImGui::SameLine();
  if (ImGui::Button("Clear selection")) {
    clearSelection();
    justCleared = true;
  }
  ImGui::End();

  if (justCleared)
    return;

  // Rename Popup
  if (s.renaming && hasPrimaryEntity) {
    ImGui::Begin("Rename", &s.renaming, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::InputText("Name", s.renameBuf, 128);
    if (ImGui::Button("Apply")) {
      if (!reg.has<NameComponent>(s.selectedEntityId))
        reg.emplace<NameComponent>(s.selectedEntityId);
      reg.get<NameComponent>(s.selectedEntityId).name = s.renameBuf;
      s.renaming = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
      s.renaming = false;
    ImGui::End();
  }

  // Model Parts
  OBJModel *modelPtr = nullptr;
  if (hasPrimaryEntity) {
    if (reg.has<MeshComponent>(s.selectedEntityId)) {
      modelPtr = reg.get<MeshComponent>(s.selectedEntityId).model;
    }
  }

  ImGui::Begin("Model Parts");
  if (!hasPrimaryEntity)
    ImGui::Text("Select an entity first.");
  else if (!modelPtr)
    ImGui::Text("Selected entity has no OBJ model.");
  else {
    ImGui::Checkbox("Edit selected part", &s.editObjPart);
    auto names = modelPtr->objectNames();
    if (names.empty())
      ImGui::Text("No named parts found.");
    else {
      ImGui::Text("Select part:");
      for (const auto &n : names) {
        bool partSelected = (s.selectedObjPartName == n);
        if (ImGui::Selectable(n.c_str(), partSelected))
          s.selectedObjPartName = n;
      }
      if (!s.selectedObjPartName.empty()) {
        if (ImGui::Button("Clear selected part TRS"))
          modelPtr->clearObjectLocalTRS(s.selectedObjPartName);
        ImGui::SameLine();
        if (ImGui::Button("Clear ALL parts TRS"))
          modelPtr->clearAllObjectLocalTRS();
      }
    }
  }
  ImGui::End();

  // Gizmo Logic
  if (s.selectedEntityId == 0) {
    // SUN GIZMO
    if (sunSelected) { // Explicitly check if sun is selected
      glm::mat4 model = glm::translate(glm::mat4(1.0f), sun.sunPos);
      ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection),
                           (ImGuizmo::OPERATION)s.gizmoOp,
                           (ImGuizmo::MODE)s.gizmoMode, glm::value_ptr(model));
      if (ImGuizmo::IsUsing()) {
        float t[3], r[3], sc[3];
        ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(model), t, r, sc);
        sun.sunPos = {t[0], t[1], t[2]};
      }
    }
    return;
  }

  if (!hasPrimaryEntity)
    return;

  // Entity Gizmo
  auto &tr = reg.get<TransformComponent>(
      s.selectedEntityId); // Assumed has Transform if it was in viewTr
  glm::mat4 M_entity = tr.getMatrix(); // Use helper
  bool canEditPart =
      (s.editObjPart && modelPtr && !s.selectedObjPartName.empty());

  if (!canEditPart) {
    glm::mat4 model = M_entity;
    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection),
                         (ImGuizmo::OPERATION)s.gizmoOp,
                         (ImGuizmo::MODE)s.gizmoMode, glm::value_ptr(model));
    if (ImGuizmo::IsUsing()) {
      float t[3], r[3], sc[3];
      ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(model), t, r, sc);
      tr.position = {t[0], t[1], t[2]};
      tr.rotation = {r[0], r[1], r[2]};
      tr.scale = {sc[0], sc[1], sc[2]};
    }
  } else {
    glm::vec3 lp(0.0f), lr(0.0f), ls(1.0f);
    (void)modelPtr->getObjectLocalTRS(s.selectedObjPartName, lp, lr, ls);
    glm::mat4 M_partLocal = buildTRS(lp, lr, ls);
    glm::mat4 model = M_entity * M_partLocal;
    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection),
                         (ImGuizmo::OPERATION)s.gizmoOp,
                         (ImGuizmo::MODE)s.gizmoMode, glm::value_ptr(model));
    if (ImGuizmo::IsUsing()) {
      glm::mat4 newLocal = glm::inverse(M_entity) * model;
      float t[3], r[3], sc[3];
      ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(newLocal), t, r, sc);
      modelPtr->setObjectLocalTRS(
          s.selectedObjPartName, glm::vec3(t[0], t[1], t[2]),
          glm::vec3(r[0], r[1], r[2]), glm::vec3(sc[0], sc[1], sc[2]));
    }
  }
}
