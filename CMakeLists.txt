cmake_minimum_required(VERSION 3.20)

project(car_sim_core LANGUAGES C CXX)

# Options (toggle logging / profiler / tests)
option(ENABLE_LOG "Enable logging" ON)
option(ENABLE_PROF "Enable profiler" ON)
option(ENABLE_TESTS "Build tests" ON)
option(ENABLE_RAPIDCHECK "Enable RapidCheck property tests" OFF)
option(SIM_ENABLE_IWYU "Run include-what-you-use during builds" OFF)
option(SIM_WARN_ODR "Enable -Wodr to detect ODR violations" OFF)

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

# IWYU
if (SIM_ENABLE_IWYU)
  find_program(IWYU_PATH NAMES include-what-you-use iwyu)
  if (IWYU_PATH)
    set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE ${IWYU_PATH})
  else()
    message(WARNING "include-what-you-use requested but not found")
  endif()
endif()

# ODR warnings
if (SIM_WARN_ODR AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wodr)
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
    option(SIM_DOWNLOAD_GTEST "Download GoogleTest if it is not provided" ON)
    set(SIM_GTEST_AVAILABLE OFF)

    if (EXISTS "${CMAKE_SOURCE_DIR}/external/googletest/CMakeLists.txt")
        add_subdirectory(external/googletest)
        set(SIM_GTEST_AVAILABLE ON)
        set(SIM_GTEST_LIBRARIES gtest gtest_main gmock)
    else()
        find_package(GTest CONFIG QUIET)
        if (GTest_FOUND)
            set(SIM_GTEST_AVAILABLE ON)
            set(SIM_GTEST_LIBRARIES GTest::gtest)
            if (TARGET GTest::gtest_main)
                list(APPEND SIM_GTEST_LIBRARIES GTest::gtest_main)
            endif()
            if (TARGET GTest::gmock)
                list(APPEND SIM_GTEST_LIBRARIES GTest::gmock)
            endif()
        elseif (SIM_DOWNLOAD_GTEST)
            include(FetchContent)
            FetchContent_Declare(
                googletest
                URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
                DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            )
            # For Windows: Prevent overriding the parent project's compiler/linker
            # settings on MSVC.
            set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
            FetchContent_MakeAvailable(googletest)
            set(SIM_GTEST_AVAILABLE ON)
            set(SIM_GTEST_LIBRARIES gtest gtest_main gmock)
        else()
            message(WARNING "GoogleTest not available; gtest-based tests will be skipped.")
        endif()
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
    ${CMAKE_CURRENT_SOURCE_DIR}/gpu
)

add_library(simcore_platform STATIC src/platform_init.cpp)
target_include_directories(simcore_platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(simcore_platform PUBLIC simcore)
set_target_properties(simcore_platform PROPERTIES POSITION_INDEPENDENT_CODE ON)

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
set(RT_SOURCES
    rt/src/runtime.cpp
    rt/src/plugin_manager.cpp
    rt/src/scheduler.cpp
    rt/src/fiber_pool.cpp
)

if (UNIX)
    list(APPEND RT_SOURCES rt/src/crashdump_posix.cpp)
else()
    list(APPEND RT_SOURCES rt/src/crashdump_stub.cpp)
endif()

add_library(simcore_rt STATIC ${RT_SOURCES})
target_include_directories(simcore_rt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/rt/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/hal
)
target_link_libraries(simcore_rt PUBLIC simcore_core)
set_target_properties(simcore_rt PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (UNIX)
    target_link_libraries(simcore_rt PUBLIC dl)
endif()

add_library(rtfw SHARED src/c_abi.cpp)
target_include_directories(rtfw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rtfw PRIVATE simcore simcore_platform simcore_rt)
target_compile_definitions(rtfw PRIVATE RTFW_BUILD=1)
set_target_properties(rtfw PROPERTIES OUTPUT_NAME rtfw)

# Main executable
add_executable(simcore_app src/main.cpp)
target_link_libraries(simcore_app PRIVATE simcore_platform)

# Sample trace dump utility
add_executable(trace_dump src/trace_dump.cpp)
target_link_libraries(trace_dump PRIVATE simcore_platform)
target_include_directories(trace_dump PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Tiny sample programs
add_subdirectory(samples)

# Tests
if (ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Optional Bloaty size analysis
find_program(BLOATY_EXE NAMES bloaty)
if (BLOATY_EXE)
  add_custom_target(bloaty
    COMMAND ${BLOATY_EXE} $<TARGET_FILE:simcore_app> -d compileunits,symbols -n 0.1
    DEPENDS simcore_app
    COMMENT "Run Bloaty on simcore_app")
endif()

add_custom_target(tests
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/build      # wipe  ▸ optional
    COMMAND ${CMAKE_COMMAND} -DENABLE_TESTS=ON -DCMAKE_BUILD_TYPE=Debug ${CMAKE_SOURCE_DIR}
    COMMAND ${CMAKE_COMMAND} --build . -- -j
    COMMAND ctest --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Configure, build and run all tests")
