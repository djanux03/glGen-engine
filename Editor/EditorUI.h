#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <string>

// Forward declarations
class SunFX;
class FireFX;
class Scene; 
class OBJModel;

struct EditorUIOutput
{
    bool wantCaptureMouse = false;
    bool wantCaptureKeyboard = false;
    bool terrainDirty = false;

    float skyYaw01 = 0.0f;
    glm::vec3 skyRotDeg = glm::vec3(0.0f);
    bool saveRequested = false; 
    bool loadRequested = false;
};

// We need a small struct to pass Scene/Selection state cleanly to the Gizmo drawer
// (Alternatively, pass these fields individually, but a struct is cleaner)
struct EditorSelectionState
{
    int& selectedEntityIndex;
    std::vector<int>& selectedEntities;
    int& lastClickedEntity;
    
    // OBJ part selection
    bool& editObjPart;
    std::string& selectedObjPartName;

    // Gizmo state
    int& gizmoOp;   // Cast to ImGuizmo::OPERATION internally
    int& gizmoMode; // Cast to ImGuizmo::MODE internally

    // Tools
    bool& renaming;
    char* renameBuf;       // Pointer to char[128]
    char* outlinerFilter;  // Pointer to char[128]
    float& focusDistance;
};

class EditorUI
{
public:
    // Existing main UI drawer
    EditorUIOutput draw(
        bool& uiMode,
        float& walkStep, float& runMult,
        float& jumpStrength, float& gravity, bool& freezePhysics,
        float& mouseSensitivity, float& fov,
        SunFX& sun,
        FireFX& fire,
        int& terrainSize,
        float& terrainSpacing,
        glm::vec3& treePos,
        glm::vec3& treeScale
        
    );

    // NEW: Gizmo & Outliner drawer (moved from App.cpp)
    void drawGizmo(
        bool uiMode,
        const glm::mat4& view,
        const glm::mat4& projection,
        Scene& scene,
        SunFX& sun,
        EditorSelectionState& sel, // Pass selection state by reference
        glm::vec3& cameraPos       // To update camera when focusing
    );
};
