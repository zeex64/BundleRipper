cmake_minimum_required(VERSION 3.20)
project(Spotbuilder LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# One self-contained exe: static CRT, no runtime DLLs to ship.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(xl_third_party STATIC
    third_party/lz4/lz4.c
    third_party/lzma/LzmaDec.c
    third_party/miniz/miniz.c
    third_party/t2d/astc.cpp
    third_party/t2d/etc.cpp
    third_party/meshoptimizer/allocator.cpp
    third_party/meshoptimizer/indexgenerator.cpp
    third_party/meshoptimizer/simplifier.cpp
    third_party/meshoptimizer/vcacheoptimizer.cpp
    third_party/meshoptimizer/vfetchoptimizer.cpp)
target_include_directories(xl_third_party PUBLIC
    third_party/lz4 third_party/lzma third_party/miniz third_party/bcdec third_party/crunch third_party/t2d
    third_party/meshoptimizer)
target_compile_definitions(xl_third_party PUBLIC MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME)
if(MSVC)
    target_compile_options(xl_third_party PRIVATE /W0)
endif()

add_library(xl_imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(xl_imgui PUBLIC third_party/imgui)
target_compile_definitions(xl_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
if(MSVC)
    target_compile_options(xl_imgui PRIVATE /W0 /utf-8)
endif()

add_executable(Spotbuilder
    src/main.cpp
    src/job.cpp
    src/options.cpp
    src/gui.cpp
    src/scene_view.cpp
    src/unity.cpp
    src/mesh.cpp
    src/texture.cpp
    src/images.cpp
    src/scene.cpp
    src/gltf.cpp
    src/curves_obj.cpp
    src/grindlines.cpp
    src/optimize.cpp
    src/plants.cpp
    src/edits.cpp
    src/json_parse.cpp
    src/studio_build.cpp
    src/sky.cpp
    res/Spotbuilder.rc)
# The .rc embeds the Blender script; rebuild it when the script changes.
set_source_files_properties(res/Spotbuilder.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/blender/make_blend.py")
target_link_libraries(Spotbuilder PRIVATE xl_third_party xl_imgui d3d11 d3dcompiler dxgi dwmapi shell32 ole32 uuid)
if(MSVC)
    target_compile_options(Spotbuilder PRIVATE /W3 /utf-8 /permissive- /MP)
    target_compile_definitions(Spotbuilder PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    # The crunch decoder header is noisy.
    set_source_files_properties(src/texture.cpp PROPERTIES COMPILE_OPTIONS "/W0")
endif()
