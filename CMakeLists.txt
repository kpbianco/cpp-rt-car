cmake_minimum_required(VERSION 3.20)

project(car_sim_core LANGUAGES CXX)

# Options (toggle logging / profiler / tests)
option(ENABLE_LOG "Enable logging" ON)
option(ENABLE_PROF "Enable profiler" ON)
option(ENABLE_TESTS "Build tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Recommended warnings (clang/gcc)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor)
endif()

# --- GoogleTest submodule ---
# Expect it at external/googletest (git submodule add ...)
if (ENABLE_TESTS)
    add_subdirectory(external/googletest)
endif()

# Header-only core library
add_library(simcore INTERFACE)
target_include_directories(simcore INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# In your top-level CMakeLists.txt

# Ensure asserts are ON for tests (avoid -DNDEBUG)
if (CMAKE_BUILD_TYPE MATCHES "Release")
  message(WARNING "Tests are more useful with assertions; consider Debug or RelWithDebInfo.")
endif()

add_library(rt_memory INTERFACE)
target_include_directories(rt_memory INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Optional toggles:
# target_compile_definitions(rt_memory INTERFACE RT_ARENA_PRETOUCH=1)
# target_compile_definitions(rt_memory INTERFACE RT_ARENA_MLOCK=1)
# target_compile_definitions(rt_memory INTERFACE RT_ARENA_MADVISE_HUGEPAGE=1)

# --- Tests ---



if (ENABLE_LOG)
    target_compile_definitions(simcore INTERFACE -DLOG_ENABLED -DLOG_DEFAULT_LEVEL=2)
endif()

if (ENABLE_PROF)
    target_compile_definitions(simcore INTERFACE -DPROF_ENABLED)
endif()

option(SIM_ENABLE_AVX2 "Enable AVX2/FMA for SIMD kernels" ON)

if (SIM_ENABLE_AVX2 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
  add_compile_options(-mavx2 -mfma)
endif()

target_compile_features(simcore INTERFACE cxx_std_20)

# Main executable
add_executable(simcore_app src/main.cpp)
target_link_libraries(simcore_app PRIVATE simcore)

# Tests
if (ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

add_custom_target(tests
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/build      # wipe  ▸ optional
    COMMAND ${CMAKE_COMMAND} -DENABLE_TESTS=ON -DCMAKE_BUILD_TYPE=Debug ${CMAKE_SOURCE_DIR}
    COMMAND ${CMAKE_COMMAND} --build . -- -j
    COMMAND ctest --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Configure, build and run all tests")