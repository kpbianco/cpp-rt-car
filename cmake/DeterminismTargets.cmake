if (NOT TARGET simcore_tests)
    return()
endif()

if (NOT TARGET simcore_app)
    return()
endif()

add_custom_target(determinism_e2e
    COMMAND ${CMAKE_COMMAND} -E env
            GTEST_FILTER=DeterminismIntegration.*
            ${CMAKE_CTEST_COMMAND}
            --test-dir ${RTFW_PROJECT_BINARY_DIR}
            --output-on-failure
            -R simcore_all
    DEPENDS simcore_tests simcore_app
    USES_TERMINAL
    COMMENT "Run determinism integration suite")
