# Runtime CMakeLists.txt

file(GLOB_RECURSE RUNTIME_SOURCES 
    "Main/*.cpp"
    "Gameplay/*.cpp"
    "Framework/*.cpp"
)

add_executable(glGen ${RUNTIME_SOURCES})

target_include_directories(glGen PRIVATE Framework Gameplay)

target_link_libraries(glGen PRIVATE Engine Editor)

if(APPLE)
    target_link_libraries(glGen PRIVATE "-framework Cocoa -framework IOKit -framework CoreVideo")
endif()
