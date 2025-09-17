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
    case bintrace::EV_GovernorRung:return "GovernorRung";
    case bintrace::EV_QueuePush:   return "QueuePush";
    case bintrace::EV_QueuePop:    return "QueuePop";
    case bintrace::EV_WorkSteal:   return "WorkSteal";
    case bintrace::EV_WatchdogTrip:return "WatchdogTrip";
    case bintrace::EV_GpuFenceWaitBegin: return "GpuFenceWaitBegin";
    case bintrace::EV_GpuFenceWaitEnd:   return "GpuFenceWaitEnd";
    case bintrace::EV_SnapshotSave:      return "SnapshotSave";
    case bintrace::EV_SnapshotLoad:      return "SnapshotLoad";
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

inline void write_etw_trace(const bintrace::Trace::Snapshot& snap, std::ostream& os) {
    os << "tsc,thread,code,a,b\n";
    for (const auto& e : snap.events) {
        os << e.tsc << ',' << e.thread << ',' << e.code << ','
           << e.a << ',' << e.b << '\n';
    }
}

inline void write_ebpf_trace(const bintrace::Trace::Snapshot& snap, std::ostream& os) {
    for (const auto& e : snap.events) {
        os << e.tsc << ' ' << e.thread << ' ' << e.code << ' '
           << e.a << ' ' << e.b << '\n';
    }
}

} // namespace trace_export

