cmake_minimum_required(VERSION 3.14)
project(glGen-Tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
  add_compile_options(/EHsc)
endif()

# ── doctest via FetchContent ─────────────────────────────────────────────────
# doctest v2.4.11 uses cmake_minimum_required(VERSION 2.8.12) which modern
# CMake rejects.  This policy override lets it configure cleanly.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

include(FetchContent)
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.11
)
FetchContent_MakeAvailable(doctest)

# ── Paths ────────────────────────────────────────────────────────────────────
# Support building standalone (cmake -S Tests -B Build-tests) or as
# add_subdirectory(Tests) from the root project.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # Standalone build — resolve engine paths relative to this directory
  set(ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Engine")
  set(THIRDPARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty")
else()
  set(ENGINE_DIR "${CMAKE_SOURCE_DIR}/Engine")
  set(THIRDPARTY_DIR "${CMAKE_SOURCE_DIR}/ThirdParty")
endif()

# ── Test executable ──────────────────────────────────────────────────────────
# Tests deliberately do NOT link the Engine library.
# We compile only the minimal .cpp files needed.
add_executable(glgen_tests
  test_main.cpp
  test_sparse_set.cpp
  test_registry.cpp
  test_event_bus.cpp
  test_perlin_noise.cpp
  test_subsystem_manager.cpp
  test_project_config.cpp
  test_terrain_brush.cpp
  test_editor_state.cpp
  test_gameplay_state.cpp

  # Engine sources needed by SubsystemManager and ProjectConfig tests
  "${ENGINE_DIR}/Core/SubsystemManager.cpp"
  "${ENGINE_DIR}/Core/ProjectConfig.cpp"
  "${ENGINE_DIR}/Core/Logger.cpp"
)

target_include_directories(glgen_tests PRIVATE
  "${ENGINE_DIR}"
  "${ENGINE_DIR}/Core"
  "${ENGINE_DIR}/ECS"
  "${CMAKE_CURRENT_SOURCE_DIR}/../Runtime/Framework"
  "${THIRDPARTY_DIR}/include"      # json.hpp lives here
  "${THIRDPARTY_DIR}/imgui/include"
  "${THIRDPARTY_DIR}/glm/include"     # glm for GameplayState.h
)

target_link_libraries(glgen_tests PRIVATE doctest::doctest)

# ── CTest integration ───────────────────────────────────────────────────────
include(CTest)
enable_testing()
add_test(NAME glgen_tests COMMAND glgen_tests)
