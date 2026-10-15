# trials_hd - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
if(APPLE)
    project(trials_hd LANGUAGES CXX OBJC OBJCXX)
else()
    project(trials_hd LANGUAGES CXX)
endif()

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

# Browser build (docs/web-port/README.md): only with -DTRIALSHD_WEB=ON under
# Emscripten's toolchain (preset web-wasm64-release). It has its own build
# description and never reaches the native one below.
option(TRIALSHD_WEB "Build for the browser (Emscripten, WebAssembly + WebGPU)" OFF)
if(TRIALSHD_WEB)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "TRIALSHD_WEB needs Emscripten's toolchain: "
                            "source ~/emsdk/emsdk_env.sh && emcmake cmake --preset web-wasm64-release")
    endif()
    include(cmake/web/trials_hd_web.cmake)
    return()
endif()

include(generated/rexglue.cmake)

# The SDK sets REXGLUE_ROOT inside its own add_subdirectory scope but doesn't
# propagate it to the parent. The macOS MoltenVK ICD staging helper needs it.
if(NOT REXGLUE_ROOT AND REXSDK_DIR)
    set(REXGLUE_ROOT "${REXSDK_DIR}")
endif()

# Sources
set(TRIALSHD_SOURCES
    src/main.cpp
    src/fallback_audio.cpp
    src/graphics_settings.cpp
    src/keyboard_controls.cpp
    src/fps_unlock.cpp
    src/gpu_wait.cpp
    src/achievements_hook.cpp
    src/crt_hooks.cpp
    src/offline_warning.cpp
    src/package_license.cpp
    src/virtual_pad.cpp
    src/exit_button.cpp
    src/splash_screen.cpp
)

if(WIN32)
    add_executable(trials_hd WIN32 ${TRIALSHD_SOURCES})
else()
    add_executable(trials_hd ${TRIALSHD_SOURCES})
endif()

# Native WebGPU renderer plugin (docs/webgpu-renderer/README.md), the default
# renderer; the Vulkan one (xenos) is always built too, as the fallback. Needs
# Dawn: tools/build_dawn.sh installs it to out/dawn-install, and the first
# configure runs it when no Dawn is found. -DTRIALSHD_WEBGPU=OFF skips it.
option(TRIALSHD_WEBGPU "Build the WebGPU renderer plugin (rexgpu-webgpu)" ON)
set(TRIALSHD_GPU_PLUGINS xenos)
if(TRIALSHD_WEBGPU)
    find_package(Threads REQUIRED)  # Dawn's exported targets reference Threads::Threads
    list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/out/dawn-install")
    find_package(Dawn QUIET)
    if(NOT Dawn_FOUND)
        message(STATUS "Dawn not found: building it with tools/build_dawn.sh (first configure only; "
                       "downloads Dawn and takes a while)")
        execute_process(COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_dawn.sh"
                        RESULT_VARIABLE _dawn_result)
        if(NOT _dawn_result EQUAL 0)
            message(FATAL_ERROR "Building Dawn failed (see above). Configure with "
                                "-DTRIALSHD_WEBGPU=OFF to build with the Vulkan renderer only.")
        endif()
        find_package(Dawn REQUIRED)
    endif()
    # Must exist before rexglue_setup_target, which stages in-tree plugins.
    add_subdirectory(src/gpu_webgpu)
    list(APPEND TRIALSHD_GPU_PLUGINS webgpu)
endif()

rexglue_setup_target(trials_hd GPU_PLUGINS ${TRIALSHD_GPU_PLUGINS})

# A fresh checkout has no generated code when CMake configures, so there is
# nothing to compile the game from yet (CMake only lists generated sources that
# exist at configure time). The first build then only runs codegen and marks
# the configuration stale; the next build re-runs CMake and compiles it all.
if(NOT REXGLUE_ENTRYPOINT_GENERATED_SOURCES)
    set(TRIALSHD_CODEGEN_READY "${CMAKE_BINARY_DIR}/trials_hd_codegen_ready.stamp")
    file(WRITE "${TRIALSHD_CODEGEN_READY}" "")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TRIALSHD_CODEGEN_READY}")
    set_target_properties(trials_hd PROPERTIES EXCLUDE_FROM_ALL ON)
    add_custom_target(trials_hd_first_codegen ALL
        COMMAND "${CMAKE_COMMAND}" -E touch "${TRIALSHD_CODEGEN_READY}"
        COMMAND "${CMAKE_COMMAND}" -E echo
                "trials_hd: game code generated - run the same build command again to compile it"
        VERBATIM)
    add_dependencies(trials_hd_first_codegen trials_hd_codegen)
endif()

