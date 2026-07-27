add_executable(sample_particles particles.cpp)
target_link_libraries(sample_particles PRIVATE simcore_platform)

add_executable(sample_tyre_belt tyre_belt_kernel.cpp)
target_link_libraries(sample_tyre_belt PRIVATE simcore_platform)

add_executable(sample_async_gpu async_gpu_stub.cpp)
target_link_libraries(sample_async_gpu PRIVATE simcore_platform)

add_executable(sample_embed_c embed_c/mini_app.c)
target_link_libraries(sample_embed_c PRIVATE rtfw)
if (WIN32)
    add_custom_command(
        TARGET sample_embed_c
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rtfw_shared>
            $<TARGET_FILE_DIR:sample_embed_c>
        COMMENT "Staging the RTFW DLL beside the shared C embedding sample"
        VERBATIM)
endif()

add_executable(sample_embed_c_static embed_c/mini_app.c)
target_link_libraries(sample_embed_c_static PRIVATE rtfw_static)

add_executable(sample_embed_cpp embed_cpp/mini_app.cpp)
target_link_libraries(sample_embed_cpp PRIVATE simcore_rt)

add_executable(sample_device_mock device_mock.cpp)
target_link_libraries(sample_device_mock PRIVATE simcore_rt)

if (RTFW_ENABLE_CUDA)
    add_executable(sample_cuda_qualification cuda_qualification.cpp)
    target_link_libraries(
        sample_cuda_qualification
        PRIVATE rtfw_cuda_driver)
endif()

if (RTFW_ENABLE_XDMA)
    add_executable(sample_xdma_qualification xdma_qualification.cpp)
    target_link_libraries(
        sample_xdma_qualification
        PRIVATE rtfw_xdma_linux)
endif()

if (ENABLE_TESTS)
    add_test(NAME sample_particles COMMAND sample_particles)
    add_test(NAME sample_tyre_belt COMMAND sample_tyre_belt)
    add_test(NAME sample_async_gpu COMMAND sample_async_gpu)
    add_test(NAME sample_embed_c COMMAND sample_embed_c)
    add_test(NAME sample_embed_c_static COMMAND sample_embed_c_static)
    add_test(NAME sample_embed_cpp COMMAND sample_embed_cpp)
    add_test(NAME sample_device_mock COMMAND sample_device_mock)
    set_tests_properties(
        sample_particles
        sample_tyre_belt
        sample_async_gpu
        sample_embed_c
        sample_embed_c_static
        sample_embed_cpp
        sample_device_mock
        PROPERTIES TIMEOUT 30)
endif()
