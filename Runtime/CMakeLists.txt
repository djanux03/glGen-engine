# Runtime CMakeLists.txt

file(GLOB_RECURSE RUNTIME_SOURCES 
    "Main/*.cpp"
    "Gameplay/*.cpp"
    "Framework/*.cpp"
)

add_executable(glGen ${RUNTIME_SOURCES})

target_include_directories(glGen PRIVATE Framework Gameplay ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/include)

target_link_libraries(glGen PRIVATE Engine Editor cpr::cpr)

if(APPLE)
    target_link_libraries(glGen PRIVATE "-framework Cocoa -framework IOKit -framework CoreVideo")
endif()
