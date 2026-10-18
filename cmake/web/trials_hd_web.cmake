# Web (Emscripten) build of Trials HD Recompiled. docs/web-port/README.md,
# sections 7.8, 7.9.11 and 10.
#
# The top-level CMakeLists.txt includes this file only with -DTRIALSHD_WEB=ON
# under Emscripten's toolchain (preset web-wasm64-release), then stops, so a
# native configure never reads it. It compiles the SDK, the recompiled game and
# the app from explicit source lists - like src/gpu_webgpu does for the SDK's
# GPU sources - rather than the SDK's CMake, whose platform setup assumes
# Windows, Linux or macOS (Vulkan, SDL, Tracy, a shared rexruntime).
#
# It reads generated/default (produced by a native build) and never runs or
# rewrites codegen. Build outputs stay in the build folder and are private
# (they contain the user's game code): never publish them.

enable_language(C)

set(_root ${CMAKE_CURRENT_SOURCE_DIR})
set(_sdk ${_root}/thirdparty/rexglue-sdk)
set(_tp ${_sdk}/thirdparty)
set(_gen ${_root}/generated/default)

if(NOT EXISTS ${_gen}/sources.cmake OR NOT EXISTS ${_gen}/trials_hd_pch.h)
    message(FATAL_ERROR
        "generated/default is missing. The web build compiles the code that the native "
        "build generates: build the native game first (docs/web-port/README.md 10.1).")
endif()

option(TRIALSHD_WEB_NODE
       "Also run under Node.js with direct host file access (development: WM1 boot checks)" OFF)

#=============================================================================
# Flags for everything
#=============================================================================
# wasm64 with shared memory (D1), real pthreads (D4), SIMD for SIMDe, native
# wasm exceptions. -ffp-contract=off: the recompiled code needs the guest's
# unfused multiply-adds (natively -ffp-model=strict, which has no meaning here).
set(TRIALSHD_WEB_CODEGEN_FLAGS -sMEMORY64=1 -pthread -msimd128 -fwasm-exceptions)
add_compile_options(
    ${TRIALSHD_WEB_CODEGEN_FLAGS}
    -fno-strict-aliasing
    -ffp-contract=off
    $<$<COMPILE_LANGUAGE:CXX>:-fno-char8_t>
    -Wno-unused-command-line-argument
)
add_link_options(${TRIALSHD_WEB_CODEGEN_FLAGS})
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

add_compile_definitions(
    SPDLOG_COMPILED_LIB
    SPDLOG_FMT_EXTERNAL
    REXGLUE_BUILD_CONFIG="$<CONFIG>"
)

# rex/version.h, as the SDK's CMake configures it.
include(${_sdk}/cmake/rex_version.cmake)
rex_resolve_version(REXGLUE_FULL_VERSION FLOOR_MAJOR 0 FLOOR_MINOR 10 SOURCE_DIR ${_sdk})
string(REGEX REPLACE "-.*$" "" REXGLUE_NUMERIC_VERSION "${REXGLUE_FULL_VERSION}")
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)(\\.([0-9]+))?(\\.([0-9]+))?" _ "${REXGLUE_NUMERIC_VERSION}")
set(REXGLUE_DERIVED_MAJOR "${CMAKE_MATCH_1}")
set(REXGLUE_DERIVED_MINOR "${CMAKE_MATCH_2}")
if(CMAKE_MATCH_4)
    set(REXGLUE_DERIVED_PATCH "${CMAKE_MATCH_4}")
else()
    set(REXGLUE_DERIVED_PATCH "0")
endif()
if(CMAKE_MATCH_6)
    set(REXGLUE_DERIVED_TWEAK "${CMAKE_MATCH_6}")
else()
    set(REXGLUE_DERIVED_TWEAK "0")
endif()
set(REXGLUE_FLOOR_VERSION
    "${REXGLUE_DERIVED_MAJOR}.${REXGLUE_DERIVED_MINOR}.${REXGLUE_DERIVED_PATCH}")
rex_version_channel(REXGLUE_VERSION_CHANNEL VERSION "${REXGLUE_FULL_VERSION}")
set(REX_PLATFORM "web-wasm64")
string(TIMESTAMP REXGLUE_BUILD_TIMESTAMP "%Y%m%d_%H%M")
configure_file(${_sdk}/include/rex/version.h.in ${CMAKE_BINARY_DIR}/include/rex/version.h @ONLY)

