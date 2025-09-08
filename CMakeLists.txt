cmake_minimum_required(VERSION 3.20)

project(car_sim_core LANGUAGES CXX)

# Options (toggle logging / profiler / tests)
option(ENABLE_LOG "Enable logging" ON)
option(ENABLE_PROF "Enable profiler" ON)
option(ENABLE_TESTS "Build tests" ON)
option(ENABLE_RAPIDCHECK "Enable RapidCheck property tests" OFF)

# Sanitizer toggles (semicolons to enable multiple: address;thread;leak;memory;hwaddress;safe-stack;cfi)
set(SIM_SANITIZERS "" CACHE STRING "Sanitizers to enable for all targets")

# Hardened build switches
option(SIM_WERROR "Treat warnings as errors" ON)
option(SIM_ENABLE_LTO "Enable link time optimization" OFF)
option(SIM_LTO_THIN "Use ThinLTO when LTO is enabled" OFF)
set(SIM_PGO "" CACHE STRING "Profile guided optimisation mode: gen or use")
set(SIM_PGO_PROFILE "" CACHE FILEPATH "Profile data path for PGO")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sanitizers
if (SIM_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  message(STATUS "Enabling sanitizers: ${SIM_SANITIZERS}")
  string(REPLACE ";" "," _san_flags "${SIM_SANITIZERS}")
  add_compile_options(-fsanitize=${_san_flags} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${_san_flags} -fno-omit-frame-pointer)
  if (SIM_SANITIZERS MATCHES "cfi")
    add_compile_options(-flto)
    add_link_options(-flto)
  endif()
endif()

# LTO
if (SIM_ENABLE_LTO)
  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    if (SIM_LTO_THIN)
      add_compile_options(-flto=thin)
      add_link_options(-flto=thin)
    else()
      add_compile_options(-flto)
      add_link_options(-flto)
    endif()
  elseif (MSVC)
    add_compile_options(/GL)
    add_link_options(/LTCG)
  endif()
endif()

# PGO
if (SIM_PGO STREQUAL "gen")
  add_compile_options(-fprofile-generate=${SIM_PGO_PROFILE})
  add_link_options(-fprofile-generate=${SIM_PGO_PROFILE})
elseif(SIM_PGO STREQUAL "use")
  add_compile_options(-fprofile-use=${SIM_PGO_PROFILE} -fprofile-correction)
  add_link_options(-fprofile-use=${SIM_PGO_PROFILE} -fprofile-correction)
endif()


# --- GoogleTest integration ---
# Use FetchContent to obtain googletest at configure time instead of
# expecting a checked out submodule. This keeps the repository light and
# avoids build failures when the submodule is missing.
if (ENABLE_TESTS)
    if (EXISTS "${CMAKE_SOURCE_DIR}/external/googletest/CMakeLists.txt")
        add_subdirectory(external/googletest)
    else()
        include(FetchContent)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        )
        # For Windows: Prevent overriding the parent project's compiler/linker
        # settings on MSVC.
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    if (ENABLE_RAPIDCHECK)
        include(FetchContent)
        FetchContent_Declare(
            rapidcheck
            GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
            GIT_TAG master
        )
        FetchContent_MakeAvailable(rapidcheck)
    endif()
endif()

# Header-only core library
add_library(simcore INTERFACE)
target_include_directories(simcore INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/rt/include
    ${CMAKE_CURRENT_SOURCE_DIR}/hal
)

# Enable warnings for project sources only
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(simcore INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor)
elseif (MSVC)
    target_compile_options(simcore INTERFACE /W4)
endif()

if (SIM_WERROR)
  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(simcore INTERFACE -Werror)
  elseif (MSVC)
    target_compile_options(simcore INTERFACE /WX)
  endif()
endif()

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

# Core module (math, containers)
add_library(simcore_core INTERFACE)
target_include_directories(simcore_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/core/include)

# Runtime module providing C API surface
set(RT_SOURCES rt/src/runtime.cpp)

if (UNIX)
    list(APPEND RT_SOURCES rt/src/crashdump_posix.cpp)
else()
    list(APPEND RT_SOURCES rt/src/crashdump_stub.cpp)
endif()

add_library(simcore_rt STATIC ${RT_SOURCES})
target_include_directories(simcore_rt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/rt/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
)
target_link_libraries(simcore_rt PUBLIC simcore_core)

# Main executable
add_executable(simcore_app src/main.cpp)
target_link_libraries(simcore_app PRIVATE simcore)

# Sample trace dump utility
add_executable(trace_dump src/trace_dump.cpp)
target_link_libraries(trace_dump PRIVATE simcore)
target_include_directories(trace_dump PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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
