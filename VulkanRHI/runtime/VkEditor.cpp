#include "VkEditor.h"

#include "VulkanRenderer.h"

#include "Assets/AssetManager.h"
#include "Assets/MeshData.h"
#include "Core/Logger.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "ECS/Systems/PhysicsSystem.h"
#include "Scene/Scene.h"

#include "EditorTheme.h"
#include "ImGuizmo.h"
#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

// ─── Helpers shared with the old EditorUI (Editor/EditorUI.cpp) ─────────────

static float normalizeAngleDeg(float angle) {
  while (angle > 180.0f)
    angle -= 360.0f;
  while (angle < -180.0f)
    angle += 360.0f;
  return angle;
}

static void normalizeEulerDeg(glm::vec3 &euler) {
  euler.x = normalizeAngleDeg(euler.x);
  euler.y = normalizeAngleDeg(euler.y);
  euler.z = normalizeAngleDeg(euler.z);
}

// Matches TransformComponent::getMatrix: T * Ry * Rx * Rz * S. ImGuizmo's
// built-in Euler decomposition uses a different convention, which makes
// entity rotations jump or drift after gizmo edits.
static bool decomposeTRSYXZ(const glm::mat4 &m, glm::vec3 &pos,
                            glm::vec3 &rotDeg, glm::vec3 &scale) {
  constexpr float kEpsilon = 1e-5f;

  pos = glm::vec3(m[3]);

  glm::vec3 c0(m[0]);
  glm::vec3 c1(m[1]);
  glm::vec3 c2(m[2]);
  scale = glm::vec3(glm::length(c0), glm::length(c1), glm::length(c2));

  if (scale.x < kEpsilon || scale.y < kEpsilon || scale.z < kEpsilon)
    return false;

  glm::mat3 r;
  r[0] = c0 / scale.x;
  r[1] = c1 / scale.y;
  r[2] = c2 / scale.z;

  if (glm::determinant(r) < 0.0f) {
    scale.x = -scale.x;
    r[0] = -r[0];
  }

  const float pitch = std::asin(std::clamp(-r[2][1], -1.0f, 1.0f));
  const float cosPitch = std::cos(pitch);

  float yaw = 0.0f;
  float roll = 0.0f;
  if (std::abs(cosPitch) > kEpsilon) {
    yaw = std::atan2(r[2][0], r[2][2]);
    roll = std::atan2(r[0][1], r[1][1]);
  } else {
    // At the singularity yaw and roll are coupled. Preserve a stable result
    // instead of letting the editor explode into large equivalent angles.
    yaw = std::atan2(-r[0][2], r[0][0]);
    roll = 0.0f;
  }

  rotDeg = glm::degrees(glm::vec3(pitch, yaw, roll));
  normalizeEulerDeg(rotDeg);
  return true;
}

static bool DragFloat3Colored(const char *label, float *v, float speed = 0.1f,
                              float vMin = 0.0f, float vMax = 0.0f) {
  bool edited = false;
  ImGui::PushID(label);

  float fullWidth = ImGui::CalcItemWidth();
  float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
  float fieldW = (fullWidth - spacing * 2.0f) / 3.0f;

  // X — Red
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.45f, 0.12f, 0.12f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                        ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                        ImVec4(0.65f, 0.18f, 0.18f, 1.0f));
  ImGui::SetNextItemWidth(fieldW);
  edited |= ImGui::DragFloat("##X", &v[0], speed, vMin, vMax, "X: %.2f");
  ImGui::PopStyleColor(3);

  ImGui::SameLine(0, spacing);

  // Y — Green
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.40f, 0.12f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                        ImVec4(0.15f, 0.50f, 0.15f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                        ImVec4(0.18f, 0.60f, 0.18f, 1.0f));
  ImGui::SetNextItemWidth(fieldW);
  edited |= ImGui::DragFloat("##Y", &v[1], speed, vMin, vMax, "Y: %.2f");
  ImGui::PopStyleColor(3);

  ImGui::SameLine(0, spacing);

  // Z — Blue
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.12f, 0.45f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                        ImVec4(0.15f, 0.15f, 0.55f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                        ImVec4(0.18f, 0.18f, 0.65f, 1.0f));
  ImGui::SetNextItemWidth(fieldW);
  edited |= ImGui::DragFloat("##Z", &v[2], speed, vMin, vMax, "Z: %.2f");
  ImGui::PopStyleColor(3);

  ImGui::SameLine(0, spacing);
  ImGui::TextDisabled("%s", label);

  ImGui::PopID();
  return edited;
}

