# Engine CMakeLists.txt

file(GLOB_RECURSE ENGINE_SOURCES 
    "Core/*.cpp"
    "Rendering/*.cpp"
    "Input/*.cpp"
    "Scene/*.cpp"
    "Assets/*.cpp"
)

add_library(Engine STATIC ${ENGINE_SOURCES})

# Include directories for Engine itself
target_include_directories(Engine PUBLIC 
    Core 
    Rendering 
    Input 
    Scene 
    Assets
)

# Link ThirdParty dependencies
target_link_libraries(Engine PUBLIC 
    glad 
    glm 
    imgui 
    stb 
    tinygltf 
    ufbx 
    tinyobjloader 
    glfw
    Editor
)

# Platform specific libs
if(APPLE)
    target_link_libraries(Engine PUBLIC "-framework Cocoa -framework IOKit -framework CoreVideo")
endif()
