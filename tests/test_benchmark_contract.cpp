#include "benchmark_fixtures/provider.hpp"
#include <gtest/gtest.h>
using namespace benchmark_test;

TEST(BenchmarkContract, HashVectors) {
    EXPECT_EQ(b::sha256(std::string_view{}),b::sha256(""));
    EXPECT_EQ(b::sha256(""),"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(b::sha256("abc"),"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(b::sha256(std::string(1000000,'a')),"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}
TEST(BenchmarkContract, RejectsMalformedBeforeInvocation) {
    Fixture original;
    std::vector<b::Descriptor> invalid;
    auto mutate=[&](auto fn) { auto d=original.descriptor; fn(d); invalid.push_back(d); };
    mutate([](auto& d){d.version=2;}); mutate([](auto& d){d.case_id="../bad";});
    mutate([](auto& d){d.case_id=std::string(1,static_cast<char>(0x80));});
    mutate([](auto& d){d.implementation="\xc3\xa9";});
    mutate([](auto& d){d.subsystem="";}); mutate([](auto& d){d.configuration="password=abc";});
    mutate([](auto& d){d.implementation=std::string(65,'x');});
    mutate([](auto& d){d.workload_kind="x\ny";}); mutate([](auto& d){d.workload_sha256="bad";});
    mutate([](auto& d){d.repetitions=0;}); mutate([](auto& d){d.repetitions=b::max_repetitions+1;});
    mutate([](auto& d){d.warmup=b::max_warmup+1;}); mutate([](auto& d){d.retain_raw=false;});
    mutate([](auto& d){d.counters.clear();}); mutate([](auto& d){d.counters.push_back(d.counters[0]);});
    mutate([](auto& d){d.counters[0].minimum=2;});
    mutate([](auto& d){d.parameters={{"x",2,0,1}};});
    mutate([](auto& d){d.parameters={{"x",0,0,1},{"x",0,0,1}};});
    mutate([](auto& d){d.counters[0].maximum=UINT64_MAX;});
    for (const auto& d:invalid) {
        Fixture f; f.descriptor=d; b::Runner runner; b::ProviderHandle h;
        EXPECT_NE(runner.register_provider(f.table(),h),b::Status::ok);
        EXPECT_EQ(f.calls,0U); EXPECT_TRUE(runner.list().empty()); EXPECT_EQ(h.owner,nullptr);
    }
}
TEST(BenchmarkContract, TransactionalTableAndDescriptorExceptions) {
    Fixture f; b::Runner r; b::ProviderHandle h;
    auto p=f.table(); p.size=0; EXPECT_EQ(r.register_provider(p,h),b::Status::invalid);
    p=f.table(); p.reserved[1]=1; EXPECT_EQ(r.register_provider(p,h),b::Status::invalid);
    p=f.table(); p.invoke=nullptr; EXPECT_EQ(r.register_provider(p,h),b::Status::invalid);
    p=f.table(); p.case_count=b::max_cases+1; EXPECT_EQ(r.register_provider(p,h),b::Status::invalid);
    f.throw_describe=true; EXPECT_EQ(r.register_provider(f.table(),h),b::Status::provider_error);
    EXPECT_TRUE(r.list().empty()); f.throw_describe=false;
    ASSERT_EQ(r.register_provider(f.table(),h),b::Status::ok);
    f.descriptor.case_id="changed"; EXPECT_EQ(r.list(),std::vector<std::string>{"fixture:case"});
}
TEST(BenchmarkContract, StableDiscoveryAndStaleHandles) {
    Fixture f,g; b::Runner first,second; b::ProviderHandle a,z,other;
    ASSERT_EQ(first.register_provider(f.table("z"),z),b::Status::ok);
    ASSERT_EQ(first.register_provider(g.table("a"),a),b::Status::ok);
    EXPECT_EQ(first.list(),(std::vector<std::string>{"a:case","z:case"}));
    EXPECT_EQ(first.register_provider(f.table("z"),other),b::Status::duplicate);
    EXPECT_EQ(second.unregister_provider(z),b::Status::stale);
    ASSERT_EQ(first.unregister_provider(z),b::Status::ok);
    EXPECT_EQ(first.unregister_provider(z),b::Status::stale);
    ASSERT_EQ(first.register_provider(f.table("z"),other),b::Status::ok);
    EXPECT_NE(z.generation,other.generation);
}
TEST(BenchmarkContract, DuplicateCasesAndCapacity) {
    Fixture f; b::Runner r; b::ProviderHandle h;
    auto p=f.table(); p.case_count=2;
    EXPECT_EQ(r.register_provider(p,h),b::Status::duplicate);
    for (std::size_t n=0;n<b::max_providers;++n) {
        const auto name="provider"+std::to_string(n);
        EXPECT_EQ(r.register_provider(f.table(name.c_str()),h),b::Status::ok);
    }
    EXPECT_EQ(r.register_provider(f.table("overflow"),h),b::Status::capacity);
}
TEST(BenchmarkContract, IdentityRejectsPathsSecretsAndUnsupportedNumbers) {
    b::Identity i;
    EXPECT_EQ(b::validate(i),b::Status::ok);
    i.cpu_model="/home/user"; EXPECT_EQ(b::validate(i),b::Status::invalid);
    i={}; i.host_label="token=secret"; EXPECT_EQ(b::validate(i),b::Status::invalid);
    i={}; i.os="x\ny"; EXPECT_EQ(b::validate(i),b::Status::invalid);
    i={}; i.source_commit="abc"; EXPECT_EQ(b::validate(i),b::Status::invalid);
    i={}; i.source_dirty="clean"; EXPECT_EQ(b::validate(i),b::Status::invalid);
    i={}; i.total_memory_bytes=UINT64_MAX; EXPECT_EQ(b::validate(i),b::Status::invalid);
    EXPECT_EQ(b::validate(b::capture_identity()),b::Status::ok);
}
