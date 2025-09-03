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
    test_rt_pool.cpp
    test_rt_arena.cpp
    test_rt_overflow_death.cpp
    test_bintrace.cpp
    test_predictive_adaptive.cpp
    test_small_array_balance.cpp
    test_aligned_allocator.cpp
)

add_executable(simcore_tests ${TEST_SOURCES})
target_link_libraries(simcore_tests PRIVATE simcore gtest gtest_main gmock)
target_include_directories(simcore_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)

add_test(NAME simcore_all COMMAND simcore_tests)

option(SIM_ENABLE_AVX2 "Enable AVX2/FMA for SIMD kernels" ON)
if (SIM_ENABLE_AVX2 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
  add_compile_options(-mavx2 -mfma)
endif()


# --- affinity test -------------------------------------------------
if (UNIX AND NOT APPLE)   # linux
    list(APPEND TEST_SOURCES tests/test_affinity.cpp)
    find_library(LIBNUMA numa)
    if (LIBNUMA)
        target_compile_definitions(simcore_tests PRIVATE SIM_USE_NUMA)
        target_link_libraries(simcore_tests PRIVATE ${LIBNUMA})
    endif()
endif()
