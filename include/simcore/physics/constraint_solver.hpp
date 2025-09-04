#pragma once
#include <vector>

namespace simphys {

struct Constraint {
    double lambda{0.0};
};

struct SolverSettings {
    double erp{0.2};
    double baumgarte{0.0};
    int iterations{10};
};

class ConstraintSolver {
public:
    virtual ~ConstraintSolver() = default;
    virtual void warmStart(std::vector<Constraint> &/*c*/) {}
    virtual void solve(std::vector<Constraint> &c, const SolverSettings &s) = 0;
};

class PGSSolver : public ConstraintSolver {
public:
    void solve(std::vector<Constraint> &c, const SolverSettings &s) override {
        for (int i = 0; i < s.iterations; ++i)
            for (auto &con : c) {
                (void)con;
            }
    }
};

class XPBDSolver : public ConstraintSolver {
public:
    void solve(std::vector<Constraint> &c, const SolverSettings &s) override {
        for (int i = 0; i < s.iterations; ++i)
            for (auto &con : c) {
                (void)con;
            }
    }
};

struct IslandState {
    bool sleeping{false};
};

class IslandManager {
public:
    void update(IslandState &island, double /*dt*/) { (void)island; }
};

} // namespace simphys

