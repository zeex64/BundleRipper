cmake_minimum_required(VERSION 3.20)
project(BundleRipper LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# One self-contained exe: static CRT, no runtime DLLs to ship.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(xl_third_party STATIC
    third_party/lz4/lz4.c
    third_party/lzma/LzmaDec.c
    third_party/miniz/miniz.c
    third_party/t2d/astc.cpp
    third_party/t2d/etc.cpp)
target_include_directories(xl_third_party PUBLIC
    third_party/lz4 third_party/lzma third_party/miniz third_party/bcdec third_party/crunch third_party/t2d)
target_compile_definitions(xl_third_party PUBLIC MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME)
if(MSVC)
    target_compile_options(xl_third_party PRIVATE /W0)
endif()

add_executable(BundleRipper
    src/main.cpp
    src/unity.cpp
    src/mesh.cpp
    src/texture.cpp
    src/images.cpp
    src/scene.cpp
    src/gltf.cpp
    src/curves_obj.cpp
    src/grindlines.cpp)
target_link_libraries(BundleRipper PRIVATE xl_third_party)
if(MSVC)
    target_compile_options(BundleRipper PRIVATE /W3 /utf-8 /permissive- /MP)
    target_compile_definitions(BundleRipper PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    # The crunch decoder header is noisy.
    set_source_files_properties(src/texture.cpp PROPERTIES COMPILE_OPTIONS "/W0")
endif()