include_directories(
    ${_sdk}/include
    ${CMAKE_BINARY_DIR}/include
    ${_tp}
)
include_directories(SYSTEM
    ${_tp}/simde
    ${_tp}/fmt/include
    ${_tp}/spdlog/include
    ${_tp}/tomlplusplus/include
    ${_tp}/sdl3/include
    ${_tp}/cli11/include
    ${_tp}/imgui
    ${_tp}/xxHash
    ${_tp}/libmspack/cabextract/mspack
    ${_tp}/tiny-aes-c
    ${_tp}/utfcpp/source
    ${_tp}/disruptorplus/include
    ${_tp}/o1heap/o1heap
    ${_tp}/renderdoc
    ${_tp}/stb
    ${_tp}/FFmpeg
)

#=============================================================================
# Third-party libraries (the subset the runtime uses)
#=============================================================================
add_library(trials_hd_web_thirdparty STATIC
    ${_tp}/fmt/src/format.cc
    ${_tp}/fmt/src/os.cc
    ${_tp}/spdlog/src/spdlog.cpp
    ${_tp}/spdlog/src/stdout_sinks.cpp
    ${_tp}/spdlog/src/color_sinks.cpp
    ${_tp}/spdlog/src/file_sinks.cpp
    ${_tp}/spdlog/src/async.cpp
    ${_tp}/spdlog/src/cfg.cpp
    ${_tp}/tiny-aes-c/aes.c
    ${_tp}/aes_128/aes.c
    ${_tp}/libmspack/cabextract/mspack/lzxd.c
    ${_tp}/o1heap/o1heap/o1heap.c
    ${_tp}/xxHash/xxhash.c
)
target_compile_definitions(trials_hd_web_thirdparty PUBLIC AES128=1 CBC=1)
set_source_files_properties(${_tp}/o1heap/o1heap/o1heap.c PROPERTIES COMPILE_DEFINITIONS
    "O1HEAP_CONFIG_HEADER=\"${_sdk}/include/rex/o1heap_config.h\"")
target_compile_options(trials_hd_web_thirdparty PRIVATE -w)

# ImGui keeps one global context: object files, linked once.
add_library(trials_hd_web_imgui OBJECT
    ${_tp}/imgui/imgui.cpp
    ${_tp}/imgui/imgui_demo.cpp
    ${_tp}/imgui/imgui_draw.cpp
    ${_tp}/imgui/imgui_tables.cpp
    ${_tp}/imgui/imgui_widgets.cpp
)
target_compile_options(trials_hd_web_imgui PRIVATE -w)

include(${CMAKE_CURRENT_LIST_DIR}/ffmpeg.cmake)

