#include <gtest/gtest.h>
#include <simcore/physics/integrators.hpp>
#include <vector>
#include <cmath>

using namespace simphys;

struct OscState {
    double pos;
    double vel;
};

static double accel(const OscState &s) { return -s.pos; }

static double energy(const OscState &s) {
    return 0.5 * s.pos * s.pos + 0.5 * s.vel * s.vel;
}

TEST(PhysicsIntegrators, EnergyInvariant) {
    OscState a{1.0, 0.0}, b{1.0, 0.0}, c{1.0, 0.0}, d{1.0, 0.0};
    double e0 = energy(a);
    for (int i = 0; i < 1000; ++i) symplecticEuler(a, 0.01, accel);
    for (int i = 0; i < 1000; ++i) semiImplicitRK2(b, 0.01, accel);
    for (int i = 0; i < 1000; ++i) rk4(c, 0.01, accel);
    for (int i = 0; i < 1000; ++i) stormerVerlet(d, 0.01, accel);
    EXPECT_NEAR(energy(a), e0, e0 * 0.1);
    EXPECT_NEAR(energy(b), e0, e0 * 0.1);
    EXPECT_NEAR(energy(c), e0, e0 * 0.1);
    EXPECT_NEAR(energy(d), e0, e0 * 0.1);
}

TEST(PhysicsIntegrators, StabilityDtSweep) {
    const std::vector<double> dts = {0.005, 0.01, 0.02, 0.04};
    for (double dt : dts) {
        int steps = static_cast<int>(1.0 / dt);
        OscState a{1.0, 0.0}, b{1.0, 0.0}, c{1.0, 0.0}, d{1.0, 0.0};
        for (int i = 0; i < steps; ++i) symplecticEuler(a, dt, accel);
        for (int i = 0; i < steps; ++i) semiImplicitRK2(b, dt, accel);
        for (int i = 0; i < steps; ++i) rk4(c, dt, accel);
        for (int i = 0; i < steps; ++i) stormerVerlet(d, dt, accel);
        double e0 = 0.5; // initial energy
        EXPECT_LT(energy(a), e0 * 4.0);
        EXPECT_LT(energy(b), e0 * 4.0);
        EXPECT_LT(energy(c), e0 * 4.0);
        EXPECT_LT(energy(d), e0 * 4.0);
    }
}

