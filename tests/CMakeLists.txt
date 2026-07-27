if (NOT DEFINED SIM_GTEST_AVAILABLE)
    set(SIM_GTEST_AVAILABLE OFF)
endif()

if (SIM_GTEST_AVAILABLE)
    set(TEST_SOURCES
        test_simcore_basic.cpp
        test_simcore_determinism.cpp
        test_logging.cpp
        test_async_logger.cpp
        test_logger_backpressure.cpp
        test_profiler.cpp
        test_adaptive_param.cpp
        test_soa.cpp
        test_sparse_set.cpp
        test_soa_view.cpp
        test_jobqueue.cpp
        test_queue_metrics.cpp
        test_metrics_json.cpp
        test_jobqueue_perf.cpp
        test_workerpool_priority.cpp
        test_phase_graph.cpp
        test_static_phase_graph.cpp
        test_chunk_autotune.cpp
        test_deterministic_reduce.cpp
        test_differential_output.cpp
        test_robust_fp.cpp
        test_budget_monitor.cpp
        test_fault_injection.cpp
        test_simd.cpp
        test_aosoa_autotune.cpp
        test_rt_pool.cpp
        test_rt_arena.cpp
        test_arena_binding_assert.cpp
        test_rt_overflow_death.cpp
        test_fiber_pool.cpp
        test_fiber_pool_race.cpp
        test_bintrace.cpp
        test_budget_degrade.cpp
        test_predictive_adaptive.cpp
        test_small_array_balance.cpp
        test_phase_telemetry.cpp
        test_aligned_allocator.cpp
        test_highres_clock.cpp
        test_highres_clock_integration.cpp
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
        test_gpu_overlap.cpp
        test_frame_graph.cpp
        test_watchdog.cpp
        test_rt_watchdog.cpp
        test_crashdump.cpp
        test_numerics.cpp
        test_prng_determinism.cpp
        test_snapshot.cpp
        test_trace_export.cpp
        test_trace_noalloc.cpp
        test_units.cpp
        test_huge_page.cpp
        test_thermal_limp.cpp
        test_limp_mode.cpp
        test_config_hot_reload.cpp
        test_plugin_manager.cpp
        test_sandbox.cpp
        test_sbom.cpp
        test_minidump_symbols.cpp
        test_perf_artifacts.cpp
        test_scheduler.cpp
        test_host_runtime.cpp
        test_periodic_runtime.cpp
        test_platform_preflight.cpp
        test_compiled_graph.cpp
        test_executor.cpp
        test_memory_plan.cpp
        test_observability.cpp
        test_determinism_replay.cpp
        test_device_runtime.cpp
        test_cuda_backend.cpp
        test_rt_pipeline.cpp
        test_platform_gate.cpp
        integration/test_determinism.cpp
    )

    if (ENABLE_RAPIDCHECK AND TARGET rapidcheck_gtest)
        list(APPEND TEST_SOURCES test_jobqueue_property.cpp)
    endif()

    if (UNIX AND NOT APPLE)   # linux
        list(APPEND TEST_SOURCES
            test_affinity.cpp
            test_numa_pool.cpp
            test_numa_integration.cpp
        )
    endif()

    add_executable(simcore_tests ${TEST_SOURCES})
    target_link_libraries(
        simcore_tests
        PRIVATE
            simcore_platform
            simcore_rt
            rtfw_cuda_backend
            ${SIM_GTEST_LIBRARIES})
    if (ENABLE_RAPIDCHECK AND TARGET rapidcheck_gtest)
        target_link_libraries(simcore_tests PRIVATE rapidcheck rapidcheck_gtest)
    endif()
    target_include_directories(simcore_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}
    )
    target_compile_definitions(simcore_tests PRIVATE
        PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        RTFW_DEMO_PATH="$<TARGET_FILE:simcore_app>"
    )

    add_dependencies(simcore_tests simcore_app)

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
    set_tests_properties(simcore_all PROPERTIES TIMEOUT 180)

    if (SIM_ENABLE_AVX2 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
      add_compile_options(-mavx2 -mfma)
    endif()

    if (UNIX AND NOT APPLE)
        find_library(LIBNUMA numa)
        find_path(NUMA_INCLUDE_DIR numa.h)
        if (LIBNUMA AND NUMA_INCLUDE_DIR)
            target_compile_definitions(simcore_tests PRIVATE SIM_USE_NUMA)
            target_link_libraries(simcore_tests PRIVATE ${LIBNUMA})
            target_include_directories(simcore_tests PRIVATE ${NUMA_INCLUDE_DIR})
        endif()
    endif()
endif()

add_executable(rtfw_determinism_artifact determinism_artifact.cpp)
target_link_libraries(rtfw_determinism_artifact PRIVATE simcore_rt)

add_executable(test_cabi_dlopen test_cabi_dlopen.c)
target_include_directories(test_cabi_dlopen PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/rt/include)
target_compile_definitions(test_cabi_dlopen PRIVATE RTFW_LIB_PATH="$<TARGET_FILE:rtfw>")
if (UNIX)
    target_link_libraries(test_cabi_dlopen PRIVATE dl)
endif()
add_test(NAME cabi_dlopen COMMAND test_cabi_dlopen)
set_tests_properties(cabi_dlopen PROPERTIES TIMEOUT 30)

add_executable(simcore_thread_violation EXCLUDE_FROM_ALL thread_violation.cpp)
target_link_libraries(simcore_thread_violation PRIVATE simcore_platform)
target_compile_definitions(simcore_thread_violation PRIVATE RTFW_ENFORCE_NO_RAW_THREADS=1)
add_test(NAME simcore_thread_violation_build
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target simcore_thread_violation --config $<CONFIG>)
set_tests_properties(simcore_thread_violation_build PROPERTIES
    TIMEOUT 120
    WILL_FAIL TRUE)

if (SIM_BUILD_FUZZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_executable(jobqueue_fuzz jobqueue_fuzz.cpp)
    target_link_libraries(jobqueue_fuzz PRIVATE simcore_platform)
    target_compile_options(jobqueue_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(jobqueue_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

    # Compile the codec into the fuzz binary so ASan/UBSan instrument the
    # parser itself rather than only the libFuzzer entry point.
    add_executable(snapshot_fuzz
        snapshot_fuzz.cpp
        ${CMAKE_SOURCE_DIR}/rt/src/snapshot_codec.cpp)
    target_link_libraries(snapshot_fuzz PRIVATE simcore simcore_core)
    target_compile_options(snapshot_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(snapshot_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

# Portable M10 XDMA state-machine tests do not require GoogleTest, vendor
# headers, a kernel driver, or hardware.
add_executable(rtfw_xdma_backend_tests xdma_backend_tests.cpp)
target_link_libraries(
    rtfw_xdma_backend_tests PRIVATE rtfw_xdma_backend)
add_test(NAME xdma_backend COMMAND rtfw_xdma_backend_tests)
set_tests_properties(xdma_backend PROPERTIES TIMEOUT 60)
