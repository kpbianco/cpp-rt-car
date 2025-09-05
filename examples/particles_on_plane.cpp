#include <simcore/physics/integrators.hpp>
#include <iostream>

struct Vec2 {
    double x{}, y{};
    Vec2 operator+(const Vec2 &o) const { return {x + o.x, y + o.y}; }
    Vec2 &operator+=(const Vec2 &o) { x += o.x; y += o.y; return *this; }
    Vec2 operator*(double s) const { return {x * s, y * s}; }
};

struct Particle {
    Vec2 pos{}; // metres
    Vec2 vel{}; // metres/second
};

int main() {
    using simphys::symplecticEuler;

    Particle p{{0.0, 10.0}, {1.0, 0.0}}; // start 10m up, moving sideways
    auto gravity = [](const Particle &) { return Vec2{0.0, -9.81}; };
    double dt = 0.016; // 60 Hz

    for (int step = 0; step < 60; ++step) {
        symplecticEuler(p, dt, gravity);
        std::cout << "t=" << (step + 1) * dt
                  << "s pos=(" << p.pos.x << ", " << p.pos.y << ")\n";
    }
}
