add_executable(sample_particles particles.cpp)
target_link_libraries(sample_particles PRIVATE simcore)

add_executable(sample_tyre_belt tyre_belt_kernel.cpp)
target_link_libraries(sample_tyre_belt PRIVATE simcore)

add_executable(sample_async_gpu async_gpu_stub.cpp)
target_link_libraries(sample_async_gpu PRIVATE simcore)

if (BUILD_TESTING)
    add_test(NAME sample_particles COMMAND sample_particles)
    add_test(NAME sample_tyre_belt COMMAND sample_tyre_belt)
    add_test(NAME sample_async_gpu COMMAND sample_async_gpu)
endif()
