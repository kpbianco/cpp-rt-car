#include <rtfw/benchmark.hpp>
#include <string_view>
namespace b=rtfw::benchmark;
struct Provider {
    std::uint64_t calls{},tick{};
    static b::Status describe(void*,std::size_t n,b::Descriptor& out) {
        if (n) return b::Status::not_found;
        out.case_id="count"; out.subsystem="external"; out.implementation="fixture-v1";
        out.configuration="fixed"; out.workload_kind="counter";
        out.workload_sha256=b::sha256("external.count.v1");
        out.counters={{"operations","count",1,1}};
        return b::Status::ok;
    }
    static b::Status invoke(void* user,std::string_view id,std::uint64_t ordinal,b::Observation& out) {
        if (id!="count") return b::Status::not_found;
        auto& self=*static_cast<Provider*>(user);
        ++self.calls; out.counters={1}; out.checksum=ordinal;
        return b::Status::ok;
    }
    static bool clock(void* user,std::uint64_t& out) {
        auto& self=*static_cast<Provider*>(user); out=self.tick; self.tick+=10; return true;
    }
};
int main(int argc,char** argv) {
    try {
        if (argc>2) return 2;
        Provider p; b::ProviderV1 table;
        table.id="external.fixture"; table.user=&p; table.case_count=1;
        table.describe=Provider::describe; table.invoke=Provider::invoke;
        b::Runner runner; b::ProviderHandle handle;
        if (runner.register_provider(table,handle)!=b::Status::ok) return 3;
        b::ClockV1 clock; clock.kind=b::ClockKind::fake; clock.user=&p; clock.read_ns=Provider::clock;
        const auto result=runner.run("external.fixture","count",clock,b::Identity{});
        if (result.status!=b::Status::ok || result.samples.size()!=5 || p.calls!=7) return 4;
        if (b::encode(result).raw.empty()) return 5;
        if (argc==2 && b::publish(result,argv[1])!=b::Status::ok) return 6;
        if (runner.unregister_provider(handle)!=b::Status::ok || !runner.list().empty()) return 7;
        return 0;
    } catch (...) { return 8; }
}