// Helper: Component header with right-click Remove + Reset button
static bool ComponentHeader(const char *label, bool *open, bool canRemove,
                            bool *wantsRemove, bool *wantsReset,
                            ImGuiTreeNodeFlags extraFlags = 0) {
  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen |
                             ImGuiTreeNodeFlags_Framed |
                             ImGuiTreeNodeFlags_AllowOverlap | extraFlags;

  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.18f, 0.22f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                        ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
  *open = ImGui::CollapsingHeader(label, flags);
  ImGui::PopStyleColor(2);

  ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
  ImGui::PushID(label);
  if (ImGui::SmallButton("R")) {
    *wantsReset = true;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Reset to defaults");
  ImGui::PopID();

  if (canRemove && ImGui::BeginPopupContextItem(label)) {
    if (ImGui::MenuItem("Remove Component")) {
      *wantsRemove = true;
    }
    ImGui::EndPopup();
  }

  return *open;
}

static bool isModelFile(const std::string &ext) {
  return ext == ".obj" || ext == ".gltf" || ext == ".glb" || ext == ".fbx";
}

// ─── VkEditor ────────────────────────────────────────────────────────────────

void VkEditor::releasePhysicsBodies(Context &ctx) {
  auto &reg = ctx.scene.registry();
  for (EntityId e : reg.view<RigidbodyComponent>()) {
    auto &rb = reg.get<RigidbodyComponent>(e);
    if (rb.bodyID != 0xFFFFFFFF) {
      ctx.physics.removeBody(rb.bodyID);
      rb.bodyID = 0xFFFFFFFF;
    }
  }
}

void VkEditor::deleteEntity(Context &ctx, uint32_t id) {
  if (id == 0)
    return;
  auto &reg = ctx.scene.registry();
  if (reg.has<RigidbodyComponent>(id)) {
    auto &rb = reg.get<RigidbodyComponent>(id);
    if (rb.bodyID != 0xFFFFFFFF) {
      ctx.physics.removeBody(rb.bodyID);
      rb.bodyID = 0xFFFFFFFF;
    }
  }
  ctx.scene.deleteEntity(id);
  ctx.scene.flushPendingDestroy();
  if (selection.selectedEntityId == id)
    selection.selectedEntityId = 0;
  selection.selectedEntities.erase(
      std::remove(selection.selectedEntities.begin(),
                  selection.selectedEntities.end(), id),
      selection.selectedEntities.end());
}

bool VkEditor::draw(Context &ctx, const glm::mat4 &view,
                    const glm::mat4 &proj) {
  bool sceneModified = false;

  if (mScenePath.empty())
    mScenePath = ctx.assetDir + "/scenes/vk_editor_scene.json";
  if (mBrowsePath.empty())
    mBrowsePath = ctx.assetDir;

  drawMenuBar(ctx);

  // ── Toolbar (same strip as the old editor) ─────────────────────────────
  if (selection.gizmoOp == ImGuizmo::TRANSLATE)
    toolbar.gizmoOp = ToolbarState::Translate;
  else if (selection.gizmoOp == ImGuizmo::ROTATE)
    toolbar.gizmoOp = ToolbarState::Rotate;
  else if (selection.gizmoOp == ImGuizmo::SCALE)
    toolbar.gizmoOp = ToolbarState::Scale;

  EditorToolbar::draw(toolbar);
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) // not fly-navigating
    EditorToolbar::processShortcuts(toolbar);

  if (toolbar.gizmoOp == ToolbarState::Translate)
    selection.gizmoOp = ImGuizmo::TRANSLATE;
  else if (toolbar.gizmoOp == ToolbarState::Rotate)
    selection.gizmoOp = ImGuizmo::ROTATE;
  else if (toolbar.gizmoOp == ToolbarState::Scale)
    selection.gizmoOp = ImGuizmo::SCALE;
  selection.gizmoMode = toolbar.worldSpace ? ImGuizmo::WORLD : ImGuizmo::LOCAL;

  // ── Panels ─────────────────────────────────────────────────────────────
  const float toolbarH = 44.0f; // toolbar strip + a small gap
  ImGuiViewport *vp = ImGui::GetMainViewport();

  if (mShowHierarchy) {
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + toolbarH),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280, 400), ImGuiCond_FirstUseEver);
    drawHierarchy(ctx);
  }
  if (mShowInspector) {
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 330,
                                   vp->WorkPos.y + toolbarH),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320, 500), ImGuiCond_FirstUseEver);
    sceneModified |= drawInspector(ctx);
  }
  if (mShowAssets) {
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 300,
                                   vp->WorkPos.y + vp->WorkSize.y - 260),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(600, 250), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Assets", &mShowAssets, ImGuiWindowFlags_NoCollapse)) {
      if (ImGui::BeginTabBar("AssetsConsoleTabs")) {
        if (ImGui::BeginTabItem("Assets")) {
          drawAssetsContent(ctx);
          ImGui::EndTabItem();
        }
        if (mShowLog && ImGui::BeginTabItem("Console")) {
          drawLog();
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
    }
    ImGui::End();
  }
  if (mShowEnvironment) {
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 330,
                                   vp->WorkPos.y + toolbarH + 510),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320, 260), ImGuiCond_FirstUseEver);
    drawEnvironment(ctx);
  }
  if (mShowStats) {
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + toolbarH + 410),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(260, 220), ImGuiCond_FirstUseEver);
    drawStats(ctx);
  }

  // ── Gizmo ──────────────────────────────────────────────────────────────
  sceneModified |= drawGizmo(ctx, view, proj);

  return sceneModified;
}

