cmake_minimum_required(VERSION 3.20)
project(quiver VERSION 2.0.0 LANGUAGES CXX)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Compiler warnings & optimization
add_compile_options(-Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -march=native)
else()
    add_compile_options(-O0 -g3)
endif()

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Dependencies
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

# nlohmann_json: try system first, fall back to FetchContent
find_package(nlohmann_json 3.2.0 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# libcpr: try system first, fall back to FetchContent
find_library(CPR_LIB NAMES cpr)
if(NOT CPR_LIB)
    include(FetchContent)
    set(CPR_USE_SYSTEM_CURL ON CACHE BOOL "" FORCE)
    set(CPR_ENABLE_SSL ON CACHE BOOL "" FORCE)
    set(CPR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        cpr
        URL https://github.com/libcpr/cpr/archive/refs/tags/1.10.5.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(cpr)
    set(CPR_LIB cpr::cpr)
endif()

# Source files (database_manager, package_manager, container_management removed)
set(SOURCES
    src/main.cpp
    src/state_store.cpp
    src/container.cpp
    src/mount.cpp
    src/device_manager.cpp
    src/image_manager.cpp
    src/network.cpp
    src/terminal.cpp
    src/volumes.cpp
    src/monitor.cpp
    src/utils.cpp
    src/command_line_handler.cpp
    src/cgroup.cpp
    src/gpu_manager.cpp
)

add_executable(quiver ${SOURCES})

target_include_directories(quiver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(quiver PRIVATE
    ${CPR_LIB}
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    Threads::Threads
    util
    dl
)

enable_testing()

add_executable(quiver_cgroup_tests
    tests/test_cgroup.cpp
    src/cgroup.cpp
    src/state_store.cpp
    src/utils.cpp
    src/monitor.cpp
)
target_include_directories(quiver_cgroup_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(quiver_cgroup_tests PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    Threads::Threads
)
add_test(NAME CgroupTests COMMAND quiver_cgroup_tests)

add_executable(quiver_gpu_tests
    tests/test_gpu.cpp
    src/gpu_manager.cpp
    src/cgroup.cpp
    src/state_store.cpp
    src/utils.cpp
    src/monitor.cpp
)
target_include_directories(quiver_gpu_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(quiver_gpu_tests PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    Threads::Threads
    dl
)
add_test(NAME GpuTests COMMAND quiver_gpu_tests)

# Phase 5.1: StateStore Lock-Free vs Legacy Monolithic Contention Benchmark
add_executable(quiver_state_benchmark
    tests/test_state_benchmark.cpp
    src/state_store.cpp
    src/utils.cpp
    src/monitor.cpp
)
target_include_directories(quiver_state_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(quiver_state_benchmark PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    Threads::Threads
    dl
)
add_test(NAME StateBenchmarkTests COMMAND quiver_state_benchmark)

# Phase 5.2: 50-100 Container Concurrency & Cleanup Stress Test Suite
add_executable(quiver_concurrency_tests
    tests/test_concurrency_stress.cpp
    src/state_store.cpp
    src/cgroup.cpp
    src/gpu_manager.cpp
    src/mount.cpp
    src/device_manager.cpp
    src/utils.cpp
    src/monitor.cpp
)
target_include_directories(quiver_concurrency_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(quiver_concurrency_tests PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    Threads::Threads
    dl
)
add_test(NAME ConcurrencyStressTests COMMAND quiver_concurrency_tests)



