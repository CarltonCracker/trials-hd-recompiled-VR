# Standalone synthetic GPU tests: no game build, plugin staging or shared Ninja directory.
# cmake -S tests/gpu_webgpu -B out/webgpu-memory-test -G Ninja \
#   -DCMAKE_PREFIX_PATH=$PWD/out/dawn-install -DCMAKE_CXX_COMPILER=clang++
# cmake --build out/webgpu-memory-test && out/webgpu-memory-test/shared_memory_test [adapter]
cmake_minimum_required(VERSION 3.25)
project(webgpu_synthetic_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(_repo ../.. ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(REXSDK_DIR "${_repo}/thirdparty/rexglue-sdk" CACHE PATH "SDK source tree")
set(REXSDK_LIBRARY_DIR "${REXSDK_DIR}/out/linux-amd64" CACHE PATH "Existing SDK libraries")
set(REXSDK_BUILD_DIR "${_repo}/out/build/linux-amd64-relwithdebinfo/rexglue-sdk" CACHE PATH
    "Existing SDK generated headers")
find_package(Threads REQUIRED)
find_package(Dawn REQUIRED)
find_library(_runtime NAMES rexruntimerd rexruntime PATHS ${REXSDK_LIBRARY_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(_fmt NAMES fmtrd fmt PATHS ${REXSDK_LIBRARY_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(_spdlog NAMES spdlogrd spdlog PATHS ${REXSDK_LIBRARY_DIR} NO_DEFAULT_PATH REQUIRED)

add_executable(shared_memory_test
    shared_memory_test.cpp
    ${_repo}/src/gpu_webgpu/guest_memory_mirror.cpp
    ${REXSDK_DIR}/src/graphics/shared_memory.cpp)
target_include_directories(shared_memory_test PRIVATE
    ${_repo}/src/gpu_webgpu ${REXSDK_DIR}/include ${REXSDK_BUILD_DIR}/include
    ${REXSDK_DIR}/thirdparty/simde ${REXSDK_DIR}/thirdparty/fmt/include
    ${REXSDK_DIR}/thirdparty/spdlog/include)
target_compile_definitions(shared_memory_test PRIVATE SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL)
target_compile_options(shared_memory_test PRIVATE -fno-strict-aliasing -ffp-model=strict -fno-char8_t)
target_link_libraries(shared_memory_test PRIVATE
    ${_runtime} ${_spdlog} ${_fmt} dawn::webgpu_dawn Threads::Threads)
# Translate authored ALU microcode and execute it as compute vs the SDK interpreter.
set(_prelude ${CMAKE_CURRENT_BINARY_DIR}/wgsl/wgsl_prelude.wgsl.h)
add_custom_command(OUTPUT ${_prelude}
    COMMAND ${CMAKE_COMMAND}
        -DIN=${_repo}/src/gpu_webgpu/wgsl/wgsl_prelude.wgsl -DOUT=${_prelude}
        -DNAME=wgsl_prelude -P ${_repo}/src/gpu_webgpu/embed_wgsl.cmake
    DEPENDS ${_repo}/src/gpu_webgpu/wgsl/wgsl_prelude.wgsl
            ${_repo}/src/gpu_webgpu/embed_wgsl.cmake VERBATIM)
add_executable(alu_test alu_test.cpp ${_prelude}
    ${_repo}/src/gpu_webgpu/wgsl/wgsl_shader_translator.cpp
    ${_repo}/src/gpu_webgpu/wgsl/wgsl_alu.cpp
    ${_repo}/src/gpu_webgpu/wgsl/wgsl_fetch.cpp
    ${REXSDK_DIR}/src/graphics/pipeline/shader/shader.cpp
    ${REXSDK_DIR}/src/graphics/pipeline/shader/interpreter.cpp
    ${REXSDK_DIR}/src/graphics/pipeline/shader/translator.cpp
    ${REXSDK_DIR}/src/graphics/pipeline/shader/translator_disasm.cpp
    ${REXSDK_DIR}/src/graphics/format/ucode.cpp
    ${REXSDK_DIR}/src/graphics/register_file.cpp
    ${REXSDK_DIR}/src/graphics/registers.cpp
    ${REXSDK_DIR}/src/graphics/flags.cpp)
target_include_directories(alu_test PRIVATE
    ${_repo}/src/gpu_webgpu ${CMAKE_CURRENT_BINARY_DIR}
    ${REXSDK_DIR}/include ${REXSDK_BUILD_DIR}/include ${REXSDK_DIR}/thirdparty/renderdoc
    ${REXSDK_DIR}/thirdparty/simde ${REXSDK_DIR}/thirdparty/fmt/include
    ${REXSDK_DIR}/thirdparty/spdlog/include)
target_compile_definitions(alu_test PRIVATE SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL)
target_compile_options(alu_test PRIVATE -fno-strict-aliasing -ffp-model=strict -fno-char8_t)
target_link_libraries(alu_test PRIVATE
    ${_runtime} ${_spdlog} ${_fmt} dawn::webgpu_dawn Threads::Threads)
add_executable(format_tables_test
    format_tables_test.cpp
    ${_repo}/src/gpu_webgpu/format_tables.cpp)
target_include_directories(format_tables_test PRIVATE
    ${_repo}/src/gpu_webgpu ${REXSDK_DIR}/include ${REXSDK_BUILD_DIR}/include
    ${REXSDK_DIR}/thirdparty/simde ${REXSDK_DIR}/thirdparty/fmt/include
    ${REXSDK_DIR}/thirdparty/spdlog/include)
target_compile_definitions(format_tables_test PRIVATE SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL)
target_compile_options(format_tables_test PRIVATE -fno-strict-aliasing -ffp-model=strict -fno-char8_t)
target_link_libraries(format_tables_test PRIVATE
    ${_runtime} ${_spdlog} ${_fmt} dawn::webgpu_dawn Threads::Threads)

add_executable(sampler_cache_test
    sampler_cache_test.cpp
    ${_repo}/src/gpu_webgpu/sampler_cache.cpp)
target_include_directories(sampler_cache_test PRIVATE
    ${_repo}/src/gpu_webgpu ${REXSDK_DIR}/include ${REXSDK_BUILD_DIR}/include
    ${REXSDK_DIR}/thirdparty/renderdoc ${REXSDK_DIR}/thirdparty/simde
    ${REXSDK_DIR}/thirdparty/fmt/include ${REXSDK_DIR}/thirdparty/spdlog/include)
target_compile_definitions(sampler_cache_test PRIVATE SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL)
target_compile_options(sampler_cache_test PRIVATE -fno-strict-aliasing -ffp-model=strict -fno-char8_t)
target_link_libraries(sampler_cache_test PRIVATE
    ${_runtime} ${_spdlog} ${_fmt} dawn::webgpu_dawn Threads::Threads)

add_executable(pipeline_state_test
    pipeline_state_test.cpp
    ${_repo}/src/gpu_webgpu/pipeline_state.cpp
    ${REXSDK_DIR}/src/graphics/util/draw.cpp
    ${REXSDK_DIR}/src/graphics/register_file.cpp
    ${REXSDK_DIR}/src/graphics/registers.cpp
    ${REXSDK_DIR}/src/graphics/flags.cpp)
target_include_directories(pipeline_state_test PRIVATE
    ${_repo}/src/gpu_webgpu ${REXSDK_DIR}/include ${REXSDK_BUILD_DIR}/include
    ${REXSDK_DIR}/thirdparty/xxHash
    ${REXSDK_DIR}/thirdparty/renderdoc ${REXSDK_DIR}/thirdparty/simde
    ${REXSDK_DIR}/thirdparty/fmt/include ${REXSDK_DIR}/thirdparty/spdlog/include)
target_compile_definitions(pipeline_state_test PRIVATE SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL)
target_compile_options(pipeline_state_test PRIVATE -fno-strict-aliasing -ffp-model=strict -fno-char8_t)
target_link_libraries(pipeline_state_test PRIVATE
    ${_runtime} ${_spdlog} ${_fmt} dawn::webgpu_dawn Threads::Threads)

add_executable(wgsl_prelude_test
    wgsl_prelude_test.cpp
    ${_prelude}
    ${REXSDK_DIR}/src/graphics/xenos.cpp
    ${REXSDK_DIR}/src/graphics/registers.cpp
    ${REXSDK_DIR}/src/graphics/flags.cpp)
target_include_directories(wgsl_prelude_test PRIVATE
    ${_repo}/src/gpu_webgpu ${CMAKE_CURRENT_BINARY_DIR}
    ${REXSDK_DIR}/include ${REXSDK_BUILD_DIR}/include
    ${REXSDK_DIR}/thirdparty/renderdoc
    ${REXSDK_DIR}/thirdparty/simde
    ${REXSDK_DIR}/thirdparty/fmt/include ${REXSDK_DIR}/thirdparty/spdlog/include)
target_compile_definitions(wgsl_prelude_test PRIVATE SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL)
target_compile_options(wgsl_prelude_test PRIVATE -fno-strict-aliasing -ffp-model=strict -fno-char8_t)
target_link_libraries(wgsl_prelude_test PRIVATE
    ${_runtime} ${_spdlog} ${_fmt} dawn::webgpu_dawn Threads::Threads)

add_executable(gamma_test
    gamma_test.cpp
    ${REXSDK_DIR}/src/graphics/xenos.cpp
    ${REXSDK_DIR}/src/graphics/registers.cpp
    ${REXSDK_DIR}/src/graphics/flags.cpp)
target_include_directories(gamma_test PRIVATE
    ${_repo}/src/gpu_webgpu
    ${REXSDK_DIR}/include ${REXSDK_BUILD_DIR}/include
    ${REXSDK_DIR}/thirdparty/renderdoc
    ${REXSDK_DIR}/thirdparty/simde
    ${REXSDK_DIR}/thirdparty/fmt/include ${REXSDK_DIR}/thirdparty/spdlog/include)
target_compile_definitions(gamma_test PRIVATE SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL)
target_compile_options(gamma_test PRIVATE -fno-strict-aliasing -ffp-model=strict -fno-char8_t)
target_link_libraries(gamma_test PRIVATE
    ${_runtime} ${_spdlog} ${_fmt} dawn::webgpu_dawn Threads::Threads)

add_executable(index_swap_test
    index_swap_test.cpp)
target_include_directories(index_swap_test PRIVATE
    ${_repo}/src/gpu_webgpu
    ${REXSDK_DIR}/include ${REXSDK_BUILD_DIR}/include
    ${REXSDK_DIR}/thirdparty/renderdoc
    ${REXSDK_DIR}/thirdparty/simde
    ${REXSDK_DIR}/thirdparty/fmt/include ${REXSDK_DIR}/thirdparty/spdlog/include)
target_compile_definitions(index_swap_test PRIVATE
    SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL
    INDEX_SWAP_WGSL_PATH="${_repo}/src/gpu_webgpu/shaders/index_swap.wgsl")
target_compile_options(index_swap_test PRIVATE -fno-strict-aliasing -ffp-model=strict -fno-char8_t)
target_link_libraries(index_swap_test PRIVATE
    ${_runtime} ${_spdlog} ${_fmt} dawn::webgpu_dawn Threads::Threads)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vertex_fetch_test.cpp")
  # Reuse the interpreter/translator compilation and flags, without the ALU test main.
  get_target_property(_fetch_sources alu_test SOURCES)
  list(REMOVE_ITEM _fetch_sources alu_test.cpp)
  add_executable(vertex_fetch_test vertex_fetch_test.cpp ${_fetch_sources})
  foreach(_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES)
    get_target_property(_value alu_test ${_property})
    set_property(TARGET vertex_fetch_test PROPERTY ${_property} ${_value})
  endforeach()
endif()

enable_testing()
if (TARGET vertex_fetch_test)
  add_test(NAME vertex_fetch COMMAND vertex_fetch_test)
endif()
add_test(NAME shared_memory COMMAND shared_memory_test)
add_test(NAME alu COMMAND alu_test)
add_test(NAME format_tables COMMAND format_tables_test)
add_test(NAME sampler_cache COMMAND sampler_cache_test)
add_test(NAME diff_draws COMMAND python3 ${_repo}/tools/diff_draws.py --self-test)
add_test(NAME pipeline_state COMMAND pipeline_state_test)
add_test(NAME wgsl_prelude COMMAND wgsl_prelude_test)
add_test(NAME gamma COMMAND gamma_test)
add_test(NAME index_swap COMMAND index_swap_test)


