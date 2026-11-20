# The 64-bit helper of the upscale bridge (src/pc/upscale_bridge.h), built on its own: the ports are 32-bit builds.
#   cmake -S external/xport/tools/upscale64 -B build/upscale64 -A x64
#   cmake --build build/upscale64 --config Release
# With DLSS: -DDLSS_SDK=<NVIDIA's DLSS SDK, github.com/NVIDIA/DLSS>. The SDK is under NVIDIA's own licence: it stays
# outside this repository; nvngx_dlss.dll is copied next to the program. `upscale64 --probe` says whether DLSS runs.
cmake_minimum_required(VERSION 3.25)
project(upscale64 LANGUAGES CXX)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "upscale64 is the 64-bit helper: configure it with -A x64")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# The port it ships with, for its version information (Task Manager shows "<port> DLSS helper").
set(UPSCALE64_PRODUCT "xport" CACHE STRING "the port it belongs to, as in its version information")
set(UPSCALE64_VERSION "0.0.0" CACHE STRING "that port's version, X.Y.Z")
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" version_ok "${UPSCALE64_VERSION}")
if(version_ok STREQUAL "")  # (not if(NOT ...): "0.0.0" reads as the number 0)
    message(FATAL_ERROR "UPSCALE64_VERSION must be X.Y.Z, not ${UPSCALE64_VERSION}")
endif()
set(UPSCALE64_VERSION_MAJOR ${CMAKE_MATCH_1})
set(UPSCALE64_VERSION_MINOR ${CMAKE_MATCH_2})
set(UPSCALE64_VERSION_PATCH ${CMAKE_MATCH_3})
configure_file(upscale64_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/upscale64_version.h @ONLY)

add_executable(upscale64 main.cpp upscale64.rc)
target_include_directories(upscale64 PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(upscale64 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(upscale64 PRIVATE d3d11 dxgi d3dcompiler)

set(DLSS_SDK "" CACHE PATH "NVIDIA DLSS SDK (include, lib/Windows_x86_64); empty: no DLSS")
if(DLSS_SDK)
    target_compile_definitions(upscale64 PRIVATE XPORT_DLSS)
    target_include_directories(upscale64 PRIVATE ${DLSS_SDK}/include)
    target_link_libraries(upscale64 PRIVATE
        $<IF:$<CONFIG:Debug>,${DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_s_dbg.lib,${DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib>)
    # The DLL beside the program, when the SDK has it (the CI fetches only what the build needs).
    if(EXISTS ${DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlss.dll)
        add_custom_command(TARGET upscale64 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlss.dll $<TARGET_FILE_DIR:upscale64>)
    endif()
endif()
