cmake_minimum_required(VERSION 3.20)
project(rtfw_add_subdirectory_consumer LANGUAGES CXX)

if (NOT DEFINED RTFW_SOURCE_DIR)
    message(FATAL_ERROR "RTFW_SOURCE_DIR must name the RTFW source tree")
endif()

option(
    RTFW_EXPECT_TESTS
    "Require the explicitly opted-in RTFW subproject test targets"
    OFF)

# Simulate a host project with a generic test option. RTFW must remain a
# library-only dependency unless its namespaced test switch is explicitly set.
set(ENABLE_TESTS ON CACHE BOOL "Parent project tests" FORCE)
add_subdirectory("${RTFW_SOURCE_DIR}" rtfw-build)

foreach(target
        rtfw::c_shared
        rtfw::c_static
        rtfw::runtime
        rtfw::rtfw
        rtfw::rtfw_static
        rtfw::simcore_rt
        rtfw::cuda_backend
        rtfw::xdma_backend)
    if (NOT TARGET "${target}")
        message(FATAL_ERROR
            "add_subdirectory package is missing target ${target}")
    endif()
endforeach()

foreach(forbidden_target package package_source)
    if (TARGET "${forbidden_target}")
        message(FATAL_ERROR
            "RTFW created parent-visible target ${forbidden_target}")
    endif()
endforeach()

if (RTFW_EXPECT_TESTS)
    foreach(test_target
            rtfw_tests
            test_cabi_dlopen
            rtfw_host_adapter_tests
            rtfw_runtime_profile_tests
            rtfw_xdma_backend_tests)
        if (NOT TARGET "${test_target}")
            message(FATAL_ERROR
                "RTFW test opt-in is missing target ${test_target}")
        endif()
    endforeach()
else()
    foreach(forbidden_target rtfw_tests simcore_tests)
        if (TARGET "${forbidden_target}")
            message(FATAL_ERROR
                "RTFW created parent-visible target ${forbidden_target}")
        endif()
    endforeach()
endif()

add_executable(rtfw_add_subdirectory_consumer main.cpp)
target_link_libraries(
    rtfw_add_subdirectory_consumer
    PRIVATE
        rtfw::runtime)
