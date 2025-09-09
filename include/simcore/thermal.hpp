#pragma once
#include <cstddef>
#include <fstream>
#include <string>

namespace sim {

inline double read_package_temp_c() {
#ifdef __linux__
    std::ifstream f("/sys/class/thermal/thermal_zone0/temp");
    double milli = 0.0;
    if (f >> milli)
        return milli / 1000.0;
#endif
    return 0.0;
}

} // namespace sim

