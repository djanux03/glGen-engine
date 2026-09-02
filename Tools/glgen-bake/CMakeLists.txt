# glgen-bake — offline mesh conditioning CLI (see main.cpp for why it exists).
# Links EngineCore for the parsers and the quadric decimator; no graphics API,
# no window, so it runs anywhere the engine builds.
add_executable(glgen-bake main.cpp)
target_link_libraries(glgen-bake PRIVATE EngineCore glm)
target_include_directories(glgen-bake PRIVATE ${CMAKE_SOURCE_DIR}/Engine)
