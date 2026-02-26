# Engine CMakeLists.txt

file(GLOB_RECURSE ENGINE_SOURCES 
    "Core/*.cpp"
    "Rendering/*.cpp"
    "Input/*.cpp"
    "Scene/*.cpp"
    "Assets/*.cpp"
    "ECS/*.cpp"
    "Scripting/*.cpp"
)

add_library(Engine STATIC ${ENGINE_SOURCES})

# Include directories for Engine itself
# '.' (Engine root) is needed for cross-subdirectory includes like "ECS/Registry.h" from Scene/
target_include_directories(Engine PUBLIC 
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

# Link ThirdParty dependencies (Engine does NOT depend on Editor)
target_link_libraries(Engine PUBLIC 
    glad 
    glm 
    imgui 
    stb 
    tinygltf 
    ufbx 
    tinyobjloader 
    glfw
    sol2
    Jolt
)

# Platform specific libs
if(APPLE)
    target_link_libraries(Engine PUBLIC "-framework Cocoa -framework IOKit -framework CoreVideo")
endif()
