#pragma once
#include <rtfw/benchmark.hpp>
#include <stdexcept>
namespace benchmark_test {
namespace b=rtfw::benchmark;
struct Fixture {
    b::Descriptor descriptor;
    std::uint64_t calls{}, ticks{}, clock_calls{};
    bool throw_describe{}, throw_invoke{}, wrong{}, unavailable{}, backward{}, throw_clock{};
    b::Runner* reenter{};
    Fixture() {
        descriptor.case_id="case"; descriptor.subsystem="framework";
        descriptor.implementation="fixture-v1"; descriptor.configuration="fixed";
        descriptor.workload_kind="counter"; descriptor.workload_sha256=b::sha256("fixture");
        descriptor.counters={{"operations","count",1,1},{"drops","count",0,0}};
    }
    b::ProviderV1 table(const char* name="fixture") {
        b::ProviderV1 p; p.id=name; p.case_count=1; p.user=this;
        p.describe=[](void* u,std::size_t,b::Descriptor& out) {
            auto& f=*static_cast<Fixture*>(u);
            if (f.throw_describe) throw std::runtime_error("secret descriptor exception");
            out=f.descriptor; return b::Status::ok;
        };
        p.invoke=[](void* u,std::string_view,std::uint64_t n,b::Observation& out) {
            auto& f=*static_cast<Fixture*>(u);
            ++f.calls;
            if (f.throw_invoke) throw std::runtime_error("secret invocation exception");
            if (f.unavailable) return b::Status::not_run;
            if (f.reenter) {
                b::ProviderHandle handle;
                if (f.reenter->register_provider(f.table("nested"),handle)!=b::Status::busy)
                    return b::Status::provider_error;
            }
            out.counters={f.wrong?2U:1U,0}; out.checksum=n;
            return b::Status::ok;
        };
        return p;
    }
    b::ClockV1 clock() {
        b::ClockV1 c; c.kind=b::ClockKind::fake; c.user=this;
        c.read_ns=[](void* u,std::uint64_t& out) {
            auto& f=*static_cast<Fixture*>(u); ++f.clock_calls;
            if (f.throw_clock) throw std::runtime_error("secret clock exception");
            out=f.backward && f.clock_calls>1 ? 0 : f.ticks;
            f.ticks+=10; return true;
        };
        return c;
    }
};
inline b::Result result(Fixture& f) {
    b::Runner runner; b::ProviderHandle h;
    if (runner.register_provider(f.table(),h)!=b::Status::ok) throw std::runtime_error("fixture registration");
    return runner.run("fixture",f.descriptor.case_id,f.clock(),b::Identity{});
}
}
