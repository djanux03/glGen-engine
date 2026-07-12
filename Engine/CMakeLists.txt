# Engine CMakeLists.txt
#
# Two layers:
#   EngineCore — graphics-API-free engine systems (ECS, Scene, physics,
#                scripting, input, CPU asset parsing). Links NO OpenGL;
#                this is what the Vulkan runtime links.
#   Engine     — the OpenGL rendering layer + GL-coupled systems, layered
#                on top of EngineCore. Existing GL targets keep linking
#                Engine and see the same API as before.

# ---- EngineCore (no OpenGL) -------------------------------------------------
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
# "ECS/Registry.h" from Scene/. Rendering is exposed for the backend-neutral
# headers that live there (Material.h) — EngineCore compiles no Rendering
# sources and links no GL.
target_include_directories(EngineCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    Core
    Rendering
    Input
    Scene
    Assets
    ECS
    Scripting
    Terrain
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

# ---- Engine (OpenGL rendering layer) ---------------------------------------
file(GLOB_RECURSE ENGINE_GL_SOURCES CONFIGURE_DEPENDS
    "Rendering/*.cpp"
    "Terrain/*.cpp"
)
list(APPEND ENGINE_GL_SOURCES
    Assets/OBJModel.cpp
    Assets/FBXModel.cpp
    Assets/UFBXModel.cpp
    ECS/Systems/DestructionSystem.cpp
)
list(FILTER ENGINE_GL_SOURCES EXCLUDE REGEX [[(^|[\\/])\._]])

add_library(Engine STATIC ${ENGINE_GL_SOURCES})

target_link_libraries(Engine PUBLIC
    EngineCore
    glad
    imgui
)

# Platform specific libs
if(APPLE)
    target_link_libraries(Engine PUBLIC "-framework Cocoa -framework IOKit -framework CoreVideo")
endif()
