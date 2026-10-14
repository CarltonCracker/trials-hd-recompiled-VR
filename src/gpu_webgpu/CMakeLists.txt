# rexgpu-webgpu - the native WebGPU renderer plugin (Dawn).
#
# Design: docs/webgpu-renderer/README.md, sections 7.6 and 8. Loaded at runtime
# with --gpu_plugin=webgpu, like the SDK's rexgpu-xenos; nothing links it.
# Dawn comes from tools/build_dawn.sh:
#   -DTRIALSHD_WEBGPU=ON -DCMAKE_PREFIX_PATH=$PWD/out/dawn-install

find_package(Threads REQUIRED)  # Dawn's exported targets reference Threads::Threads
find_package(Dawn REQUIRED)

set(_sdk_graphics ${REXGLUE_ROOT}/src/graphics)

# The SDK's backend-agnostic GPU sources: REXGPU_XENOS_SOURCES in
# ${REXGLUE_ROOT}/src/graphics/CMakeLists.txt minus plugin_main, the SPIR-V
# translator and the Vulkan backend. Checked with
# docs/webgpu-renderer/probe/link_stub.sh; keep the two lists in sync. The
# stateless Xenos format layer is already in rexruntime (rexcore).
set(REXGPU_WEBGPU_SDK_SOURCES
    ${_sdk_graphics}/flags.cpp
    ${_sdk_graphics}/register_file.cpp
    ${_sdk_graphics}/registers.cpp
    ${_sdk_graphics}/pipeline/shader/shader.cpp
    ${_sdk_graphics}/format/ucode.cpp
    ${_sdk_graphics}/sampler_info.cpp
    ${_sdk_graphics}/graphics_system.cpp
    ${_sdk_graphics}/command_processor.cpp
    ${_sdk_graphics}/util/draw_extent_estimator.cpp
    ${_sdk_graphics}/util/draw.cpp
    ${_sdk_graphics}/packet_disassembler.cpp
    ${_sdk_graphics}/primitive_processor.cpp
    ${_sdk_graphics}/pipeline/render_target/cache.cpp
    ${_sdk_graphics}/shared_memory.cpp
    ${_sdk_graphics}/pipeline/texture/cache.cpp
    ${_sdk_graphics}/pipeline/shader/interpreter.cpp
    ${_sdk_graphics}/pipeline/shader/translator.cpp
    ${_sdk_graphics}/pipeline/shader/translator_disasm.cpp
)

# Our own WGSL, embedded as C++ strings (wgsl/<name>.wgsl.h: kWgsl_<name>).
set(REXGPU_WEBGPU_WGSL
    shaders/guest_output.wgsl
    shaders/immediate.wgsl
)
set(_wgsl_headers)
foreach(_wgsl IN LISTS REXGPU_WEBGPU_WGSL)
    get_filename_component(_name ${_wgsl} NAME_WE)
    set(_header ${CMAKE_CURRENT_BINARY_DIR}/wgsl/${_name}.wgsl.h)
    add_custom_command(
        OUTPUT ${_header}
        COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/${_wgsl} -DOUT=${_header}
                -DNAME=${_name} -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_wgsl.cmake
        DEPENDS ${_wgsl} embed_wgsl.cmake
        COMMENT "Embedding ${_wgsl}"
        VERBATIM)
    list(APPEND _wgsl_headers ${_header})
endforeach()

add_library(rexgpu-webgpu SHARED
    ${REXGPU_WEBGPU_SDK_SOURCES}
    ${_wgsl_headers}
    plugin_main.cpp
    webgpu_backend.cpp
    webgpu_command_processor.cpp
    webgpu_graphics_system.cpp
    webgpu_immediate_drawer.cpp
    webgpu_presenter.cpp
    webgpu_provider.cpp
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(X11 REQUIRED)
    target_sources(rexgpu-webgpu PRIVATE x11_display.cpp)
    target_link_libraries(rexgpu-webgpu PRIVATE X11::X11)
endif()

# Hidden visibility keeps this plugin's copies of the SDK classes private, as
# in rexgpu-xenos. The postfixes follow the plugin loader's file names
# (librexgpu-webgpu{,d,rd}.so); the SDK only sets them in its own scope.
set_target_properties(rexgpu-webgpu PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    DEBUG_POSTFIX d
    RELWITHDEBINFO_POSTFIX rd
)

target_include_directories(rexgpu-webgpu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${REXGLUE_ROOT}/thirdparty/renderdoc
    ${REXGLUE_ROOT}
)

# The SDK's directory-wide options, which do not reach this directory. They
# have to match: -fno-char8_t changes the types (and mangled names) of u8
# literals, and the GPU code relies on the other two.
target_compile_options(rexgpu-webgpu PRIVATE
    -fno-strict-aliasing
    -ffp-model=strict
    -fno-char8_t
    -Wno-unused-variable
)

if(COMMAND rex_target_pch)
    rex_target_pch(rexgpu-webgpu
        <mutex>
        <unordered_map>
        <rex/math.h>
        <rex/memory.h>
        <rex/thread/mutex.h>
        <rex/system/xmemory.h>
        <rex/graphics/xenos.h>
        <rex/graphics/registers.h>
        <rex/graphics/format/ucode.h>
        <rex/graphics/pipeline/shader/shader.h>
    )
endif()

# Same rule as the SDK: link rexruntime, never the rexcore/rexui OBJECT
# libraries (they are inside rexruntime already). Dawn links statically.
target_link_libraries(rexgpu-webgpu PRIVATE
    rexruntime
    xxHash::xxhash
    dawn::webgpu_dawn
)

# Dawn's static libraries (Dawn, Tint, Abseil, ...) have default visibility;
# keep their symbols private so only rex_gpu_* is exported.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(rexgpu-webgpu PRIVATE -Wl,--exclude-libs,ALL)
endif()
