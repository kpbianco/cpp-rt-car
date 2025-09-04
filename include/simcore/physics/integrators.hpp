#pragma once
#include <algorithm>
#include <cmath>

namespace simphys {

// Generic state integration helpers. State is expected to expose
// 'pos' and 'vel' members supporting basic arithmetic with doubles.

template <class State, class AccelFn>
void symplecticEuler(State &s, double dt, AccelFn &&accel) {
    s.vel += accel(s) * dt;
    s.pos += s.vel * dt;
}

template <class State, class AccelFn>
void semiImplicitRK2(State &s, double dt, AccelFn &&accel) {
    auto a1 = accel(s);
    auto vHalf = s.vel + a1 * (dt * 0.5);
    State mid{s};
    mid.pos += vHalf * (dt * 0.5);
    mid.vel = vHalf;
    auto a2 = accel(mid);
    s.vel += a2 * dt;
    s.pos += vHalf * dt;
}

template <class State, class AccelFn>
void rk4(State &s, double dt, AccelFn &&accel) {
    auto a1 = accel(s);
    auto v2 = s.vel + a1 * (dt * 0.5);
    State s2{s};
    s2.pos += s.vel * (dt * 0.5);
    s2.vel = v2;
    auto a2 = accel(s2);

    auto v3 = s.vel + a2 * (dt * 0.5);
    State s3{s};
    s3.pos += v2 * (dt * 0.5);
    s3.vel = v3;
    auto a3 = accel(s3);

    auto v4 = s.vel + a3 * dt;
    State s4{s};
    s4.pos += v3 * dt;
    s4.vel = v4;
    auto a4 = accel(s4);

    s.pos += (s.vel + 2.0 * v2 + 2.0 * v3 + v4) * (dt / 6.0);
    s.vel += (a1 + 2.0 * a2 + 2.0 * a3 + a4) * (dt / 6.0);
}

struct CFLAdvisor {
    double maxVelocity{1.0};
    double maxAcceleration{1.0};
    double maxDistance{1.0};

    double operator()(double dt) const {
        double vStep = maxDistance / std::max(maxVelocity, 1e-9);
        double aStep = std::sqrt(maxDistance / std::max(maxAcceleration, 1e-9));
        return std::min({dt, vStep, aStep});
    }
};

struct IslandConfig {
    double cfl{1.0};
    bool sleeping{false};
};

class SubstepScheduler {
public:
    static int schedule(const IslandConfig &island, double dt) {
        if (island.sleeping)
            return 0;
        return std::max(1, static_cast<int>(std::ceil(dt / island.cfl)));
    }
};

} // namespace simphys

