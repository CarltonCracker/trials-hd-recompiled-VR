# trials_hd - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0069 NEW)
if(APPLE)
    project(trials_hd LANGUAGES CXX OBJC OBJCXX)
else()
    project(trials_hd LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Local changes to the ReXGlue SDK (patches/rexglue-sdk/*.patch): applied to the
# submodule, or updated after a pull. See cmake/sdk_patches.cmake.
include(cmake/sdk_patches.cmake)

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

# Tracy profiler: avoid per-draw profiling checks in optimized builds.
# -DREXGLUE_ENABLE_TRACY=ON opts back in; multi-config builds keep the SDK default.
# The same for the SDK's perf counters (only its debug overlay reads them; the
# shared atomic counters cost ~2% of the game's render thread):
# -DREXGLUE_ENABLE_PERF_COUNTERS=ON opts back in.
get_property(_trialshd_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _trialshd_multi_config AND CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "Enable Tracy profiler integration")
    set(REXGLUE_ENABLE_PERF_COUNTERS OFF CACHE BOOL
        "Enable lightweight perf counter registry and overlay")
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
    src/performance_overlay.cpp
    src/ui_theme.cpp
    src/keyboard_controls.cpp
    src/fps_unlock.cpp
    src/gpu_wait.cpp
    src/achievements_hook.cpp
    src/crt_hooks.cpp
    src/offline_warning.cpp
    src/custom_track_list.cpp
    src/package_license.cpp
    src/virtual_pad.cpp
    src/exit_button.cpp
    src/gamepad_nav.cpp
    src/installer/stfs_package.cpp
    src/installer/game_installer.cpp
    src/installer/installer_dialog.cpp
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

# The game code is generated from the player's default.xex (build.sh extracts
# it from their package to out/xex/). Its SHA-1 goes into the binary so the
# setup screen only installs the version the code was generated from.
set(TRIALSHD_BUILD_XEX "${CMAKE_CURRENT_SOURCE_DIR}/out/xex/default.xex")
set(TRIALSHD_BUILD_XEX_SHA1 "")
if(EXISTS "${TRIALSHD_BUILD_XEX}")
    file(SHA1 "${TRIALSHD_BUILD_XEX}" TRIALSHD_BUILD_XEX_SHA1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TRIALSHD_BUILD_XEX}")
endif()
configure_file(cmake/trials_hd_build_info.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/build_info/trials_hd_build_info.h" @ONLY)
target_include_directories(trials_hd PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/build_info"
    "${REXSDK_DIR}/thirdparty/crypto")  # TinySHA1.hpp

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
    add_executable(imgui_sharpness_test tests/imgui_sharpness_test.cpp
        src/performance_overlay.cpp src/ui_theme.cpp)
    target_link_libraries(imgui_sharpness_test PRIVATE rex::runtime)
    target_include_directories(imgui_sharpness_test PRIVATE src
        $<TARGET_PROPERTY:imgui::imgui,INTERFACE_INCLUDE_DIRECTORIES>)
    add_test(NAME imgui_sharpness COMMAND imgui_sharpness_test)
    add_executable(guest_ui_text_test tests/guest_ui_text_test.cpp)
    target_include_directories(guest_ui_text_test PRIVATE src)
    target_link_libraries(guest_ui_text_test PRIVATE rex::runtime)
    add_test(NAME guest_ui_text COMMAND guest_ui_text_test)
    # Bookkeeping of the vulkan_gpu_profile timestamp profiler (header only, no device).
    add_executable(vk_gpu_profile_test tests/vk_gpu_profile_test.cpp)
    target_link_libraries(vk_gpu_profile_test PRIVATE rex::runtime)
    add_test(NAME vk_gpu_profile COMMAND vk_gpu_profile_test)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME fps_log COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_fps_log_test.py)
        add_test(NAME render_pose_log COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_render_pose_log_test.py)
        add_test(NAME tracy_default COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/tracy_default_test.py ${CMAKE_COMMAND})
    endif()
    # The game installer on synthetic packages, against tools/add_dlc.py. Its
    # own build_info header stands in for the build's default.xex hash.
    add_executable(installer_cli tests/installer/installer_cli.cpp
                   src/installer/game_installer.cpp src/installer/stfs_package.cpp)
    target_include_directories(installer_cli PRIVATE src tests/installer
                               "${REXSDK_DIR}/thirdparty/crypto")
    target_link_libraries(installer_cli PRIVATE rex::runtime)
    if(Python3_Interpreter_FOUND)
        add_test(NAME installer COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/installer/test_installer.py
                 $<TARGET_FILE:installer_cli>)
    endif()
endif()

# Development tool: dumps the loaded XEX image for writing codegen overrides.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/xex_dump")
    add_executable(xex_dump EXCLUDE_FROM_ALL tools/xex_dump/xex_dump.cpp)
    target_link_libraries(xex_dump PRIVATE rex::runtime)
endif()

# The game installer's STFS reader (src/installer/stfs_package.cpp) as a CLI:
# build.sh takes default.xex out of the player's package with it before codegen
# (it needs no generated code or SDK). Also lists and extracts packages (W-S13).
add_executable(stfs_extract EXCLUDE_FROM_ALL tools/stfs_extract/main.cpp
               src/installer/stfs_package.cpp)
target_include_directories(stfs_extract PRIVATE src)

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
