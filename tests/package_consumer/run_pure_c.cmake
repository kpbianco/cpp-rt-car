if (NOT DEFINED RTFW_PURE_C_SOURCE_DIR OR
    NOT DEFINED RTFW_PURE_C_BINARY_DIR OR
    NOT DEFINED RTFW_PURE_C_PREFIX OR
    NOT DEFINED RTFW_PURE_C_CONFIG)
    message(FATAL_ERROR "pure-C package test arguments are incomplete")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -S "${RTFW_PURE_C_SOURCE_DIR}"
        -B "${RTFW_PURE_C_BINARY_DIR}"
        "-DCMAKE_BUILD_TYPE=${RTFW_PURE_C_CONFIG}"
        "-DCMAKE_PREFIX_PATH=${RTFW_PURE_C_PREFIX}"
    RESULT_VARIABLE configure_result)
if (NOT configure_result EQUAL 0)
    message(FATAL_ERROR "pure-C package configure failed")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        --build "${RTFW_PURE_C_BINARY_DIR}"
        --config "${RTFW_PURE_C_CONFIG}"
    RESULT_VARIABLE build_result)
if (NOT build_result EQUAL 0)
    message(FATAL_ERROR "pure-C package build failed")
endif()

execute_process(
    COMMAND
        "${CMAKE_CTEST_COMMAND}"
        --test-dir "${RTFW_PURE_C_BINARY_DIR}"
        --build-config "${RTFW_PURE_C_CONFIG}"
        --output-on-failure
    RESULT_VARIABLE test_result)
if (NOT test_result EQUAL 0)
    message(FATAL_ERROR "pure-C package execution failed")
endif()
