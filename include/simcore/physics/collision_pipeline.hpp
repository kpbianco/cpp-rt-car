#pragma once

namespace simphys {

class BroadPhase {
public:
    virtual ~BroadPhase() = default;
    virtual void computePairs() = 0;
};

class SweepAndPrune : public BroadPhase {
public:
    void computePairs() override {}
};

class BVHBroadPhase : public BroadPhase {
public:
    void computePairs() override {}
};

class NarrowPhase {
public:
    virtual ~NarrowPhase() = default;
    virtual void generateContacts() = 0;
};

class CollisionPipeline {
public:
    BroadPhase *broad{nullptr};
    NarrowPhase *narrow{nullptr};
    bool ccd{false};

    void step() {
        if (broad)
            broad->computePairs();
        if (narrow)
            narrow->generateContacts();
    }
};

} // namespace simphys

