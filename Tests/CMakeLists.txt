cmake_minimum_required(VERSION 3.14)
# C is enabled for Lua's sources (the ScriptJson conversion tests need a real
# Lua VM); everything else here is C++.
project(glGen-Tests LANGUAGES C CXX)

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
  test_editor_state.cpp
  test_gameplay_state.cpp
  test_terrain_noise.cpp
  test_terrain_chunk_manager.cpp
  test_terrain_scatter.cpp
  test_terrain_brush.cpp
  test_asset_manager.cpp
  test_generators.cpp
  test_script_json.cpp
  test_command_server.cpp
  test_mesh_decimate.cpp
  test_mesh_validate.cpp

  # Engine sources needed by SubsystemManager and ProjectConfig tests
  "${ENGINE_DIR}/Core/SubsystemManager.cpp"
  "${ENGINE_DIR}/Core/ProjectConfig.cpp"
  "${ENGINE_DIR}/Core/Logger.cpp"

  # Engine sources needed by terrain noise/query/streaming/scatter tests
  "${ENGINE_DIR}/Terrain/TerrainNoise.cpp"
  "${ENGINE_DIR}/Terrain/TerrainQuery.cpp"
  "${ENGINE_DIR}/Terrain/TerrainChunkMesher.cpp"
  "${ENGINE_DIR}/Terrain/TerrainChunkManager.cpp"
  "${ENGINE_DIR}/Terrain/ScatterManifest.cpp"
  "${ENGINE_DIR}/Terrain/TerrainScatter.cpp"
  "${ENGINE_DIR}/Terrain/HeightOffsetGrid.cpp"
  "${ENGINE_DIR}/Assets/MeshPrimitives.cpp"

  # Engine sources needed by the AssetManager tests. The file parsers it
  # calls are stubbed in test_asset_manager.cpp -- see the note there.
  "${ENGINE_DIR}/Assets/AssetManager.cpp"

  # Procedural generator pipeline (AI_ASSET_PIPELINE_PLAN.md Phase 1).
  "${ENGINE_DIR}/Generators/MeshBuilder.cpp"
  "${ENGINE_DIR}/Generators/GeneratorSchema.cpp"
  "${ENGINE_DIR}/Generators/GeneratorRegistry.cpp"
  "${ENGINE_DIR}/Generators/AssetRecipe.cpp"
  "${ENGINE_DIR}/Generators/TextureGen.cpp"
  "${ENGINE_DIR}/Generators/AssetLibrary.cpp"
  "${ENGINE_DIR}/Generators/gen/Builtins.cpp"
  "${ENGINE_DIR}/Generators/gen/GenTree.cpp"
  "${ENGINE_DIR}/Generators/gen/GenRock.cpp"
  "${ENGINE_DIR}/Generators/gen/GenGrass.cpp"
  "${ENGINE_DIR}/Generators/gen/GenKitbash.cpp"
  "${ENGINE_DIR}/Generators/MeshDecimate.cpp"
  "${ENGINE_DIR}/Generators/MeshValidate.cpp"
  "${ENGINE_DIR}/Generators/gen/GenImport.cpp"

  # Command port (AI_ASSET_PIPELINE_PLAN.md Phase 3). Tested over a real
  # loopback socket, since the framing and deferred-reply mechanism are what
  # clients actually depend on.
  "${ENGINE_DIR}/Bridge/CommandServer.cpp"
)

if(WIN32)
  target_link_libraries(glgen_tests PRIVATE ws2_32)
endif()

# ── Lua, for the ScriptJson conversion tests ────────────────────────────────
# Only ScriptJson.h is exercised here, not ScriptBindings.h -- the latter pulls
# in GLFW and the ECS, which this deliberately loader-free test target has no
# business linking. The conversion rules are the error-prone part anyway.
file(GLOB TEST_LUA_SOURCES "${THIRDPARTY_DIR}/lua/src/*.c")
list(FILTER TEST_LUA_SOURCES EXCLUDE REGEX [[(^|[\\/])\._]])
list(FILTER TEST_LUA_SOURCES EXCLUDE REGEX "lua/src/lua\\.c$")
list(FILTER TEST_LUA_SOURCES EXCLUDE REGEX "lua/src/luac\\.c$")
add_library(test_lua STATIC ${TEST_LUA_SOURCES})
target_include_directories(test_lua PUBLIC "${THIRDPARTY_DIR}/lua/src")
if(MSVC)
  target_compile_options(test_lua PRIVATE /W0)
else()
  target_compile_options(test_lua PRIVATE -w)
endif()
target_link_libraries(glgen_tests PRIVATE test_lua)
target_include_directories(glgen_tests PRIVATE
  "${THIRDPARTY_DIR}/sol2/include"
  "${ENGINE_DIR}/Scripting"
)

target_include_directories(glgen_tests PRIVATE
  "${ENGINE_DIR}"
  "${ENGINE_DIR}/Core"
  "${ENGINE_DIR}/ECS"
  "${ENGINE_DIR}/Terrain"
  "${ENGINE_DIR}/Assets"
  "${ENGINE_DIR}/Bridge"
  "${ENGINE_DIR}/Generators"
  "${ENGINE_DIR}/Rendering"
  "${CMAKE_CURRENT_SOURCE_DIR}/../VulkanRHI/runtime"
  "${THIRDPARTY_DIR}/include"      # json.hpp lives here
  "${THIRDPARTY_DIR}/imgui/include"
  "${THIRDPARTY_DIR}/glm/include"     # glm for GameplayState.h
)

# TerrainChunkManager uses std::thread.
find_package(Threads REQUIRED)
target_link_libraries(glgen_tests PRIVATE Threads::Threads)

target_link_libraries(glgen_tests PRIVATE doctest::doctest)

# ── CTest integration ───────────────────────────────────────────────────────
include(CTest)
enable_testing()
add_test(NAME glgen_tests COMMAND glgen_tests)
