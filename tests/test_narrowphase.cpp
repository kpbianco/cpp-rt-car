#include <gtest/gtest.h>
#include <simcore/physics/collision_pipeline.hpp>

using namespace simphys;

struct DummyBroad : BroadPhase { void computePairs() override { ++called; } int called{0}; };
struct DummyNarrow : GJKEPANarrowPhase {
    int calls{0};
    void generateContacts() override {
        ++calls;
        GJKEPANarrowPhase::generateContacts();
    }
};
struct DummyCCD : ConservativeAdvancementCCD { int calls{0}; void sweep() override { ++calls; } };

TEST(NarrowPhase, ContactCaching) {
    ContactCache cache;
    DummyNarrow narrow; narrow.cache = &cache;
    narrow.generateContacts();
    ASSERT_EQ(cache.contacts.size(), 1u);
    int first = cache.contacts[0].id;
    narrow.generateContacts();
    EXPECT_EQ(cache.contacts[0].id, first); // reused from cache
}

TEST(CollisionPipeline, CCDHook) {
    DummyBroad broad; DummyNarrow narrow; ContactCache cache; narrow.cache = &cache;
    DummyCCD ccd;
    CollisionPipeline pipe; pipe.broad = &broad; pipe.narrow = &narrow; pipe.ccd = true; pipe.ccdSolver = &ccd;
    pipe.step();
    EXPECT_EQ(broad.called, 1);
    EXPECT_EQ(narrow.calls, 1);
    EXPECT_EQ(ccd.calls, 1);
}
