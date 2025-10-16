add_executable(sample_particles particles.cpp)
target_link_libraries(sample_particles PRIVATE simcore_platform)

add_executable(sample_tyre_belt tyre_belt_kernel.cpp)
target_link_libraries(sample_tyre_belt PRIVATE simcore_platform)

add_executable(sample_async_gpu async_gpu_stub.cpp)
target_link_libraries(sample_async_gpu PRIVATE simcore_platform)

add_executable(sample_embed_c embed_c/mini_app.c)
target_link_libraries(sample_embed_c PRIVATE rtfw)

if (BUILD_TESTING)
    add_test(NAME sample_particles COMMAND sample_particles)
    add_test(NAME sample_tyre_belt COMMAND sample_tyre_belt)
    add_test(NAME sample_async_gpu COMMAND sample_async_gpu)
    add_test(NAME sample_embed_c COMMAND sample_embed_c)
endif()
