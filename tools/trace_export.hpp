#pragma once
#include <ostream>
#include <cstdint>
#include "simcore/bintrace.hpp"

namespace trace_export {

inline const char* codeToCat(std::uint32_t code) {
    switch (code) {
    case bintrace::EV_PhaseBegin:  return "PhaseBegin";
    case bintrace::EV_PhaseEnd:    return "PhaseEnd";
    case bintrace::EV_ChunkStart:  return "ChunkStart";
    case bintrace::EV_ChunkDone:   return "ChunkDone";
    case bintrace::EV_BudgetLadder:return "BudgetLadder";
    default: return "Unknown";
    }
}

inline void write_chrome_trace(const bintrace::Trace::Snapshot& snap, std::ostream& os) {
    os << "{\"traceEvents\":[";
    bool first = true;
    for (const auto& e : snap.events) {
        if (!first) os << ',';
        first = false;
        os << "{\"cat\":\"" << codeToCat(e.code) << "\",";
        os << "\"tid\":" << e.thread << ',';
        os << "\"ts\":" << e.tsc << "}";
    }
    os << "]}";
}

} // namespace trace_export

