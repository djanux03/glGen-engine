# Engine CMakeLists.txt
#
# EngineCore — graphics-API-free engine systems (ECS, Scene, physics,
# scripting, input, CPU asset parsing). Links no graphics API; this is what
# the Vulkan runtime (glGenVk) links. OpenGL has been removed from the repo.

# ---- EngineCore --------------------------------------------------------
set(ENGINE_CORE_SOURCES
    Core/CrashHandler.cpp
    Core/EngineAssert.cpp
    Core/Logger.cpp
    Core/ProjectConfig.cpp
    Core/SubsystemManager.cpp
    Scene/Scene.cpp
    Input/keyboard.cpp
    Input/mouse.cpp
    Assets/AssetManager.cpp
    Assets/MeshParseOBJ.cpp
    Assets/MeshParseGLTF.cpp
    Assets/MeshParseFBX.cpp
    Assets/MeshPrimitives.cpp
    ECS/Systems/PhysicsSystem.cpp
    ECS/Systems/PhysicsTerrainChunk.cpp
)

add_library(EngineCore STATIC ${ENGINE_CORE_SOURCES})

# '.' (Engine root) is needed for cross-subdirectory includes like
# "ECS/Registry.h" from Scene/. Rendering is exposed for the one
# backend-neutral header that lives there (Material.h) — EngineCore compiles
# no Rendering sources and links no graphics API.
target_include_directories(EngineCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    Core
    Rendering
    Input
    Scene
    Assets
    ECS
    Scripting
    ../ThirdParty/JoltPhysics
)

target_link_libraries(EngineCore PUBLIC
    glm
    stb
    tinygltf
    ufbx
    tinyobjloader
    glfw
    sol2
    Jolt
)
