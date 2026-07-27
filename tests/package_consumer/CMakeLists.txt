cmake_minimum_required(VERSION 3.20)
project(rtfw_package_consumer LANGUAGES C CXX)

find_package(
    rtfw 0.12 CONFIG REQUIRED
    COMPONENTS c_shared c_static cpp_runtime)

if (NOT RTFW_C_ABI_VERSION EQUAL 8)
    message(FATAL_ERROR "Expected installed RTFW C ABI version 8")
endif()

add_executable(rtfw_consumer_shared c_consumer.c)
target_link_libraries(rtfw_consumer_shared PRIVATE rtfw::rtfw)

add_executable(rtfw_consumer_static c_consumer.c)
target_link_libraries(rtfw_consumer_static PRIVATE rtfw::rtfw_static)

add_executable(rtfw_consumer_cpp cpp_consumer.cpp)
target_compile_features(rtfw_consumer_cpp PRIVATE cxx_std_20)
target_link_libraries(rtfw_consumer_cpp PRIVATE rtfw::simcore_rt)

if (WIN32)
    add_custom_command(
        TARGET rtfw_consumer_shared
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rtfw::rtfw>
            $<TARGET_FILE_DIR:rtfw_consumer_shared>
        VERBATIM)
endif()

enable_testing()
add_test(NAME installed_c_shared COMMAND rtfw_consumer_shared)
add_test(NAME installed_c_static COMMAND rtfw_consumer_static)
add_test(NAME installed_cpp_runtime COMMAND rtfw_consumer_cpp)