#=============================================================================
# ReXGlue SDK
#=============================================================================
set(TRIALSHD_WEB_SDK_CORE
    core/arena.cpp
    core/bit_map.cpp
    core/bit_stream.cpp
    core/byte_stream.cpp
    core/clock.cpp
    core/cvar.cpp
    core/exception_handler.cpp
    core/filesystem.cpp
    core/filesystem_wildcard.cpp
    core/frame_timing.cpp
    core/hash.cpp
    core/logging.cpp
    core/memory.cpp
    core/mutex.cpp
    core/ring_buffer.cpp
    core/string_buffer.cpp
    core/threading.cpp
    core/timer_queue.cpp
    core/utf8.cpp
    core/vec128.cpp
    # Platform layer (7.9.1): POSIX files that compile for the web, with web
    # branches where needed, and _emscripten replacements for the rest.
    core/atomic_posix.cpp
    core/clock_posix.cpp
    core/dbg_posix.cpp
    core/dynlib_posix.cpp
    core/exception_handler_emscripten.cpp
    core/fiber_emscripten.cpp
    core/fma_emscripten.cpp
    core/filesystem_posix.cpp
    core/mapped_memory_posix.cpp
    core/math_gcc.cpp
    core/memory_emscripten.cpp
    core/platform/console_posix.cpp
    core/platform/env_posix.cpp
    core/seh_posix.cpp
    core/socket_posix.cpp
    core/system_posix.cpp
    core/threading_posix.cpp
    # Xenos data-format layer (rexcore natively).
    graphics/xenos.cpp
    graphics/pipeline/texture/info.cpp
    graphics/pipeline/texture/info_formats.cpp
    graphics/pipeline/texture/extent.cpp
    graphics/pipeline/texture/util.cpp
    graphics/pipeline/texture/conversion.cpp
)
set(TRIALSHD_WEB_SDK_FILESYSTEM
    filesystem/device.cpp
    filesystem/entry.cpp
    filesystem/virtual_file_system.cpp
    filesystem/devices/disc_image_device.cpp
    filesystem/devices/disc_image_entry.cpp
    filesystem/devices/disc_image_file.cpp
    filesystem/devices/host_path_device.cpp
    filesystem/devices/host_path_entry.cpp
    filesystem/devices/host_path_file.cpp
    filesystem/devices/null_device.cpp
    filesystem/devices/null_entry.cpp
    filesystem/devices/null_file.cpp
    filesystem/devices/stfs_container_device.cpp
    filesystem/devices/stfs_container_entry.cpp
    filesystem/devices/stfs_container_file.cpp
)
set(TRIALSHD_WEB_SDK_SYSTEM
    system/runtime.cpp
    system/embedded_metadata.cpp
    system/achievement_manager.cpp
    system/achievements.cpp
    system/kernel_state.cpp
    system/xmemory.cpp
    system/kernel_module.cpp
    system/user_module.cpp
    system/xthread.cpp
    system/xevent.cpp
    system/xfile.cpp
    system/xmutant.cpp
    system/xsemaphore.cpp
    system/xtimer.cpp
    system/xiocompletion.cpp
    system/xnotifylistener.cpp
    system/xenumerator.cpp
    system/xsocket.cpp
    system/xsymboliclink.cpp
    system/xobject.cpp
    system/xmodule.cpp
    system/guest_path.cpp
    system/util/object_table.cpp
    system/util/native_list.cpp
    system/util/xdbf_utils.cpp
    system/util/gameinfo_utils.cpp
    system/xam/app_manager.cpp
    system/xam/content_manager.cpp
    system/xam/user_profile.cpp
    system/ppc_types.cpp
    system/elf_module.cpp
    system/entry_table.cpp
    system/export_resolver.cpp
    system/lzx.cpp
    system/mmio_handler.cpp
    system/map_parser.cpp
    system/module.cpp
    system/function_dispatcher.cpp
    system/gpu_plugin_loader.cpp
    system/thread.cpp
    system/thread_state.cpp
    system/xex_module.cpp
)
set(TRIALSHD_WEB_SDK_KERNEL
    kernel/kernel_init.cpp
    kernel/xam/xam_module.cpp
    kernel/xam/xam_avatar.cpp
    kernel/xam/xam_content.cpp
    kernel/xam/xam_content_aggregate.cpp
    kernel/xam/xam_content_device.cpp
    kernel/xam/xam_debug.cpp
    kernel/xam/xam_enum.cpp
    kernel/xam/xam_info.cpp
    kernel/xam/xam_input.cpp
    kernel/xam/xam_locale.cpp
    kernel/xam/xam_msg.cpp
    kernel/xam/xam_net.cpp
    kernel/xam/xam_notify.cpp
    kernel/xam/xam_nui.cpp
    kernel/xam/xam_party.cpp
    kernel/xam/xam_task.cpp
    kernel/xam/xam_ui.cpp
    kernel/xam/xam_user.cpp
    kernel/xam/xam_video.cpp
    kernel/xam/xam_voice.cpp
    kernel/xam/xam_misc.cpp
    kernel/xam/apps/xam_app.cpp
    kernel/xam/apps/xgi_app.cpp
    kernel/xam/apps/xlivebase_app.cpp
    kernel/xam/apps/xmp_app.cpp
    kernel/xbdm/xbdm_module.cpp
    kernel/xbdm/xbdm_misc.cpp
    kernel/xboxkrnl/xboxkrnl_crypt.cpp
    kernel/xboxkrnl/xboxkrnl_debug.cpp
    kernel/xboxkrnl/xboxkrnl_error.cpp
    kernel/xboxkrnl/xboxkrnl_hal.cpp
    kernel/xboxkrnl/xboxkrnl_hid.cpp
    kernel/xboxkrnl/xboxkrnl_io.cpp
    kernel/xboxkrnl/xboxkrnl_io_info.cpp
    kernel/xboxkrnl/xboxkrnl_memory.cpp
    kernel/xboxkrnl/xboxkrnl_misc.cpp
    kernel/xboxkrnl/xboxkrnl_module.cpp
    kernel/xboxkrnl/xboxkrnl_modules.cpp
    kernel/xboxkrnl/xboxkrnl_ob.cpp
    kernel/xboxkrnl/xboxkrnl_rtl.cpp
    kernel/xboxkrnl/xboxkrnl_strings.cpp
    kernel/xboxkrnl/xboxkrnl_threading.cpp
    kernel/xboxkrnl/xboxkrnl_video.cpp
    kernel/xboxkrnl/xboxkrnl_xconfig.cpp
    kernel/xboxkrnl/xboxkrnl_audio.cpp
    kernel/xboxkrnl/xboxkrnl_audio_xma.cpp
    kernel/crt/heap.cpp
    kernel/crt/file.cpp
    kernel/crt/memory.cpp
    kernel/crt/string.cpp
    kernel/crt/threading.cpp
)
set(TRIALSHD_WEB_SDK_AUDIO
    audio/audio_driver.cpp
    audio/audio_system.cpp
    audio/downmix.cpp
    audio/xma_context.cpp
    audio/xma_decoder.cpp
    audio/xma_register_file.cpp
    audio/nop/nop_audio_system.cpp
    audio/web/web_audio_driver.cpp
)
set(TRIALSHD_WEB_SDK_INPUT
    input/input_system.cpp
    input/device_assignment.cpp
    input/state_merge.cpp
    input/mnk/mnk_input_driver.cpp
    input/nop/nop_input_driver.cpp
    input/web/web_input_driver.cpp
)
set(TRIALSHD_WEB_SDK_UI
    ui/graphics_upload_buffer_pool.cpp
    ui/graphics_util.cpp
    ui/image_decode.cpp
    ui/imgui_dialog.cpp
    ui/imgui_drawer.cpp
    ui/immediate_drawer.cpp
    ui/keybinds.cpp
    ui/menu_item.cpp
    ui/presenter.cpp
    ui/renderdoc_api.cpp
    ui/window.cpp
    ui/windowed_app.cpp
    ui/windowed_app_context.cpp
    ui/overlay/achievement_icon_cache.cpp
    ui/overlay/achievement_toast.cpp
    ui/overlay/achievements_overlay.cpp
    ui/overlay/debug_overlay.cpp
    ui/overlay/console_overlay.cpp
    ui/overlay/console_commands.cpp
    ui/overlay/settings_overlay.cpp
    ui/sdl_virtual_key.cpp
    ui/web/web_ui.cpp
    ui/web/window_web.cpp
    ui/web/windowed_app_context_web.cpp
    # The app shell (rexglue_configure_target compiles these into the game).
    ui/rex_app.cpp
    ui/web/windowed_app_main_web.cpp
)
# Backend-agnostic GPU emulation: the list in src/gpu_webgpu/CMakeLists.txt.
set(TRIALSHD_WEB_SDK_GRAPHICS
    graphics/flags.cpp
    graphics/register_file.cpp
    graphics/registers.cpp
    graphics/pipeline/shader/shader.cpp
    graphics/format/ucode.cpp
    graphics/sampler_info.cpp
    graphics/graphics_system.cpp
    graphics/command_processor.cpp
    graphics/util/draw_extent_estimator.cpp
    graphics/util/draw.cpp
    graphics/packet_disassembler.cpp
    graphics/primitive_processor.cpp
    graphics/pipeline/render_target/cache.cpp
    graphics/shared_memory.cpp
    graphics/pipeline/texture/cache.cpp
    graphics/pipeline/shader/interpreter.cpp
    graphics/pipeline/shader/translator.cpp
    graphics/pipeline/shader/translator_disasm.cpp
)
set(_sdk_sources
    ${TRIALSHD_WEB_SDK_CORE} ${TRIALSHD_WEB_SDK_FILESYSTEM} ${TRIALSHD_WEB_SDK_SYSTEM}
    ${TRIALSHD_WEB_SDK_KERNEL} ${TRIALSHD_WEB_SDK_AUDIO} ${TRIALSHD_WEB_SDK_INPUT}
    ${TRIALSHD_WEB_SDK_UI} ${TRIALSHD_WEB_SDK_GRAPHICS})