void VkEditor::drawMenuBar(Context &ctx) {
  if (!ImGui::BeginMainMenuBar())
    return;

  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("New Scene")) {
      releasePhysicsBodies(ctx);
      ctx.scene.clear();
      selection = SelectionState{};
      LOG_INFO("Editor", "New scene");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
      std::error_code ec;
      std::filesystem::create_directories(
          std::filesystem::path(mScenePath).parent_path(), ec);
      if (ctx.scene.saveToFile(mScenePath))
        LOG_INFO("Editor", "Saved scene: " + mScenePath);
      else
        LOG_ERROR("Editor", "Failed to save scene: " + mScenePath);
    }
    if (ImGui::MenuItem("Load Scene", "Ctrl+O")) {
      releasePhysicsBodies(ctx);
      if (ctx.scene.loadFromFile(mScenePath)) {
        selection = SelectionState{};
        LOG_INFO("Editor", "Loaded scene: " + mScenePath);
      } else {
        LOG_ERROR("Editor", "Failed to load scene: " + mScenePath);
      }
    }
    ImGui::SetNextItemWidth(320);
    char pathBuf[512];
    std::snprintf(pathBuf, sizeof(pathBuf), "%s", mScenePath.c_str());
    if (ImGui::InputText("##ScenePath", pathBuf, sizeof(pathBuf)))
      mScenePath = pathBuf;
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Create")) {
    if (ImGui::MenuItem("Empty Entity")) {
      uint32_t e = ctx.scene.createEmptyEntity("Empty");
      selection.selectedEntityId = e;
      selection.selectedEntities = {e};
    }
    ImGui::Separator();
    const char *prims[] = {"cube", "sphere", "plane", "cylinder", "cone"};
    const char *labels[] = {"Cube", "Sphere", "Plane", "Cylinder", "Cone"};
    for (int i = 0; i < 5; ++i) {
      if (ImGui::MenuItem(labels[i])) {
        uint32_t e = ctx.scene.spawnPrimitive(prims[i]);
        if (e != 0) {
          selection.selectedEntityId = e;
          selection.selectedEntities = {e};
        }
      }
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Windows")) {
    ImGui::MenuItem("Hierarchy", nullptr, &mShowHierarchy);
    ImGui::MenuItem("Inspector", nullptr, &mShowInspector);
    ImGui::MenuItem("Assets / Console", nullptr, &mShowAssets);
    ImGui::MenuItem("Environment", nullptr, &mShowEnvironment);
    ImGui::MenuItem("Statistics", nullptr, &mShowStats);
    ImGui::EndMenu();
  }

  if (ctx.simulatePhysics) {
    // Play/pause control on the right, like the old play-state buttons.
    ImGui::SameLine(ImGui::GetWindowWidth() - 130);
    const bool playing = *ctx.simulatePhysics;
    ImGui::PushStyleColor(ImGuiCol_Button, playing ? EditorTheme::kAccent
                                                   : EditorTheme::kHeader);
    if (ImGui::SmallButton(playing ? "|| Pause Physics" : "> Play Physics"))
      *ctx.simulatePhysics = !playing;
    ImGui::PopStyleColor();
  }

  ImGui::EndMainMenuBar();

  // Global shortcuts
  ImGuiIO &io = ImGui::GetIO();
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(mScenePath).parent_path(), ec);
    if (ctx.scene.saveToFile(mScenePath))
      LOG_INFO("Editor", "Saved scene: " + mScenePath);
  }
  if (!io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_Delete, false) &&
      selection.selectedEntityId != 0) {
    deleteEntity(ctx, selection.selectedEntityId);
  }
}

