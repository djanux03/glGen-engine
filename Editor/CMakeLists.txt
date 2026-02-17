# Editor CMakeLists.txt

# Currently Editor only has EditorUI, which might be linked into Runtime or separate.
# The user design had Editor as a separate folder.
# Inspecting EditorUI.cpp suggests it's a component, not a standalone app yet.
# For now, let's make it a library that Runtime can link if needed, 
# OR just leave it empty if there is no main entry point for Editor yet.
# But wait, EditorUI implementation was used by App.cpp.
# So App.cpp depends on EditorUI.

file(GLOB EDITOR_SOURCES "*.cpp")

if(EDITOR_SOURCES)
    add_library(Editor STATIC ${EDITOR_SOURCES})
    target_include_directories(Editor PUBLIC .)
    target_link_libraries(Editor PUBLIC Engine)
else()
    add_library(Editor INTERFACE)
endif()