list(TRANSFORM _sdk_sources PREPEND "${_sdk}/src/")

# OBJECT, not STATIC: natively rexruntime is a shared library, so every object
# is linked, including ones that only self-register (cvars, kernel exports).
add_library(trials_hd_web_sdk OBJECT ${_sdk_sources})
# fma() through the CPU's FMA instruction when the engine fuses relaxed_madd
# (the file checks at startup); the link wraps fma (-Wl,--wrap=fma below).
set_source_files_properties(${_sdk}/src/core/fma_emscripten.cpp PROPERTIES
    COMPILE_OPTIONS -mrelaxed-simd)
target_include_directories(trials_hd_web_sdk PRIVATE ${_sdk} ${_sdk}/src)

#=============================================================================
# Recompiled game code (generated/default, from the native build)
#=============================================================================
include(${_gen}/sources.cmake)
add_library(trials_hd_web_recomp OBJECT ${GENERATED_SOURCES})
# Optimization level of the recompiled game code (experiments: -O3).
set(TRIALSHD_WEB_RECOMP_OPT "-O2" CACHE STRING "Optimization flag for the generated game code")
target_compile_options(trials_hd_web_recomp PRIVATE ${TRIALSHD_WEB_RECOMP_OPT})
target_include_directories(trials_hd_web_recomp PRIVATE ${_root} ${_root}/src ${_gen})
target_precompile_headers(trials_hd_web_recomp PRIVATE ${_gen}/trials_hd_pch.h)

