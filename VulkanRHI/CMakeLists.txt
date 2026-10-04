# VulkanRHI — the engine's renderer (OpenGL has been removed from the repo).

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

set(VK_SHADERS mesh.vert mesh.frag meshInstanced.vert meshInstanced.frag
               fogVisibility.comp fogInject.comp fogHistory.comp fogIntegrate.comp fogComposite.frag bloomPyramid.comp skyAtmosphere.comp cloudShadow.comp skyIrradiance.comp cloudHistory.comp exposureHistogram.comp
               depthAlpha.frag
                 tonemap.vert tonemap.frag temporal.frag
               terrainChunk.vert terrainChunk.frag sky.vert sky.frag
               ssao.frag blur.frag bloomExtract.frag bloomBlur.frag
               volumetric.frag volumetricComposite.frag water.frag
               clouds.frag
               line.vert line.frag)
set(VK_SPV_FILES)
foreach(shader ${VK_SHADERS})
    set(spv ${VK_SHADER_OUT_DIR}/${shader}.spv)
    add_custom_command(
        OUTPUT ${spv}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3
                ${VK_SHADER_SRC_DIR}/${shader} -o ${spv}
        DEPENDS ${VK_SHADER_SRC_DIR}/${shader}
                ${VK_SHADER_SRC_DIR}/frameData.glsl
                ${VK_SHADER_SRC_DIR}/skyModel.glsl
                ${VK_SHADER_SRC_DIR}/grassMaterial.glsl
               ${VK_SHADER_SRC_DIR}/biomeLighting.glsl
                ${VK_SHADER_SRC_DIR}/paintMaterial.glsl
                ${VK_SHADER_SRC_DIR}/fog.glsl
                ${VK_SHADER_SRC_DIR}/snowMaterial.glsl
                ${VK_SHADER_SRC_DIR}/surfaceShadow.glsl
                ${VK_SHADER_SRC_DIR}/atmosphereData.glsl
                ${VK_SHADER_SRC_DIR}/fogSample.glsl
                ${VK_SHADER_SRC_DIR}/fogMedium.glsl
                ${VK_SHADER_SRC_DIR}/skyLutModel.glsl
                ${VK_SHADER_SRC_DIR}/skyLutSample.glsl
                ${VK_SHADER_SRC_DIR}/cloudDensity.glsl
                ${VK_SHADER_SRC_DIR}/cloudShadow.glsl
                ${VK_SHADER_SRC_DIR}/skyIrradiance.glsl
                ${VK_SHADER_SRC_DIR}/skySHBasis.glsl
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
    VulkanAccel.cpp
    VulkanRenderer.cpp
    VulkanAtmosphereRenderer.cpp
    VulkanCloudHistory.cpp
    VulkanExposureMeter.cpp
    VulkanRenderSystem.cpp
    vk_mem_alloc_impl.cpp
)
# Note: tinyobjloader + stb_image_write implementations come from EngineCore's
# parsers and the tinygltf lib respectively — do not compile them again here.
target_include_directories(VulkanRHI PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VMA_INCLUDE_DIR}
)
# EngineCore supplies the GL-free engine systems (ECS, Scene, AssetManager
# with CPU MeshData) that VulkanRenderSystem drives — no OpenGL anywhere in
# this dependency chain.
# GLM_FORCE_DEPTH_ZERO_TO_ONE makes glm::perspective emit Vulkan's [0,1] depth.
target_link_libraries(VulkanRHI PUBLIC Vulkan::Vulkan glm tinyobjloader stb EngineCore)
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
# The engine's (now GL-free) ECS headers drive the scene. Header-only: no Engine
# link needed, just the include paths.
target_include_directories(glGenVulkanSmoke PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/ECS
    ${CMAKE_SOURCE_DIR}/Engine/Assets)
target_compile_definitions(glGenVulkanSmoke PRIVATE
    GLGEN_VK_SHADER_DIR="${VK_SHADER_OUT_DIR}"
    GLGEN_VK_MODEL_PATH="${CMAKE_SOURCE_DIR}/assets/playerassets/axe.obj"
    GLGEN_VK_MODEL_PATH2="${CMAKE_SOURCE_DIR}/assets/terraingeneratorassets/rock.obj"
    GLGEN_VK_MODEL_PATH3="${CMAKE_SOURCE_DIR}/assets/terraingeneratorassets/tree.obj")
add_dependencies(glGenVulkanSmoke VulkanShaders)

# --- glGenVk: the engine's runtime (EngineCore + VulkanRHI) ------------------
add_executable(glGenVk
    runtime/main.cpp
    runtime/VkEditor.cpp
    runtime/VkScriptBindings.cpp
    runtime/VkAgentBridge.cpp
    runtime/gameplay/VkPlayerControllerSystem.cpp
    runtime/gameplay/VkPlayerInteractionSystem.cpp
    runtime/gameplay/VkRifleSystem.cpp
    runtime/gameplay/VkSpaceshipControlSystem.cpp
    runtime/subsystems/VkWindowSubsystem.cpp
    runtime/subsystems/VkPhysicsSubsystem.cpp
    runtime/subsystems/VkScriptSubsystem.cpp
    runtime/subsystems/VkEditorSubsystem.cpp
    runtime/subsystems/VkCoreAppLayer.cpp
    runtime/subsystems/VkAudioSubsystem.cpp
    runtime/subsystems/VkTerrainSubsystem.cpp)
target_link_libraries(glGenVk PRIVATE VulkanRHI EngineCore glfw Vulkan::Vulkan imgui_vk)
target_include_directories(glGenVk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
target_compile_definitions(glGenVk PRIVATE
    GLGEN_VK_SHADER_DIR="${VK_SHADER_OUT_DIR}"
    GLGEN_VK_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
    GLGEN_VK_SCRIPT_DIR="${CMAKE_SOURCE_DIR}/scripts")
add_dependencies(glGenVk VulkanShaders)
