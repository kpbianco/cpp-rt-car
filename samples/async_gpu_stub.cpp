#include <cassert>
#include <vector>
#include <iostream>

int main() {
    const int N = 1024;
    std::vector<float> data(N, 1.0f);

#ifdef _OPENMP
#pragma omp target teams distribute parallel for map(tofrom: data[0:N])
    for (int i = 0; i < N; ++i) {
        data[i] *= 2.0f;
    }
#else
    for (int i = 0; i < N; ++i) {
        data[i] *= 2.0f;
    }
#endif

    assert(data[0] == 2.0f);
    std::cout << "data[0]=" << data[0] << "\n";
}
