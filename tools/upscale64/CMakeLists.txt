# The 64-bit helper of the upscale bridge (src/pc/upscale_bridge.h), built on its own: the ports are 32-bit builds.
#   cmake -S external/xport/tools/upscale64 -B build/upscale64 -A x64
#   cmake --build build/upscale64 --config Release
cmake_minimum_required(VERSION 3.25)
project(upscale64 LANGUAGES CXX)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "upscale64 is the 64-bit helper: configure it with -A x64")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_executable(upscale64 main.cpp)
target_compile_definitions(upscale64 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(upscale64 PRIVATE d3d11 dxgi d3dcompiler)
