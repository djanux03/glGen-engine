cmake_minimum_required(VERSION 3.10)
project(glGen VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
  add_compile_options(/EHsc)
endif()

# Output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Fetch cpr for HTTP requests (Laravel Backend Integration)
include(FetchContent)
FetchContent_Declare(
  cpr
  GIT_REPOSITORY https://github.com/libcpr/cpr.git
  GIT_TAG        1.10.5
)
FetchContent_MakeAvailable(cpr)

# Add subdirectories
add_subdirectory(ThirdParty)
add_subdirectory(Engine)
add_subdirectory(Editor)
add_subdirectory(Runtime)

# ── Experimental Vulkan backend (Phase 0 of the Vulkan-first rewrite) ────────
# OFF by default: the OpenGL build (build.ps1 / build.sh) is unaffected.
# Enable with:  cmake -S . -B <dir> -DGLGEN_BUILD_VULKAN=ON
option(GLGEN_BUILD_VULKAN "Build the experimental Vulkan RHI + smoke test" OFF)
if(GLGEN_BUILD_VULKAN)
  add_subdirectory(VulkanRHI)
endif()
