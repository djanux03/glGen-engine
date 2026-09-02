cmake_minimum_required(VERSION 3.10)
project(glGen VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
  add_compile_options(/EHsc)
  # Force a consistent MSVC runtime library project-wide (CACHE+FORCE so it
  # propagates reliably into add_subdirectory'd projects, notably Jolt's own
  # CMakeLists which otherwise defaults to /MT while everything else here
  # uses CMake's implicit /MD default, causing LNK2038 "RuntimeLibrary
  # mismatch" on any from-scratch configure. Pre-existing inconsistency,
  # unrelated to OpenGL removal -- just never surfaced until a clean rebuild
  # hit it.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "" FORCE)
  # Jolt's own CMakeLists.txt (ThirdParty/JoltPhysics/Build) wholesale-
  # replaces CMAKE_CXX_FLAGS_DEBUG/RELEASE when this is on (its default),
  # which drops the runtime-library marker CMake needs to emit a correct
  # <RuntimeLibrary> for those two configs specifically -- leaving it empty
  # and falling back to MSVC's static default there, reintroducing the same
  # mismatch even with the override above. Force it off.
  set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
endif()

# Output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Add subdirectories
add_subdirectory(ThirdParty)
add_subdirectory(Engine)
add_subdirectory(VulkanRHI)
add_subdirectory(Tools/glgen-bake)

# ── Tests (opt-in) ──────────────────────────────────────────────────────────
option(GLGEN_BUILD_TESTS "Build the glGen test suite" OFF)
if(GLGEN_BUILD_TESTS)
  add_subdirectory(Tests)
endif()
