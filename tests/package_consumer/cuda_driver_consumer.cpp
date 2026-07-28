#include <rt/cuda_driver.hpp>

int main() {
    const auto driver = rt::cuda_driver_api();
    return driver.api_version == rt::cuda_driver_api_version &&
                   driver.launch_kernel != nullptr
               ? 0
               : 1;
}
