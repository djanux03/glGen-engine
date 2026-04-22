# ThirdParty CMakeLists.txt

# GLAD
add_library(glad STATIC glad/src/glad.c)
target_include_directories(glad PUBLIC glad/include)

# GLM - header only
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE glm/include)

# GLFW
include(FetchContent)

option(GLGEN_USE_FETCHED_GLFW "Fetch GLFW instead of relying on a system package" ON)

if(GLGEN_USE_FETCHED_GLFW)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "GLFW docs" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "GLFW examples" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "GLFW tests" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "GLFW install" FORCE)

    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
    )
    FetchContent_MakeAvailable(glfw)
else()
    find_package(glfw3 CONFIG REQUIRED)
endif()

# ImGui
file(GLOB IMGUI_SOURCES 
    imgui/src/*.cpp 
    imgui/src/ImGuizmo.cpp
    imgui/backends/imgui_impl_glfw.cpp 
    imgui/backends/imgui_impl_opengl3.cpp
)
list(FILTER IMGUI_SOURCES EXCLUDE REGEX [[(^|[\\/])\._]])
add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC imgui/include imgui/backends)
target_link_libraries(imgui PUBLIC glfw)
# ImGui needs OpenGL
find_package(OpenGL REQUIRED)
target_link_libraries(imgui PUBLIC OpenGL::GL)

# STB - header only but we have impl file
add_library(stb STATIC stb/src/stb_image_impl.cpp)
target_include_directories(stb PUBLIC stb/include stb/include/stb)

# TinyGLTF
add_library(tinygltf STATIC src/tiny_gltf_impl.cpp)
target_include_directories(tinygltf PUBLIC include)
target_link_libraries(tinygltf PUBLIC stb)

# ufbx
add_library(ufbx STATIC src/ufbx.c)
target_include_directories(ufbx PUBLIC include)

# TinyObjLoader
add_library(tinyobjloader INTERFACE)
target_include_directories(tinyobjloader INTERFACE include)

# Lua 5.4
file(GLOB LUA_SOURCES lua/src/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX [[(^|[\\/])\._]])
# Remove standalone executables (lua.c = interpreter, luac.c = compiler)
list(FILTER LUA_SOURCES EXCLUDE REGEX "lua/src/lua\\.c$")
list(FILTER LUA_SOURCES EXCLUDE REGEX "lua/src/luac\\.c$")
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC lua/src)
# Silence warnings in Lua source
if(MSVC)
    target_compile_options(lua PRIVATE /W0)
else()
    target_compile_options(lua PRIVATE -w)
endif()

# sol2 - header-only C++ Lua bindings
add_library(sol2 INTERFACE)
target_include_directories(sol2 INTERFACE sol2/include)
target_link_libraries(sol2 INTERFACE lua)

# Jolt Physics
set(TARGET_UNIT_TESTS OFF CACHE BOOL "Build Unit Tests" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "Build Hello World" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "Build Performance Test" FORCE)
add_subdirectory(JoltPhysics/Build JoltPhysicsBuild)
