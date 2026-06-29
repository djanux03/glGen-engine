# Experimental Vulkan RHI (Phase 0 of the Vulkan-first rewrite).
# Only added to the build when GLGEN_BUILD_VULKAN=ON; the OpenGL engine is
# untouched otherwise.

find_package(Vulkan REQUIRED)

# --- Vulkan Memory Allocator (header-only) -----------------------------------
include(FetchContent)
FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.1.0
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)
# Use the source include dir directly so we control the single VMA_IMPLEMENTATION TU.
set(VMA_INCLUDE_DIR ${vulkanmemoryallocator_SOURCE_DIR}/include)

# --- Shader compilation: GLSL -> SPIR-V via glslc ----------------------------
if(NOT Vulkan_GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc not found in the Vulkan SDK; cannot compile shaders.")
endif()

set(VK_SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/shaders/vulkan)
set(VK_SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${VK_SHADER_OUT_DIR})

set(VK_SHADERS mesh.vert mesh.frag tonemap.vert tonemap.frag shadow.vert
               terrain.task terrain.mesh terrain.frag)
set(VK_SPV_FILES)
foreach(shader ${VK_SHADERS})
    set(spv ${VK_SHADER_OUT_DIR}/${shader}.spv)
    add_custom_command(
        OUTPUT ${spv}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3
                ${VK_SHADER_SRC_DIR}/${shader} -o ${spv}
        DEPENDS ${VK_SHADER_SRC_DIR}/${shader}
        COMMENT "glslc ${shader} -> SPIR-V"
        VERBATIM
    )
    list(APPEND VK_SPV_FILES ${spv})
endforeach()
add_custom_target(VulkanShaders DEPENDS ${VK_SPV_FILES})

# --- VulkanRHI static library ------------------------------------------------
add_library(VulkanRHI STATIC
    VulkanContext.cpp
    VulkanSwapchain.cpp
    VulkanBindless.cpp
    VulkanPipelineCache.cpp
    VulkanMesh.cpp
    VulkanRenderer.cpp
    vk_mem_alloc_impl.cpp
    vk_stb_write_impl.cpp
)
target_include_directories(VulkanRHI PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VMA_INCLUDE_DIR}
)
# glm (math) and tinyobjloader (OBJ loading) are header-only and carry no
# OpenGL dependency, so VulkanRHI stays decoupled from the GL engine.
# GLM_FORCE_DEPTH_ZERO_TO_ONE makes glm::perspective emit Vulkan's [0,1] depth.
target_link_libraries(VulkanRHI PUBLIC Vulkan::Vulkan glm tinyobjloader stb)
target_compile_definitions(VulkanRHI PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
)
add_dependencies(VulkanRHI VulkanShaders)

# --- ImGui for Vulkan (core + glfw platform + vulkan renderer, no OpenGL) -----
file(GLOB IMGUI_VK_SOURCES
    ${CMAKE_SOURCE_DIR}/ThirdParty/imgui/src/*.cpp
    ${CMAKE_SOURCE_DIR}/ThirdParty/imgui/backends/imgui_impl_glfw.cpp
    ${CMAKE_SOURCE_DIR}/ThirdParty/imgui/backends/imgui_impl_vulkan.cpp
)
list(FILTER IMGUI_VK_SOURCES EXCLUDE REGEX [[(^|[\\/])\._]])
add_library(imgui_vk STATIC ${IMGUI_VK_SOURCES})
target_include_directories(imgui_vk PUBLIC
    ${CMAKE_SOURCE_DIR}/ThirdParty/imgui/include
    ${CMAKE_SOURCE_DIR}/ThirdParty/imgui/backends
)
target_link_libraries(imgui_vk PUBLIC glfw Vulkan::Vulkan)

# --- Phase 0 smoke-test executable -------------------------------------------
add_executable(glGenVulkanSmoke smoke/main.cpp)
target_link_libraries(glGenVulkanSmoke PRIVATE VulkanRHI glfw Vulkan::Vulkan imgui_vk)
target_compile_definitions(glGenVulkanSmoke PRIVATE
    GLGEN_VK_SHADER_DIR="${VK_SHADER_OUT_DIR}"
    GLGEN_VK_MODEL_PATH="${CMAKE_SOURCE_DIR}/assets/playerassets/axe.obj")
add_dependencies(glGenVulkanSmoke VulkanShaders)
