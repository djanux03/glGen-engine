# Runtime CMakeLists.txt

file(GLOB_RECURSE RUNTIME_SOURCES 
    "Main/*.cpp"
    "Gameplay/*.cpp"
    "Framework/*.cpp"
)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX [[(^|[\\/])\._]])

add_executable(glGen ${RUNTIME_SOURCES})

target_include_directories(glGen PRIVATE Framework Gameplay ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/include)

target_link_libraries(glGen PRIVATE Engine Editor cpr::cpr)

if(WIN32)
    set(GLGEN_D3D_REDIST_DIR "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64")
    foreach(GLGEN_RUNTIME_DLL dxcompiler.dll dxil.dll)
        if(EXISTS "${GLGEN_D3D_REDIST_DIR}/${GLGEN_RUNTIME_DLL}")
            add_custom_command(TARGET glGen POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${GLGEN_D3D_REDIST_DIR}/${GLGEN_RUNTIME_DLL}"
                    "$<TARGET_FILE_DIR:glGen>/${GLGEN_RUNTIME_DLL}"
            )
        endif()
    endforeach()
endif()

if(APPLE)
    target_link_libraries(glGen PRIVATE "-framework Cocoa -framework IOKit -framework CoreVideo -framework CoreFoundation -framework AudioToolbox -framework CoreAudio")
endif()
