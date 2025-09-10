#pragma once
#include <vector>

namespace simphys {

struct Constraint {
    double lambda{0.0};
    double cachedLambda{0.0}; // warm-start cache
};

struct SolverSettings {
    double erp{0.2};
    double baumgarte{0.0};
    int iterations{10};
    bool warmStart{false};
    bool splitImpulse{false};
    bool shockPropagation{true}; // false -> block GS
};

class ConstraintSolver {
public:
    virtual ~ConstraintSolver() = default;

    void solve(std::vector<Constraint> &c, const SolverSettings &s) {
        if (s.warmStart)
            for (auto &con : c)
                con.lambda = con.cachedLambda;
        doSolve(c, s);
        if (s.warmStart)
            for (auto &con : c)
                con.cachedLambda = con.lambda;
    }

protected:
    virtual void doSolve(std::vector<Constraint> &c, const SolverSettings &s) = 0;
};

class PGSSolver : public ConstraintSolver {
protected:
    void doSolve(std::vector<Constraint> &c, const SolverSettings &s) override {
        for (int i = 0; i < s.iterations; ++i)
            for (auto &con : c)
                con.lambda += 1.0; // placeholder Gauss-Seidel step
        (void)s.splitImpulse;
        (void)s.shockPropagation;
    }
};

class XPBDSolver : public ConstraintSolver {
protected:
    void doSolve(std::vector<Constraint> &c, const SolverSettings &s) override {
        for (int i = 0; i < s.iterations; ++i)
            for (auto &con : c)
                con.lambda += 1.0; // placeholder XPBD step
        (void)s.splitImpulse;
    }
};

struct IslandState {
    bool sleeping{false};
    double idleTime{0.0};
};

class IslandManager {
public:
    double sleepThreshold{1.0};
    void update(IslandState &island, double dt, bool active) {
        if (active) {
            island.idleTime = 0.0;
            island.sleeping = false;
        } else {
            island.idleTime += dt;
            if (island.idleTime >= sleepThreshold)
                island.sleeping = true;
        }
    }
};

} // namespace simphys

