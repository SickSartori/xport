# sick-xport: the parts of a native Xbox port that do not depend on the game (disc and XBE reading, the
# kernel, memory, threads, the NV2A's registers and textures, paths, logs, crash reports, profiling).
#
# A game includes this file from its CMakeLists.txt (the library sits in external/sick-xport as a git
# submodule) and compiles XPORT_RUNTIME_SOURCES into its own runtime DLL, beside its own sources: the library and
# the game call each other's functions (kernel_internal.h, nv2a.h, audio_mixer.h), so they build as one target, with
# the game's flags and link-time optimisation across both. Then it names itself with xport_identity.
#
#   include(external/sick-xport/xport.cmake)
#   add_library(mc3runtime SHARED ${XPORT_RUNTIME_SOURCES} src/pc/...)
#   target_link_libraries(mc3runtime PRIVATE xportcore ...)
#   xport_identity(mc3runtime mc3port MC3PORT MC3_)
#   xport_add_tests(mc3port MC3PORT MC3_)            # the library's own tests in the game's ctest

set(XPORT_DIR ${CMAKE_CURRENT_LIST_DIR})

# Portable core, no platform headers: disc image and XBE parsing, kernel export table.
add_library(xportcore STATIC
    ${XPORT_DIR}/src/core/kernel_exports.cpp
    ${XPORT_DIR}/src/core/xbe.cpp
    ${XPORT_DIR}/src/core/xdvdfs.cpp
)
target_include_directories(xportcore PUBLIC ${XPORT_DIR}/src)

set(XPORT_RUNTIME_SOURCES
    ${XPORT_DIR}/src/pc/crash_report.cpp
    ${XPORT_DIR}/src/pc/exec_memory.cpp
    ${XPORT_DIR}/src/pc/fs_patch.cpp
    ${XPORT_DIR}/src/pc/game_pause.cpp
    ${XPORT_DIR}/src/pc/gamepad.cpp
    ${XPORT_DIR}/src/pc/handles.cpp
    ${XPORT_DIR}/src/pc/kernel.cpp
    ${XPORT_DIR}/src/pc/kernel_crypto.cpp
    ${XPORT_DIR}/src/pc/kernel_dispatcher.cpp
    ${XPORT_DIR}/src/pc/kernel_memory.cpp
    ${XPORT_DIR}/src/pc/kernel_system.cpp
    ${XPORT_DIR}/src/pc/launcher_music.cpp
    ${XPORT_DIR}/src/pc/log.cpp
    ${XPORT_DIR}/src/pc/mmio.cpp
    ${XPORT_DIR}/src/pc/nv2a_combiner.cpp
    ${XPORT_DIR}/src/pc/nv2a_texture.cpp
    ${XPORT_DIR}/src/pc/nv2a_vsh.cpp
    ${XPORT_DIR}/src/pc/paths.cpp
    ${XPORT_DIR}/src/pc/physical_memory.cpp
    ${XPORT_DIR}/src/pc/privileged.cpp
    ${XPORT_DIR}/src/pc/profile.cpp
    ${XPORT_DIR}/src/pc/renderer_d3d11.cpp
    ${XPORT_DIR}/src/pc/texture_upscale.cpp
    ${XPORT_DIR}/src/pc/xbox_thread.cpp
)

# The game's names in the library's files and switches (src/pc/port.h). Also used for its tests.
function(xport_identity target name name_upper env_prefix)
    target_compile_definitions(${target} PRIVATE XPORT_NAME="${name}" XPORT_NAME_UPPER="${name_upper}"
                                                 XPORT_ENV_PREFIX="${env_prefix}")
endfunction()

# The library's tests, added to the game's ctest with the game's identity (the game drops its copies of them).
function(xport_add_tests name name_upper env_prefix)
    function(xport_test test)
        add_executable(${test} ${XPORT_DIR}/tests/${test}.cpp ${ARGN})
        target_link_libraries(${test} PRIVATE xportcore)
        target_include_directories(${test} PRIVATE ${XPORT_DIR}/tests)
        target_compile_definitions(${test} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        target_compile_options(${test} PRIVATE /utf-8)
        xport_identity(${test} ${name} ${name_upper} ${env_prefix})
        set_target_properties(${test} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
        add_test(NAME ${test} COMMAND ${test})
    endfunction()
    xport_test(test_core)
    xport_test(test_log ${XPORT_DIR}/src/pc/log.cpp)
    xport_test(test_texture_pages)
    xport_test(test_textures ${XPORT_DIR}/src/pc/nv2a_texture.cpp ${XPORT_DIR}/src/pc/texture_upscale.cpp)
    xport_test(test_update_check)
    xport_test(test_data_folder ${XPORT_DIR}/src/pc/paths.cpp)
    xport_test(test_crash_report ${XPORT_DIR}/src/pc/crash_report.cpp)
endfunction()
