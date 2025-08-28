set(TEST_SOURCES
    test_simcore_basic.cpp
    test_simcore_determinism.cpp
    test_logging.cpp
    test_profiler.cpp
    test_adaptive_param.cpp
    test_soa.cpp
    test_jobqueue.cpp
    test_phase_graph.cpp
    test_chunk_autotune.cpp
    test_deterministic_reduction.cpp
    test_budget_monitor.cpp
    test_simd.cpp
)

add_executable(simcore_tests ${TEST_SOURCES})
target_link_libraries(simcore_tests PRIVATE simcore gtest gtest_main gmock)
target_include_directories(simcore_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests
)

add_test(NAME simcore_all COMMAND simcore_tests)

option(SIM_ENABLE_AVX2 "Enable AVX2/FMA for SIMD kernels" ON)
if (SIM_ENABLE_AVX2 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
  add_compile_options(-mavx2 -mfma)
endif()

# Make sure tests see the headers
target_include_directories(simcore_tests PRIVATE ${CMAKE_SOURCE_DIR})

# --- affinity test -------------------------------------------------
if (UNIX AND NOT APPLE)   # linux
    list(APPEND TEST_SOURCES tests/test_affinity.cpp)
    find_library(LIBNUMA numa)
    if (LIBNUMA)
        target_compile_definitions(simcore_tests PRIVATE SIM_USE_NUMA)
        target_link_libraries(simcore_tests PRIVATE ${LIBNUMA})
    endif()
endif()