# The SDK's rexglue_configure_target() (called from rexglue_setup_target above)
# stages the shared runtime libraries next to the executable only on Windows and
# macOS (plus the MoltenVK Vulkan stack on macOS), and stages the GPU plugin
# only when trials_hd relinks, so an SDK-only change would leave stale copies.
# The binary's RPATH is $ORIGIN / @executable_path, so copy them alongside it on
# every build; copy_if_different keeps this cheap.
if(NOT WIN32)
    set(TRIALSHD_RUNTIME_LIBS $<TARGET_FILE:rexruntime> $<TARGET_FILE:rexgpu-xenos>)
    set(TRIALSHD_RUNTIME_LIB_TARGETS rexruntime rexgpu-xenos)
    if(TARGET rexgpu-webgpu)
        list(APPEND TRIALSHD_RUNTIME_LIBS $<TARGET_FILE:rexgpu-webgpu>)
        list(APPEND TRIALSHD_RUNTIME_LIB_TARGETS rexgpu-webgpu)
    endif()
    if(TARGET TracyClient)
        get_target_property(_tracy_type TracyClient TYPE)
        if(_tracy_type STREQUAL "SHARED_LIBRARY")
            list(APPEND TRIALSHD_RUNTIME_LIBS $<TARGET_FILE:TracyClient>)
            list(APPEND TRIALSHD_RUNTIME_LIB_TARGETS TracyClient)
        endif()
    endif()
    add_custom_target(trials_hd_stage_libs
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TRIALSHD_RUNTIME_LIBS}
                $<TARGET_FILE_DIR:trials_hd>
        DEPENDS ${TRIALSHD_RUNTIME_LIB_TARGETS}
        COMMENT "Staging runtime libraries next to trials_hd"
        VERBATIM)
    add_dependencies(trials_hd trials_hd_stage_libs)
endif()

# rex_app.cpp (compiled into this target by the SDK) includes imgui.h, but
# rexruntime links imgui privately, so the header path does not propagate when
# the SDK is built from source. Take only the include directory: linking
# imgui::imgui would add a second copy of ImGui's global context.
target_include_directories(trials_hd PRIVATE
    $<TARGET_PROPERTY:imgui::imgui,INTERFACE_INCLUDE_DIRECTORIES>)

# The SDK forces BUILD_TESTING off; keep project tests independent of that cache entry.
option(TRIALSHD_BUILD_TESTING "Build project regression tests" ON)
if(TRIALSHD_BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    enable_testing()
    add_executable(frame_clock_test tests/frame_clock_test.cpp)
    target_include_directories(frame_clock_test PRIVATE src)
    add_test(NAME frame_clock COMMAND frame_clock_test)
    add_executable(render_pose_test tests/render_pose_test.cpp)
    target_include_directories(render_pose_test PRIVATE src)
    add_test(NAME render_pose COMMAND render_pose_test)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME fps_log COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_fps_log_test.py)
        add_test(NAME render_pose_log COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_render_pose_log_test.py)
    endif()
endif()

# Development tool: dumps the loaded XEX image for writing codegen overrides.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/xex_dump")
    add_executable(xex_dump EXCLUDE_FROM_ALL tools/xex_dump/xex_dump.cpp)
    target_link_libraries(xex_dump PRIVATE rex::runtime)
endif()

# Development tool: Xbox 360 STFS package extractor (W-S13).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/stfs_extract")
    add_library(stfs_extract_lib STATIC tools/stfs_extract/stfs_extract.cpp)
    target_include_directories(stfs_extract_lib PUBLIC tools/stfs_extract)
    add_executable(stfs_extract EXCLUDE_FROM_ALL tools/stfs_extract/main.cpp)
    target_link_libraries(stfs_extract PRIVATE stfs_extract_lib)
endif()

# Standalone check of the WebGPU stack: window, surface, features/limits, and CPU benchmark.
if(TRIALSHD_WEBGPU)
    find_package(Threads REQUIRED)
    find_package(Dawn REQUIRED)
    add_library(trials_hd_webgpu STATIC src/webgpu/webgpu_context.cpp)
    target_include_directories(trials_hd_webgpu PUBLIC src)
    target_link_libraries(trials_hd_webgpu PUBLIC dawn::webgpu_dawn SDL3::SDL3)

    add_executable(webgpu_smoke tools/webgpu_smoke/main.cpp)
    target_link_libraries(webgpu_smoke PRIVATE trials_hd_webgpu)
endif()

# Reverse-engineering aid: count calls of every recompiled function in an address
# range (default: the Direct3D runtime) and log them per second to the CSV named
# by TRIALS_HD_CALL_TRACE_CSV. See docs/webgpu-renderer/README.md section 6. Needs a codegen'd tree.
option(TRIALSHD_CALL_TRACE "Build per-function call counters (diagnostics)" OFF)
set(TRIALSHD_CALL_TRACE_RANGE "0x8212B000;0x82170000" CACHE STRING "Traced address range (lo;hi)")
if(TRIALSHD_CALL_TRACE)
    set(_register "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/trials_hd_register.cpp")
    if(NOT EXISTS "${_register}")
        message(FATAL_ERROR "TRIALSHD_CALL_TRACE needs generated code: build once without it first")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_shims "${CMAKE_CURRENT_BINARY_DIR}/diag/call_trace_shims.cpp")
    list(GET TRIALSHD_CALL_TRACE_RANGE 0 _lo)
    list(GET TRIALSHD_CALL_TRACE_RANGE 1 _hi)
    add_custom_command(
        OUTPUT "${_shims}"
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_trace_shims.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/generated/default" "${CMAKE_CURRENT_SOURCE_DIR}/src"
                "${_shims}" ${_lo} ${_hi}
        DEPENDS "${_register}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_trace_shims.py"
        VERBATIM)
    target_sources(trials_hd PRIVATE src/diag/call_trace.cpp "${_shims}")
    target_compile_definitions(trials_hd PRIVATE TRIALSHD_CALL_TRACE=1)
endif()
