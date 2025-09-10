#pragma once
#include <vector>

namespace simphys {

struct Contact {
    int id{0};
};

class ContactCache {
public:
    std::vector<Contact> contacts;
};

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

class GJKEPANarrowPhase : public NarrowPhase {
public:
    ContactCache *cache{nullptr};
    void generateContacts() override {
        Contact c{1};
        if (cache && !cache->contacts.empty())
            c = cache->contacts.front();
        if (cache)
            cache->contacts = {c};
    }
};

class MPRNarrowPhase : public NarrowPhase {
public:
    ContactCache *cache{nullptr};
    void generateContacts() override {
        Contact c{2};
        if (cache && !cache->contacts.empty())
            c = cache->contacts.front();
        if (cache)
            cache->contacts = {c};
    }
};

class CCDSolver {
public:
    virtual ~CCDSolver() = default;
    virtual void sweep() = 0;
};

class ConservativeAdvancementCCD : public CCDSolver {
public:
    void sweep() override {}
};

class CollisionPipeline {
public:
    BroadPhase *broad{nullptr};
    NarrowPhase *narrow{nullptr};
    bool ccd{false};
    CCDSolver *ccdSolver{nullptr};

    void step() {
        if (broad)
            broad->computePairs();
        if (narrow)
            narrow->generateContacts();
        if (ccd && ccdSolver)
            ccdSolver->sweep();
    }
};

} // namespace simphys

