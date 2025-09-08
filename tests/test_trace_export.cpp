#include <gtest/gtest.h>
#include "simcore/bintrace.hpp"
#include "tools/trace_export.hpp"
#include <sstream>
#include <fstream>
#include <iterator>
#include <string>

TEST(TraceExport, GoldenJson)
{
    bintrace::Trace::Snapshot snap;
    snap.events = {
        bintrace::Event{100, bintrace::EV_PhaseBegin, 0, 0, 0, 0},
        bintrace::Event{150, bintrace::EV_PhaseEnd, 0, 0, 0, 0},
        bintrace::Event{200, bintrace::EV_ChunkStart, 0, 0, 1, 0},
        bintrace::Event{300, bintrace::EV_ChunkDone, 0, 0, 1, 0}
    };
    snap.perThreadCount = {2,2};

    std::ostringstream os;
    trace_export::write_chrome_trace(snap, os);
    std::string out = os.str();

    std::ifstream f(std::string(PROJECT_SOURCE_DIR) + "/tests/golden/trace_sample.json");
    std::string golden((std::istreambuf_iterator<char>(f)), {});
    EXPECT_EQ(out, golden);

    std::size_t bin_size = snap.events.size() * sizeof(bintrace::Event);
    EXPECT_LT(out.size(), bin_size * 10);
}