void VkEditor::drawHierarchy(Context &ctx) {
  if (ImGui::Begin("Hierarchy", &mShowHierarchy, ImGuiWindowFlags_NoCollapse)) {
    auto &s = selection;
    ImGui::InputTextWithHint("##filter", "Search...", s.outlinerFilter, 128);
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
      s.outlinerFilter[0] = 0;
    ImGui::Separator();

    auto &reg = ctx.scene.registry();

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

    uint32_t pendingDelete = 0;

    auto drawEntityRow = [&](EntityId entity, const std::string &name) {
      uint32_t id = (uint32_t)entity;
      bool isSelected = false;
      for (auto selId : s.selectedEntities)
        if (selId == id)
          isSelected = true;

      ImGui::PushID((int)id);
      if (ImGui::Selectable(name.c_str(), isSelected)) {
        if (ImGui::GetIO().KeyCtrl) {
          if (isSelected) {
            s.selectedEntities.erase(std::remove(s.selectedEntities.begin(),
                                                 s.selectedEntities.end(), id),
                                     s.selectedEntities.end());
            if (s.selectedEntityId == id)
              s.selectedEntityId = 0;
          } else {
            s.selectedEntities.push_back(id);
            s.selectedEntityId = id;
          }
        } else {
          s.selectedEntities.clear();
          s.selectedEntities.push_back(id);
          s.selectedEntityId = id;
        }
        s.lastClickedEntity = id;
      }
      if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Delete"))
          pendingDelete = id;
        ImGui::EndPopup();
      }
      ImGui::PopID();
    };

    std::vector<std::pair<EntityId, std::string>> sceneEntities;
    auto view = reg.view<TransformComponent>();
    for (auto entity : view) {
      std::string name = "Entity " + std::to_string((uint32_t)entity);
      if (reg.has<NameComponent>(entity))
        name = reg.get<NameComponent>(entity).name;
      if (!passFilter(name.c_str()))
        continue;
      sceneEntities.push_back({entity, name});
    }

    if (ImGui::TreeNodeEx("Scene",
                          ImGuiTreeNodeFlags_DefaultOpen |
                              ImGuiTreeNodeFlags_SpanFullWidth,
                          "Scene (%d)", (int)sceneEntities.size())) {
      for (auto &it : sceneEntities)
        drawEntityRow(it.first, it.second);
      ImGui::TreePop();
    }

    if (pendingDelete != 0)
      deleteEntity(ctx, pendingDelete);
  }
  ImGui::End();
}

