# trials_hd - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(trials_hd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Local changes to the ReXGlue SDK live in patches/rexglue-sdk/trials-hd.patch
# (regenerated with tools/export_sdk_patch.sh) so the submodule can keep
# tracking upstream. Apply it to a clean checkout; a tree with other local
# changes is left alone (SDK development in progress).
find_package(Git REQUIRED)
file(GLOB TRIALSHD_SDK_PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/rexglue-sdk/*.patch")
list(SORT TRIALSHD_SDK_PATCHES)
foreach(_patch IN LISTS TRIALSHD_SDK_PATCHES)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${_patch}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk
        RESULT_VARIABLE _already_applied OUTPUT_QUIET ERROR_QUIET)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk
        OUTPUT_VARIABLE _sdk_changes OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _already_applied EQUAL 0 AND NOT _sdk_changes STREQUAL "")
        message(STATUS "SDK tree has local changes; not re-applying ${_patch}")
    elseif(NOT _already_applied EQUAL 0)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply ${_patch}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk
            RESULT_VARIABLE _apply_result)
        if(NOT _apply_result EQUAL 0)
            message(FATAL_ERROR "Failed to apply SDK patch ${_patch}")
        endif()
        message(STATUS "Applied SDK patch ${_patch}")
    endif()
endforeach()

# AddressSanitizer build for memory-error hunting (SDK, libraries and the app;
# the generated guest code is excluded below - it only touches guest memory,
# and instrumenting it makes the build and the game far slower).
# Configure in its own build dir with its own SDK output dir, e.g.
#   cmake --preset linux-amd64-relwithdebinfo -B out/build/asan -DTRIALSHD_ASAN=ON \
#     -DREXGLUE_OUTPUT_DIR=$PWD/out/build/asan/sdk
option(TRIALSHD_ASAN "Build with AddressSanitizer (diagnostics)" OFF)
if(TRIALSHD_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

include(generated/rexglue.cmake)

# Sources
set(TRIALSHD_SOURCES
    src/main.cpp
    src/fallback_audio.cpp
    src/graphics_settings.cpp
    src/main_menu.cpp
    src/package_license.cpp
    src/virtual_pad.cpp
)

if(WIN32)
    add_executable(trials_hd WIN32 ${TRIALSHD_SOURCES})
else()
    add_executable(trials_hd ${TRIALSHD_SOURCES})
endif()

rexglue_setup_target(trials_hd GPU_PLUGINS xenos)

if(TRIALSHD_ASAN AND TARGET trials_hd_recomp)
    target_compile_options(trials_hd_recomp PRIVATE -fno-sanitize=address)
endif()

# The SDK builds its shared libraries into its own out/ directory and only
# stages them next to the executable on Windows (and stages the GPU plugin only
# when trials_hd relinks). The binary's RPATH is $ORIGIN, so copy them alongside
# it on every build; copy_if_different keeps this cheap.
set(TRIALSHD_RUNTIME_LIBS $<TARGET_FILE:rexruntime> $<TARGET_FILE:rexgpu-xenos>)
set(TRIALSHD_RUNTIME_LIB_TARGETS rexruntime rexgpu-xenos)
if(TARGET TracyClient)
    get_target_property(_tracy_type TracyClient TYPE)
    if(_tracy_type STREQUAL "SHARED_LIBRARY")
        list(APPEND TRIALSHD_RUNTIME_LIBS $<TARGET_FILE:TracyClient>)
        list(APPEND TRIALSHD_RUNTIME_LIB_TARGETS TracyClient)
    endif()
endif()
add_custom_target(trials_hd_stage_libs
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TRIALSHD_RUNTIME_LIBS}
            ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${TRIALSHD_RUNTIME_LIB_TARGETS}
    COMMENT "Staging runtime libraries next to trials_hd"
    VERBATIM)
add_dependencies(trials_hd trials_hd_stage_libs)

# rex_app.cpp (compiled into this target by the SDK) includes imgui.h, but
# rexruntime links imgui privately, so the header path does not propagate when
# the SDK is built from source. Take only the include directory: linking
# imgui::imgui would add a second copy of ImGui's global context.
target_include_directories(trials_hd PRIVATE
    $<TARGET_PROPERTY:imgui::imgui,INTERFACE_INCLUDE_DIRECTORIES>)

# Development tool: dumps the loaded XEX image for writing codegen overrides.
add_executable(xex_dump EXCLUDE_FROM_ALL tools/xex_dump/xex_dump.cpp)
target_link_libraries(xex_dump PRIVATE rex::runtime)
