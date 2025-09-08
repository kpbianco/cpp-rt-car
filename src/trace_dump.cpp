#include "simcore/bintrace.hpp"
#include "tools/trace_export.hpp"
#include <fstream>

int main() {
    bintrace::Trace tr;
    tr.init(1, 1024, true);
    tr.bindThread(0);
    tr.log(bintrace::EV_PhaseBegin);
    tr.log(bintrace::EV_PhaseEnd);
    auto snap = tr.snapshot();
    std::ofstream f("trace.json");
    trace_export::write_chrome_trace(snap, f);
    return 0;
}