bool VkEditor::drawInspector(Context &ctx) {
  bool edited = false;
  if (ImGui::Begin("Inspector", &mShowInspector, ImGuiWindowFlags_NoCollapse)) {
    auto &reg = ctx.scene.registry();
    auto &s = selection;
    uint32_t id = s.selectedEntityId;

    if (id == 0 || !reg.has<TransformComponent>(id)) {
      ImGui::TextDisabled("No entity selected.");
      ImGui::End();
      return false;
    }

    // ── Entity header ────────────────────────────────────────────────────
    ImGui::Text("Entity %u", id);
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
      deleteEntity(ctx, id);
      ImGui::End();
      return true;
    }
    ImGui::Separator();

    // ── Name ─────────────────────────────────────────────────────────────
    if (reg.has<NameComponent>(id)) {
      auto &nc = reg.get<NameComponent>(id);
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%s", nc.name.c_str());
      if (ImGui::InputText("Name", buf, sizeof(buf))) {
        nc.name = buf;
        edited = true;
      }
    }

    // ── Transform ────────────────────────────────────────────────────────
    if (reg.has<TransformComponent>(id)) {
      bool open = false, wantRemove = false, wantReset = false;
      ComponentHeader("Transform", &open, false, &wantRemove, &wantReset);
      if (wantReset) {
        reg.get<TransformComponent>(id) = TransformComponent{};
        edited = true;
      }
      if (open) {
        auto &tr = reg.get<TransformComponent>(id);
        edited |= DragFloat3Colored("Position", &tr.position.x, 0.1f);
        if (DragFloat3Colored("Rotation", &tr.rotation.x, 0.5f)) {
          normalizeEulerDeg(tr.rotation);
          edited = true;
        }
        edited |= DragFloat3Colored("Scale", &tr.scale.x, 0.01f, 0.01f, 100.0f);
      }
    }

    // ── Mesh ─────────────────────────────────────────────────────────────
    if (reg.has<MeshComponent>(id)) {
      bool open = false, wantRemove = false, wantReset = false;
      ComponentHeader("Mesh", &open, true, &wantRemove, &wantReset);
      if (wantRemove) {
        reg.removeComponent<MeshComponent>(id);
        edited = true;
      } else if (open) {
        auto &mc = reg.get<MeshComponent>(id);
        edited |= ImGui::Checkbox("Visible", &mc.visible);
        ImGui::Checkbox("Casts Shadow", &mc.castsShadow);
        ImGui::TextWrapped("Asset: %s",
                           mc.assetId.empty() ? "(none)" : mc.assetId.c_str());

        const MeshData *data = nullptr;
        if (mc.objHandle.valid())
          data = ctx.assets.getOBJData(mc.objHandle);
        else if (mc.gltfHandle.valid())
          data = ctx.assets.getGLTFData(mc.gltfHandle);
        else if (mc.ufbxHandle.valid())
          data = ctx.assets.getUFBXData(mc.ufbxHandle);
        if (data) {
          ImGui::Text("Submeshes: %zu", data->submeshes.size());
          glm::vec3 mn, mx;
          if (data->getGlobalBounds(mn, mx)) {
            const glm::vec3 size = mx - mn;
            ImGui::Text("Bounds: %.2f x %.2f x %.2f", size.x, size.y, size.z);
          }
        } else {
          ImGui::TextColored(EditorTheme::kWarning, "No parsed mesh data");
        }
      }
    }

    // ── Rigidbody ────────────────────────────────────────────────────────
    if (reg.has<RigidbodyComponent>(id)) {
      bool open = false, wantRemove = false, wantReset = false;
      ComponentHeader("Rigidbody", &open, true, &wantRemove, &wantReset);
      auto removeBody = [&]() {
        auto &rb = reg.get<RigidbodyComponent>(id);
        if (rb.bodyID != 0xFFFFFFFF) {
          ctx.physics.removeBody(rb.bodyID);
          rb.bodyID = 0xFFFFFFFF;
        }
      };
      if (wantRemove) {
        removeBody();
        reg.removeComponent<RigidbodyComponent>(id);
        edited = true;
      } else {
        auto &rb = reg.get<RigidbodyComponent>(id);
        if (wantReset) {
          removeBody();
          rb = RigidbodyComponent{};
          edited = true;
        }
        if (open) {
          const char *types[] = {"Static", "Kinematic", "Dynamic"};
          int type = (int)rb.type;
          if (ImGui::Combo("Type", &type, types, 3)) {
            removeBody(); // recreate with the new motion type
            rb.type = (RigidbodyComponent::Type)type;
            edited = true;
          }
          edited |= ImGui::DragFloat("Mass", &rb.mass, 0.1f, 0.01f, 1000.0f);
          edited |= ImGui::SliderFloat("Friction", &rb.friction, 0.0f, 1.0f);
          edited |=
              ImGui::SliderFloat("Restitution", &rb.restitution, 0.0f, 1.0f);
          ImGui::TextDisabled(
              "Body: %s", rb.bodyID == 0xFFFFFFFF ? "(pending)" : "created");
        }
      }
    }

    // ── Collider ─────────────────────────────────────────────────────────
    if (reg.has<ColliderComponent>(id)) {
      bool open = false, wantRemove = false, wantReset = false;
      ComponentHeader("Collider", &open, true, &wantRemove, &wantReset);
      if (wantRemove) {
        reg.removeComponent<ColliderComponent>(id);
        edited = true;
      } else {
        auto &col = reg.get<ColliderComponent>(id);
        if (wantReset) {
          col = ColliderComponent{};
          edited = true;
        }
        if (open) {
          const char *shapes[] = {"Box", "Sphere", "Capsule"};
          int shape = (int)col.shape;
          if (ImGui::Combo("Shape", &shape, shapes, 3)) {
            col.shape = (ColliderComponent::Shape)shape;
            edited = true;
          }
          edited |= DragFloat3Colored("Offset", &col.offset.x, 0.05f);
          if (col.shape == ColliderComponent::Shape::Box) {
            edited |= DragFloat3Colored("Size", &col.dimensions.x, 0.05f,
                                        0.01f, 500.0f);
          } else {
            edited |= ImGui::DragFloat("Radius", &col.dimensions.x, 0.02f,
                                       0.01f, 100.0f);
            if (col.shape == ColliderComponent::Shape::Capsule)
              edited |= ImGui::DragFloat("Height", &col.dimensions.y, 0.02f,
                                         0.01f, 100.0f);
          }
        }
      }
    }

    // ── Add Component ────────────────────────────────────────────────────
    ImGui::Separator();
    if (ImGui::Button("Add Component", ImVec2(-1, 0)))
      ImGui::OpenPopup("AddComponentPopup");
    if (ImGui::BeginPopup("AddComponentPopup")) {
      if (!reg.has<RigidbodyComponent>(id) &&
          ImGui::MenuItem("Rigidbody")) {
        reg.emplace<RigidbodyComponent>(id);
        edited = true;
      }
      if (!reg.has<ColliderComponent>(id) && ImGui::MenuItem("Collider")) {
        reg.emplace<ColliderComponent>(id);
        edited = true;
      }
      ImGui::EndPopup();
    }
  }
  ImGui::End();
  return edited;
}

