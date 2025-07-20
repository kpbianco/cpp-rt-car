set(TEST_SOURCES
    test_simcore_basic.cpp
    test_simcore_determinism.cpp
    test_logging.cpp
    test_profiler.cpp
    test_adaptive_param.cpp
)

add_executable(simcore_tests ${TEST_SOURCES})
target_link_libraries(simcore_tests PRIVATE simcore gtest gtest_main gmock)
target_include_directories(simcore_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests
)

add_test(NAME simcore_all COMMAND simcore_tests)
