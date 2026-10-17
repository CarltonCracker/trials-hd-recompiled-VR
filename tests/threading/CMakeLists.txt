# Isolated host-thread tests against the edited POSIX source; no SDK rebuild/staging.
cmake_minimum_required(VERSION 3.25)
project(alertable_wait_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(_repo ../.. ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(REXSDK_DIR "${_repo}/thirdparty/rexglue-sdk" CACHE PATH "SDK source")
set(REXSDK_BUILD_DIR "${_repo}/out/build/linux-amd64-relwithdebinfo/rexglue-sdk" CACHE PATH "SDK headers")
set(REXSDK_LIBRARY_DIR "${REXSDK_DIR}/out/linux-amd64" CACHE PATH "SDK libraries")
find_package(Threads REQUIRED)
find_library(_runtime NAMES rexruntimerd rexruntime PATHS ${REXSDK_LIBRARY_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(_fmt NAMES fmtrd fmt PATHS ${REXSDK_LIBRARY_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(_spdlog NAMES spdlogrd spdlog PATHS ${REXSDK_LIBRARY_DIR} NO_DEFAULT_PATH REQUIRED)
add_executable(alertable_wait_test alertable_wait_test.cpp ${REXSDK_DIR}/src/core/threading_posix.cpp)
target_include_directories(alertable_wait_test PRIVATE
    ${REXSDK_DIR}/include ${REXSDK_BUILD_DIR}/include
    ${REXSDK_DIR}/thirdparty/simde ${REXSDK_DIR}/thirdparty/fmt/include
    ${REXSDK_DIR}/thirdparty/spdlog/include)
target_compile_definitions(alertable_wait_test PRIVATE SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL)
target_compile_options(alertable_wait_test PRIVATE -fno-strict-aliasing -fno-char8_t)
target_link_libraries(alertable_wait_test PRIVATE ${_runtime} ${_spdlog} ${_fmt} Threads::Threads)
enable_testing()
add_test(NAME alertable_wait COMMAND alertable_wait_test)
set_tests_properties(alertable_wait PROPERTIES TIMEOUT 20)

add_executable(timer_queue_test timer_queue_test.cpp ${REXSDK_DIR}/src/core/timer_queue.cpp)
target_include_directories(timer_queue_test PRIVATE
    ${REXSDK_DIR}/include ${REXSDK_BUILD_DIR}/include
    ${REXSDK_DIR}/thirdparty/disruptorplus/include
    ${REXSDK_DIR}/thirdparty/fmt/include ${REXSDK_DIR}/thirdparty/simde)
target_compile_options(timer_queue_test PRIVATE -UNDEBUG)
target_link_libraries(timer_queue_test PRIVATE Threads::Threads)
add_test(NAME timer_queue COMMAND timer_queue_test)
set_tests_properties(timer_queue PROPERTIES TIMEOUT 20)

add_executable(trials_fast_path_test trials_fast_path_test.cpp ${REXSDK_DIR}/src/graphics/flags.cpp)
target_include_directories(trials_fast_path_test PRIVATE
    ${REXSDK_DIR}/include ${REXSDK_BUILD_DIR}/include
    ${REXSDK_DIR}/thirdparty/fmt/include ${REXSDK_DIR}/thirdparty/simde
    ${REXSDK_DIR}/thirdparty/spdlog/include ${REXSDK_DIR}/thirdparty/renderdoc)
target_compile_definitions(trials_fast_path_test PRIVATE SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL)
target_compile_options(trials_fast_path_test PRIVATE -UNDEBUG)
target_link_libraries(trials_fast_path_test PRIVATE ${_runtime} ${_spdlog} ${_fmt} Threads::Threads)
add_test(NAME trials_fast_path COMMAND trials_fast_path_test)
set_tests_properties(trials_fast_path PROPERTIES TIMEOUT 20)
