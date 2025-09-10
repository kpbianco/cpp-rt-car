set(TEST_SOURCES
    test_simcore_basic.cpp
    test_simcore_determinism.cpp
    test_logging.cpp
    test_async_logger.cpp
    test_profiler.cpp
    test_adaptive_param.cpp
    test_soa.cpp
    test_sparse_set.cpp
    test_soa_view.cpp
    test_jobqueue.cpp
    test_jobqueue_perf.cpp
    test_workerpool_priority.cpp
    test_phase_graph.cpp
    test_static_phase_graph.cpp
    test_chunk_autotune.cpp
    test_deterministic_reduction.cpp
    test_robust_fp.cpp
    test_budget_monitor.cpp
    test_fault_injection.cpp
    test_simd.cpp
    test_aosoa_autotune.cpp
    test_rt_pool.cpp
    test_rt_arena.cpp
    test_rt_overflow_death.cpp
    test_fiber_pool.cpp
    test_bintrace.cpp
    test_budget_degrade.cpp
    test_predictive_adaptive.cpp
    test_small_array_balance.cpp
    test_phase_telemetry.cpp
    test_aligned_allocator.cpp
    test_highres_clock.cpp
    test_rate_governor.cpp
    test_hazard_ptr.cpp
    test_lockfree_queue.cpp
    test_seqlock_config.cpp
    test_backpressure.cpp
    test_physics_integrators.cpp
    test_constraint_solver.cpp
    test_narrowphase.cpp
    test_hal.cpp
    test_gpu_stub.cpp
    test_frame_graph.cpp
    test_watchdog.cpp
    test_rt_watchdog.cpp
    test_crashdump.cpp
    test_numerics.cpp
    test_prng_determinism.cpp
    test_snapshot.cpp
    test_trace_export.cpp
    test_units.cpp
    test_huge_page.cpp
    test_thermal_limp.cpp
    test_config_hot_reload.cpp
    test_plugin_manager.cpp
    test_sandbox.cpp
    test_sbom.cpp
    test_minidump_symbols.cpp
    test_perf_artifacts.cpp
    test_plugin_manager.cpp
    test_scheduler.cpp
)

if (ENABLE_RAPIDCHECK AND TARGET rapidcheck_gtest)
    list(APPEND TEST_SOURCES test_jobqueue_property.cpp)
endif()

add_executable(simcore_tests ${TEST_SOURCES})
target_link_libraries(simcore_tests PRIVATE simcore simcore_rt gtest gtest_main gmock)
if (ENABLE_RAPIDCHECK AND TARGET rapidcheck_gtest)
    target_link_libraries(simcore_tests PRIVATE rapidcheck rapidcheck_gtest)
endif()
target_include_directories(simcore_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}
)
target_compile_definitions(simcore_tests PRIVATE PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_library(test_sample_plugin SHARED sample_plugin.c)
target_include_directories(test_sample_plugin PRIVATE ${CMAKE_SOURCE_DIR}/rt/include)
set_target_properties(test_sample_plugin PROPERTIES OUTPUT_NAME sample_plugin)

add_library(test_incompatible_plugin SHARED sample_plugin_bad.c)
target_include_directories(test_incompatible_plugin PRIVATE ${CMAKE_SOURCE_DIR}/rt/include)
set_target_properties(test_incompatible_plugin PROPERTIES OUTPUT_NAME bad_plugin)

add_dependencies(simcore_tests test_sample_plugin test_incompatible_plugin)
target_compile_definitions(simcore_tests PRIVATE
    PLUGIN_PATH="$<TARGET_FILE:test_sample_plugin>"
    BAD_PLUGIN_PATH="$<TARGET_FILE:test_incompatible_plugin>"
)

add_test(NAME simcore_all COMMAND simcore_tests)

option(SIM_ENABLE_AVX2 "Enable AVX2/FMA for SIMD kernels" ON)
if (SIM_ENABLE_AVX2 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
  add_compile_options(-mavx2 -mfma)
endif()

option(SIM_BUILD_FUZZERS "Build libFuzzer harnesses" OFF)
if (SIM_BUILD_FUZZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_executable(jobqueue_fuzz jobqueue_fuzz.cpp)
    target_link_libraries(jobqueue_fuzz PRIVATE simcore)
    target_compile_options(jobqueue_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(jobqueue_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

# --- affinity test -------------------------------------------------
if (UNIX AND NOT APPLE)   # linux
    list(APPEND TEST_SOURCES tests/test_affinity.cpp tests/test_numa_pool.cpp)
    find_library(LIBNUMA numa)
    find_path(NUMA_INCLUDE_DIR numa.h)
    if (LIBNUMA AND NUMA_INCLUDE_DIR)
        target_compile_definitions(simcore_tests PRIVATE SIM_USE_NUMA)
        target_link_libraries(simcore_tests PRIVATE ${LIBNUMA})
        target_include_directories(simcore_tests PRIVATE ${NUMA_INCLUDE_DIR})
    endif()
endif()
