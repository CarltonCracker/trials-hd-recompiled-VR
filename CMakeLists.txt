# trials_hd - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(trials_hd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Local fixes to the ReXGlue SDK live in patches/rexglue-sdk so the submodule can
# keep tracking upstream. Apply any that are not applied yet.
find_package(Git REQUIRED)
file(GLOB TRIALSHD_SDK_PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/rexglue-sdk/*.patch")
list(SORT TRIALSHD_SDK_PATCHES)
foreach(_patch IN LISTS TRIALSHD_SDK_PATCHES)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${_patch}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk
        RESULT_VARIABLE _already_applied OUTPUT_QUIET ERROR_QUIET)
    if(NOT _already_applied EQUAL 0)
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

# The SDK builds its shared libraries into its own out/ directory and only
# stages them next to the executable on Windows. The binary's RPATH is $ORIGIN,
# so copy them alongside it here.
set(TRIALSHD_RUNTIME_LIBS rexruntime)
if(TARGET TracyClient)
    get_target_property(_tracy_type TracyClient TYPE)
    if(_tracy_type STREQUAL "SHARED_LIBRARY")
        list(APPEND TRIALSHD_RUNTIME_LIBS TracyClient)
    endif()
endif()
foreach(_lib IN LISTS TRIALSHD_RUNTIME_LIBS)
    add_custom_command(TARGET trials_hd POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${_lib}> $<TARGET_FILE_DIR:trials_hd>
        VERBATIM)
endforeach()

# rex_app.cpp (compiled into this target by the SDK) includes imgui.h, but
# rexruntime links imgui privately, so the header path does not propagate when
# the SDK is built from source. Take only the include directory: linking
# imgui::imgui would add a second copy of ImGui's global context.
target_include_directories(trials_hd PRIVATE
    $<TARGET_PROPERTY:imgui::imgui,INTERFACE_INCLUDE_DIRECTORIES>)

# Development tool: dumps the loaded XEX image for writing codegen overrides.
add_executable(xex_dump EXCLUDE_FROM_ALL tools/xex_dump/xex_dump.cpp)
target_link_libraries(xex_dump PRIVATE rex::runtime)

# WebGPU renderer (in development, see docs/webgpu-renderer.md). Needs Dawn:
# build thirdparty/dawn and point CMAKE_PREFIX_PATH at its install prefix, e.g.
#   -DTRIALSHD_WEBGPU=ON -DCMAKE_PREFIX_PATH=$PWD/out/dawn-install
option(TRIALSHD_WEBGPU "Build the WebGPU renderer and its tools" OFF)
if(TRIALSHD_WEBGPU)
    find_package(Threads REQUIRED)  # Dawn's exported targets reference Threads::Threads
    find_package(Dawn REQUIRED)
    add_library(trials_hd_webgpu STATIC src/webgpu/webgpu_context.cpp)
    target_include_directories(trials_hd_webgpu PUBLIC src)
    target_link_libraries(trials_hd_webgpu PUBLIC dawn::webgpu_dawn SDL3::SDL3)

    # Standalone check of the WebGPU stack: window, device, surface, a triangle.
    add_executable(webgpu_smoke tools/webgpu_smoke/main.cpp)
    target_link_libraries(webgpu_smoke PRIVATE trials_hd_webgpu)
endif()
