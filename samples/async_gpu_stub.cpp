#include <cassert>
#include <cstddef>
#include <iostream>
#include <vector>

int main() {
    constexpr std::size_t N = 1024;
    std::vector<float> data(N, 1.0f);

#ifdef _OPENMP
#pragma omp target teams distribute parallel for map(tofrom: data[0:N])
    for (std::size_t i = 0; i < N; ++i) {
        data[i] *= 2.0f;
    }
#else
    for (std::size_t i = 0; i < N; ++i) {
        data[i] *= 2.0f;
    }
#endif

    assert(data[0] == 2.0f);
    std::cout << "data[0]=" << data[0] << "\n";
}