void VkEditor::drawAssetsContent(Context &ctx) {
  ImGui::SetNextItemWidth(200);
  ImGui::InputTextWithHint("##AssetSearch", "Filter...", mAssetSearch,
                           sizeof(mAssetSearch));
  ImGui::SameLine();
  if (ImGui::Button("Asset Root"))
    mBrowsePath = ctx.assetDir;
  ImGui::SameLine();
  ImGui::TextDisabled("%s", mBrowsePath.c_str());
  ImGui::Separator();

  namespace fs = std::filesystem;
  std::error_code ec;

  auto matches = [&](const std::string &name) {
    if (mAssetSearch[0] == 0)
      return true;
    std::string a = name, b = mAssetSearch;
    for (auto &c : a)
      c = (char)tolower((unsigned char)c);
    for (auto &c : b)
      c = (char)tolower((unsigned char)c);
    return a.find(b) != std::string::npos;
  };

  if (ImGui::BeginChild("AssetList", ImVec2(0, 0))) {
    fs::path browse(mBrowsePath);
    if (browse.has_parent_path() &&
        browse.lexically_normal() !=
            fs::path(ctx.assetDir).lexically_normal()) {
      if (ImGui::Selectable(".. (up)"))
        mBrowsePath = browse.parent_path().string();
    }

    std::vector<fs::directory_entry> dirs, files;
    for (const auto &entry : fs::directory_iterator(browse, ec)) {
      const std::string name = entry.path().filename().string();
      if (name.rfind("._", 0) == 0)
        continue;
      if (entry.is_directory())
        dirs.push_back(entry);
      else
        files.push_back(entry);
    }
    for (const auto &d : dirs) {
      const std::string name = "[dir] " + d.path().filename().string();
      if (!matches(name))
        continue;
      if (ImGui::Selectable(name.c_str()))
        mBrowsePath = d.path().string();
    }
    for (const auto &f : files) {
      std::string ext = f.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c) { return (char)tolower(c); });
      if (!isModelFile(ext))
        continue;
      const std::string name = f.path().filename().string();
      if (!matches(name))
        continue;
      ImGui::PushID(name.c_str());
      ImGui::Selectable(name.c_str());
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        // Spawn through the real Scene path; VulkanRenderSystem resolves the
        // mesh through the AssetManager next frame.
        std::string full = f.path().generic_string();
        uint32_t e = ctx.scene.spawnFromFile(full);
        if (e != 0) {
          selection.selectedEntityId = e;
          selection.selectedEntities = {e};
          LOG_INFO("Editor", "Spawned: " + full);
        } else {
          LOG_ERROR("Editor", "Failed to spawn: " + full);
        }
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Double-click to spawn");
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
}

