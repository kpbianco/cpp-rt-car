cmake_minimum_required(VERSION 3.20)
project(rtfw_pure_c_consumer LANGUAGES C)

find_package(
    rtfw 1.2 CONFIG REQUIRED
    COMPONENTS c_shared c_static)

add_executable(rtfw_pure_c_shared main.c)
target_link_libraries(rtfw_pure_c_shared PRIVATE rtfw::c_shared)

add_executable(rtfw_pure_c_static main.c)
target_link_libraries(rtfw_pure_c_static PRIVATE rtfw::c_static)

enable_testing()
add_test(NAME pure_c_shared COMMAND rtfw_pure_c_shared)
add_test(NAME pure_c_static COMMAND rtfw_pure_c_static)
