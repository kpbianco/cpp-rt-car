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

if (ENABLE_LOG)
    target_compile_definitions(simcore INTERFACE -DLOG_ENABLED -DLOG_DEFAULT_LEVEL=2)
endif()

if (ENABLE_PROF)
    target_compile_definitions(simcore INTERFACE -DPROF_ENABLED)
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