#=============================================================================
# WebGPU renderer (src/gpu_webgpu), linked in (7.9.10), on the browser's
# WebGPU through emdawnwebgpu (W-L07). The browser variant only: it needs a
# canvas and JSPI (the GPU worker's waits suspend to the event loop, 7.4).
#=============================================================================
set(TRIALSHD_WEB_WEBGPU OFF)
if(NOT TRIALSHD_WEB_NODE)
    set(TRIALSHD_WEB_WEBGPU ON)
    set(_gpu ${_root}/src/gpu_webgpu)
    # The same files as src/gpu_webgpu/CMakeLists.txt, minus the X11 helper.
    file(GLOB _gpu_sources CONFIGURE_DEPENDS ${_gpu}/*.cpp ${_gpu}/wgsl/*.cpp)
    list(FILTER _gpu_sources EXCLUDE REGEX "/x11_display\\.cpp$")
    # Embedded WGSL (wgsl/<name>.wgsl.h), with the renderer's own script.
    file(GLOB _gpu_wgsl CONFIGURE_DEPENDS ${_gpu}/shaders/*.wgsl ${_gpu}/wgsl/*.wgsl)
    set(_gpu_wgsl_headers)
    foreach(_wgsl IN LISTS _gpu_wgsl)
        get_filename_component(_name ${_wgsl} NAME_WE)
        set(_header ${CMAKE_BINARY_DIR}/gpu_webgpu/wgsl/${_name}.wgsl.h)
        add_custom_command(
            OUTPUT ${_header}
            COMMAND ${CMAKE_COMMAND} -DIN=${_wgsl} -DOUT=${_header} -DNAME=${_name}
                    -P ${_gpu}/embed_wgsl.cmake
            DEPENDS ${_wgsl} ${_gpu}/embed_wgsl.cmake
            VERBATIM)
        list(APPEND _gpu_wgsl_headers ${_header})
    endforeach()
    add_library(trials_hd_web_webgpu OBJECT ${_gpu_sources} ${_gpu_wgsl_headers})
    target_include_directories(trials_hd_web_webgpu PRIVATE
        ${_gpu} ${CMAKE_BINARY_DIR}/gpu_webgpu ${_sdk})
    target_compile_options(trials_hd_web_webgpu PRIVATE --use-port=emdawnwebgpu)
endif()

#=============================================================================
# The game
#=============================================================================
add_executable(trials_hd
    src/main.cpp
    src/fallback_audio.cpp
    src/graphics_settings.cpp
    src/performance_overlay.cpp
    src/keyboard_controls.cpp
    src/fps_unlock.cpp
    src/gpu_wait.cpp
    src/achievements_hook.cpp
    src/crt_hooks.cpp
    src/offline_warning.cpp
    src/package_license.cpp
    src/exit_button.cpp
    src/web/null_graphics_system.cpp
    src/web/web_files.cpp
    src/web/web_input.cpp
    tools/stfs_extract/stfs_extract.cpp
    $<TARGET_OBJECTS:trials_hd_web_sdk>
    $<TARGET_OBJECTS:trials_hd_web_imgui>
    $<TARGET_OBJECTS:trials_hd_web_recomp>
)
target_include_directories(trials_hd PRIVATE ${_root} ${_root}/src ${_gen})
target_link_libraries(trials_hd PRIVATE trials_hd_web_thirdparty trials_hd_web_ffmpeg)
if(TRIALSHD_WEB_WEBGPU)
    target_sources(trials_hd PRIVATE $<TARGET_OBJECTS:trials_hd_web_webgpu>)
    target_compile_definitions(trials_hd PRIVATE TRIALSHD_WEB_WEBGPU=1)
    target_compile_options(trials_hd PRIVATE --use-port=emdawnwebgpu)
    # JSPI: every pthread entry can suspend, so the GPU worker's WebGPU waits
    # (WaitAny) and its per-frame yield return to the browser's event loop.
    # The command processor's thread receives the page's canvas (7.9.5).
    # web/webgpu_compat.js shims WebGPU calls some browsers reject (Firefox).
    target_link_options(trials_hd PRIVATE
        --use-port=emdawnwebgpu -sJSPI -sOFFSCREENCANVAS_SUPPORT
        --pre-js=${_root}/web/webgpu_compat.js)
    set_property(TARGET trials_hd APPEND PROPERTY LINK_DEPENDS ${_root}/web/webgpu_compat.js)
endif()

# Linear memory: the 2.75 GiB guest window (7.9.2) plus the host heap. A pool
# of workers big enough that guest thread creation never waits for one (8.4).
# Stacks are only committed when touched.
set(_web_link_flags
    -sPROXY_TO_PTHREAD
    -sPTHREAD_POOL_SIZE=48
    -sINITIAL_MEMORY=3328mb
    -sMAXIMUM_MEMORY=8gb
    -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=8mb
    -sDEFAULT_PTHREAD_STACK_SIZE=4mb
    -sEXIT_RUNTIME=1
    -Wl,--wrap=fma
    -sEXPORTED_RUNTIME_METHODS=callMain,wasmMemory
    -Wno-pthreads-mem-growth
    # Function names in the module (profiles and stack traces); no code cost.
    --profiling-funcs
)
if(TRIALSHD_WEB_NODE)
    # Development: Node with the host file system (--game_data_root=<assets>).
    # Emscripten's legacy file system: WasmFS's NODERAWFS does not see host paths.
    list(APPEND _web_link_flags -sENVIRONMENT=node -sNODERAWFS=1)
else()
    # WasmFS: the file system runs in wasm, so pthreads use it without proxying
    # to the browser's main thread, and it has the OPFS and fetch backends (8.6).
    target_compile_definitions(trials_hd PRIVATE TRIALSHD_WEB_WASMFS=1)
    # The page: out/build/<preset>/web/trials_hd.html, served by tools/web/serve.py.
    list(APPEND _web_link_flags -sWASMFS -sENVIRONMENT=web,worker
         --shell-file ${_root}/web/shell.html)
    configure_file(${_root}/web/input.js ${CMAKE_BINARY_DIR}/web/input.js COPYONLY)
    configure_file(${_root}/web/import.js ${CMAKE_BINARY_DIR}/web/import.js COPYONLY)
    configure_file(${_root}/web/audio.js ${CMAKE_BINARY_DIR}/web/audio.js COPYONLY)
    configure_file(${_root}/web/audio_worklet.js ${CMAKE_BINARY_DIR}/web/audio_worklet.js COPYONLY)
    set_target_properties(trials_hd PROPERTIES
        SUFFIX ".html"
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/web
        LINK_DEPENDS ${_root}/web/shell.html)
endif()
target_link_options(trials_hd PRIVATE ${_web_link_flags})
