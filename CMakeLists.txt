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

# Helper to link shared objects against sanitizer runtimes when we need to
# satisfy the dynamic loader (e.g. for dlopen-based smoke tests).
function(simcore_link_sanitizer_runtime target)
  if (NOT SIM_SANITIZERS)
    return()
  endif()
  if (MSVC)
    return()
  endif()
  if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    return()
  endif()

  set(_runtime_tags)
  foreach(_san IN LISTS SIM_SANITIZERS)
    if (_san STREQUAL "address" OR _san STREQUAL "leak")
      list(APPEND _runtime_tags asan)
    elseif (_san STREQUAL "thread")
      list(APPEND _runtime_tags tsan)
    elseif (_san STREQUAL "memory")
      list(APPEND _runtime_tags msan)
    elseif (_san STREQUAL "undefined")
      list(APPEND _runtime_tags ubsan)
    elseif (_san STREQUAL "hwaddress")
      list(APPEND _runtime_tags hwasan)
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _runtime_tags)
  if (NOT _runtime_tags)
    return()
  endif()

  list(JOIN SIM_SANITIZERS "," _san_flags)

  set(_runtime_patterns)
  foreach(_tag IN LISTS _runtime_tags)
    if (_tag STREQUAL "asan")
      list(APPEND _runtime_patterns "asan")
    elseif (_tag STREQUAL "ubsan")
      list(APPEND _runtime_patterns "ubsan")
    elseif (_tag STREQUAL "tsan")
      list(APPEND _runtime_patterns "tsan")
    elseif (_tag STREQUAL "msan")
      list(APPEND _runtime_patterns "msan")
    elseif (_tag STREQUAL "hwasan")
      list(APPEND _runtime_patterns "hwasan")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _runtime_patterns)
  if (NOT _runtime_patterns)
    return()
  endif()

  set(_driver_libs)
  if (CMAKE_CXX_COMPILER)
    set(_dummy_dir "${CMAKE_BINARY_DIR}/CMakeFiles")
    file(MAKE_DIRECTORY "${_dummy_dir}")
    set(_dummy_src "${_dummy_dir}/simcore_sanitize_dummy.cpp")
    if (NOT EXISTS "${_dummy_src}")
      file(WRITE "${_dummy_src}" "int simcore_sanitize_dummy() { return 0; }\n")
    endif()
    set(_dummy_out "${_dummy_dir}/libsimcore_sanitize_dummy.so")

    execute_process(
      COMMAND ${CMAKE_CXX_COMPILER} -### -x c++ "${_dummy_src}" -shared -fsanitize=${_san_flags} -fno-omit-frame-pointer -o "${_dummy_out}"
      OUTPUT_VARIABLE _driver_stdout
      ERROR_VARIABLE _driver_stderr
      RESULT_VARIABLE _driver_result
    )
    if (_driver_result EQUAL 0)
      set(_driver_output "${_driver_stdout}\n${_driver_stderr}")
      string(REGEX MATCHALL "\"([^\"]+)\"" _quoted_tokens "${_driver_output}")
      foreach(_token IN LISTS _quoted_tokens)
        string(REGEX REPLACE "^\"|\"$" "" _value "${_token}")
        foreach(_pattern IN LISTS _runtime_patterns)
          if (_value MATCHES "${_pattern}")
            list(APPEND _driver_libs "${_value}")
            break()
          endif()
        endforeach()
      endforeach()
    endif()
  endif()

  set(_resolved_libs)
  set(_static_candidates)
  foreach(_lib IN LISTS _driver_libs)
    if (_lib MATCHES "^-Wl,")
      list(APPEND _resolved_libs "${_lib}")
    elseif (_lib MATCHES "^-l.+")
      list(APPEND _resolved_libs "${_lib}")
    elseif (IS_ABSOLUTE "${_lib}" AND EXISTS "${_lib}")
      if (_lib MATCHES "\\.a$")
        string(REGEX REPLACE "\\.a$" ".so" _maybe_shared "${_lib}")
        if (EXISTS "${_maybe_shared}")
          list(APPEND _resolved_libs "${_maybe_shared}")
        else()
          string(REGEX REPLACE "\\.a$" ".dylib" _maybe_dylib "${_lib}")
          if (EXISTS "${_maybe_dylib}")
            list(APPEND _resolved_libs "${_maybe_dylib}")
          else()
            list(APPEND _static_candidates "${_lib}")
          endif()
        endif()
      else()
        list(APPEND _resolved_libs "${_lib}")
      endif()
    endif()
  endforeach()

  if (NOT _resolved_libs)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _arch_lower)
    if (_arch_lower STREQUAL "x86_64" OR _arch_lower STREQUAL "amd64")
      set(_arch_token "x86_64")
    elseif (_arch_lower STREQUAL "aarch64" OR _arch_lower STREQUAL "arm64")
      set(_arch_token "aarch64")
    else()
      set(_arch_token "${_arch_lower}")
    endif()

    foreach(_tag IN LISTS _runtime_tags)
      set(_gnu_candidates)
      set(_clang_candidates)
      if (_tag STREQUAL "asan")
        set(_gnu_candidates
          "libasan.so"
          "libasan.a"
        )
        set(_clang_candidates
          "libclang_rt.asan-${_arch_token}.so"
          "libclang_rt.asan-${_arch_token}.a"
          "libclang_rt.asan_cxx-${_arch_token}.so"
          "libclang_rt.asan_cxx-${_arch_token}.a"
        )
      elseif (_tag STREQUAL "ubsan")
        set(_gnu_candidates
          "libubsan.so"
          "libubsan.a"
        )
        set(_clang_candidates
          "libclang_rt.ubsan_standalone-${_arch_token}.so"
          "libclang_rt.ubsan_standalone-${_arch_token}.a"
          "libclang_rt.ubsan_standalone_cxx-${_arch_token}.so"
          "libclang_rt.ubsan_standalone_cxx-${_arch_token}.a"
        )
      elseif (_tag STREQUAL "tsan")
        set(_gnu_candidates
          "libtsan.so"
          "libtsan.a"
        )
        set(_clang_candidates
          "libclang_rt.tsan-${_arch_token}.so"
          "libclang_rt.tsan-${_arch_token}.a"
        )
      elseif (_tag STREQUAL "msan")
        set(_clang_candidates
          "libclang_rt.msan-${_arch_token}.so"
          "libclang_rt.msan-${_arch_token}.a"
        )
      elseif (_tag STREQUAL "hwasan")
        set(_clang_candidates
          "libclang_rt.hwasan-${_arch_token}.so"
          "libclang_rt.hwasan-${_arch_token}.a"
        )
      endif()

      set(_candidates)
      if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        list(APPEND _candidates ${_clang_candidates} ${_gnu_candidates})
      else()
        list(APPEND _candidates ${_gnu_candidates} ${_clang_candidates})
      endif()

      foreach(_candidate IN LISTS _candidates)
        execute_process(
          COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=${_candidate}
          OUTPUT_VARIABLE _candidate_path
          OUTPUT_STRIP_TRAILING_WHITESPACE
          ERROR_QUIET
        )
        if (_candidate_path AND NOT _candidate_path STREQUAL ${_candidate}
            AND EXISTS "${_candidate_path}")
          if (_candidate_path MATCHES "\\.a$")
            string(REGEX REPLACE "\\.a$" ".so" _maybe_shared "${_candidate_path}")
            if (EXISTS "${_maybe_shared}")
              list(APPEND _resolved_libs "${_maybe_shared}")
            else()
              string(REGEX REPLACE "\\.a$" ".dylib" _maybe_dylib "${_candidate_path}")
              if (EXISTS "${_maybe_dylib}")
                list(APPEND _resolved_libs "${_maybe_dylib}")
              else()
                list(APPEND _static_candidates "${_candidate_path}")
              endif()
            endif()
          else()
            list(APPEND _resolved_libs "${_candidate_path}")
          endif()
          break()
        endif()
      endforeach()
    endforeach()
  endif()

  set(_link_libs ${_resolved_libs})
  if (NOT _link_libs AND _static_candidates)
    list(REMOVE_DUPLICATES _static_candidates)
    set(_link_libs ${_static_candidates})
    message(STATUS "Sanitizer runtimes for ${target}: using static runtime fallback ${_static_candidates}")
  endif()

  if (_link_libs)
    list(REMOVE_DUPLICATES _link_libs)
    target_link_libraries(${target} PRIVATE ${_link_libs})
    message(STATUS "Sanitizer runtimes for ${target}: ${_link_libs}")
  endif()
endfunction()

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
simcore_link_sanitizer_runtime(rtfw)

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
