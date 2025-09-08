#include <cassert>
#include <cmath>
#include <numbers>
#include <iostream>
#include <numeric>
#include <vector>

int main() {
    const std::size_t samples = 360;       // angular samples around drum
    const double step = 2.0 * std::numbers::pi / static_cast<double>(samples);

    std::vector<double> tension(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        double theta = static_cast<double>(i) * step;
        // simple sinusoidal variation in belt tension
        tension[i] = 1000.0 + 100.0 * std::sin(theta);
    }

    double avg =
        std::accumulate(tension.begin(), tension.end(), 0.0) /
        static_cast<double>(samples);
    assert(std::abs(avg - 1000.0) < 1e-6);
    std::cout << "Average belt tension: " << avg << " N\n";
}