void VkEditor::drawLog() {
  if (ImGui::Button("Bottom"))
    mConsoleAutoScroll = true;
  ImGui::SameLine();
  ImGui::Checkbox("Auto-scroll", &mConsoleAutoScroll);
  ImGui::SameLine();

  ImGui::PushStyleColor(ImGuiCol_Button, mFilterInfo
                                             ? ImVec4(0.2f, 0.5f, 0.8f, 1)
                                             : ImVec4(0.2f, 0.2f, 0.2f, 1));
  if (ImGui::SmallButton("Info"))
    mFilterInfo = !mFilterInfo;
  ImGui::PopStyleColor();
  ImGui::SameLine();

  ImGui::PushStyleColor(ImGuiCol_Button, mFilterWarn
                                             ? ImVec4(0.9f, 0.8f, 0.2f, 1)
                                             : ImVec4(0.2f, 0.2f, 0.2f, 1));
  if (ImGui::SmallButton("Warn"))
    mFilterWarn = !mFilterWarn;
  ImGui::PopStyleColor();
  ImGui::SameLine();

  ImGui::PushStyleColor(ImGuiCol_Button, mFilterError
                                             ? ImVec4(0.9f, 0.3f, 0.3f, 1)
                                             : ImVec4(0.2f, 0.2f, 0.2f, 1));
  if (ImGui::SmallButton("Error"))
    mFilterError = !mFilterError;
  ImGui::PopStyleColor();

  ImGui::SameLine();
  ImGui::SetNextItemWidth(200);
  ImGui::InputTextWithHint("##ConsoleSearch", "Search...", mConsoleSearch,
                           sizeof(mConsoleSearch));

  ImGui::Separator();

  if (ImGui::BeginChild("ConsoleScroll", ImVec2(0, 0), 0,
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    const auto entries = Logger::instance().recentEntries(1000);
    for (const auto &e : entries) {
      if (e.level == Logger::Level::Info && !mFilterInfo)
        continue;
      if (e.level == Logger::Level::Warn && !mFilterWarn)
        continue;
      if ((e.level == Logger::Level::Error ||
           e.level == Logger::Level::Fatal) &&
          !mFilterError)
        continue;
      if (e.level == Logger::Level::Trace)
        continue;
      if (mConsoleSearch[0] != 0 &&
          e.message.find(mConsoleSearch) == std::string::npos &&
          e.category.find(mConsoleSearch) == std::string::npos)
        continue;

      ImVec4 color = EditorTheme::kText;
      if (e.level == Logger::Level::Warn)
        color = EditorTheme::kWarning;
      else if (e.level == Logger::Level::Error ||
               e.level == Logger::Level::Fatal)
        color = EditorTheme::kError;

      ImGui::TextColored(color, "[%s] [%s] %s", e.timestamp.c_str(),
                         e.category.c_str(), e.message.c_str());
    }
    if (mConsoleAutoScroll &&
        ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
      ImGui::SetScrollHereY(1.0f);
  }
  ImGui::EndChild();
}

void VkEditor::drawStats(Context &ctx) {
  if (ImGui::Begin("Statistics", &mShowStats, ImGuiWindowFlags_NoCollapse)) {
    const float fps = (ctx.dt > 1e-6f) ? 1.0f / ctx.dt : 0.0f;
    mFpsHistory[mFpsHistoryIdx] = fps;
    mFpsHistoryIdx = (mFpsHistoryIdx + 1) % kFpsHistorySize;

    ImGui::Text("%.1f FPS (%.2f ms)", fps, ctx.dt * 1000.0f);
    ImGui::PlotLines("##fps", mFpsHistory, kFpsHistorySize, mFpsHistoryIdx,
                     nullptr, 0.0f, FLT_MAX, ImVec2(-1, 46));
    ImGui::Separator();

    auto &reg = ctx.scene.registry();
    int total = 0, meshes = 0, bodies = 0;
    for (EntityId e : reg.view<TransformComponent>()) {
      (void)e;
      ++total;
    }
    for (EntityId e : reg.view<MeshComponent>()) {
      (void)e;
      ++meshes;
    }
    for (EntityId e : reg.view<RigidbodyComponent>()) {
      (void)e;
      ++bodies;
    }
    ImGui::Text("Entities: %d", total);
    ImGui::Text("Meshes:   %d", meshes);
    ImGui::Text("Bodies:   %d", bodies);
    ImGui::Separator();
    ImGui::TextDisabled("Backend: Vulkan 1.3 (RT shadows,");
    ImGui::TextDisabled("mesh-shader terrain, bindless)");
  }
  ImGui::End();
}

void VkEditor::drawEnvironment(Context &ctx) {
  if (ImGui::Begin("Environment", &mShowEnvironment,
                   ImGuiWindowFlags_NoCollapse)) {
    vkrhi::VulkanRenderer::Params &p = ctx.renderer.params();
    if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::SliderFloat("Light yaw", &p.lightYawDeg, 0.0f, 360.0f);
      ImGui::SliderFloat("Light pitch", &p.lightPitchDeg, 5.0f, 89.0f);
      ImGui::SliderFloat("Exposure", &p.exposure, 0.1f, 3.0f);
    }
    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::SliderFloat("FOV", &p.fovDeg, 20.0f, 90.0f);
      ImGui::Text("Position %.1f, %.1f, %.1f", p.camPos.x, p.camPos.y,
                  p.camPos.z);
    }
    if (ImGui::CollapsingHeader("Terrain generator",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Checkbox("Draw terrain", &p.drawTerrain);
      ImGui::SliderFloat("Amplitude", &p.terrainAmplitude, 0.0f, 3.0f);
      ImGui::SliderFloat("Frequency", &p.terrainFrequency, 0.05f, 1.5f);
      ImGui::SliderFloat("Octaves", &p.terrainOctaves, 1.0f, 8.0f, "%.0f");
      if (ImGui::Button("Randomize seed"))
        p.terrainSeed = (float)(std::rand() % 1000);
      ImGui::SameLine();
      ImGui::Text("seed %.0f", p.terrainSeed);
    }
  }
  ImGui::End();
}

bool VkEditor::drawGizmo(Context &ctx, const glm::mat4 &view,
                         const glm::mat4 &proj) {
  bool edited = false;

  ImGuizmo::BeginFrame();
  ImGuizmo::SetOrthographic(false);
  ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());

  ImGuiIO &io = ImGui::GetIO();
  ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);

  auto &reg = ctx.scene.registry();
  auto &s = selection;

  if (s.selectedEntityId == 0 || !reg.has<TransformComponent>(s.selectedEntityId))
    return false;

  auto &tr = reg.get<TransformComponent>(s.selectedEntityId);
  glm::mat4 model = tr.getMatrix();

  float snapValues[3] = {toolbar.snapValue, toolbar.snapValue,
                         toolbar.snapValue};
  ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                       (ImGuizmo::OPERATION)s.gizmoOp,
                       (ImGuizmo::MODE)s.gizmoMode, glm::value_ptr(model),
                       nullptr, toolbar.snapEnabled ? snapValues : nullptr);

  if (ImGuizmo::IsUsing()) {
    glm::vec3 t(0.0f), r(0.0f), sc(1.0f);
    if (!decomposeTRSYXZ(model, t, r, sc)) {
      float tf[3], rf[3], scf[3];
      ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(model), tf, rf, scf);
      t = {tf[0], tf[1], tf[2]};
      r = {rf[0], rf[1], rf[2]};
      sc = {scf[0], scf[1], scf[2]};
    }
    tr.position = t;
    tr.rotation = r;
    tr.scale = sc;
    edited = true;
  }
  return edited;
}
