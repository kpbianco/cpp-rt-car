#include <rt/cuda_driver.hpp>

int main() {
    const auto driver = rt::cuda_driver_api();
    return driver.api_version == rt::cuda_driver_api_version_2 &&
                   driver.launch_kernel != nullptr &&
                   driver.graph_launch != nullptr
               ? 0
               : 1;
}
