#pragma once
#include <ostream>
#include <cstdint>
#include "simcore/bintrace.hpp"

namespace trace_export {

inline const char* codeToCat(std::uint32_t code) {
    switch (code) {
    case bintrace::EV_PhaseBegin:         return "PhaseBegin";
    case bintrace::EV_PhaseEnd:           return "PhaseEnd";
    case bintrace::EV_ChunkStart:         return "ChunkStart";
    case bintrace::EV_ChunkDone:          return "ChunkDone";
    case bintrace::EV_GovernorRung:       return "GovernorRung";
    case bintrace::EV_QueuePush:
    case bintrace::EV_QueuePop:           return "queue";
    case bintrace::EV_WorkSteal:          return "WorkSteal";
    case bintrace::EV_WatchdogTrip:       return "WatchdogTrip";
    case bintrace::EV_GpuFenceWaitBegin:  return "GpuFenceWaitBegin";
    case bintrace::EV_GpuFenceWaitEnd:    return "GpuFenceWaitEnd";
    case bintrace::EV_SnapshotSave:       return "SnapshotSave";
    case bintrace::EV_SnapshotLoad:       return "SnapshotLoad";
    case bintrace::EV_PlatformCrumb:      return "PlatformCrumb";
    default: return "Unknown";
    }
}

inline const char* codeToName(std::uint32_t code) {
    switch (code) {
    case bintrace::EV_PhaseBegin:         return "Phase Begin";
    case bintrace::EV_PhaseEnd:           return "Phase End";
    case bintrace::EV_ChunkStart:         return "Chunk Start";
    case bintrace::EV_ChunkDone:          return "Chunk Done";
    case bintrace::EV_GovernorRung:       return "Governor Rung";
    case bintrace::EV_QueuePush:          return "Queue Push";
    case bintrace::EV_QueuePop:           return "Queue Pop";
    case bintrace::EV_WorkSteal:          return "Work Steal";
    case bintrace::EV_WatchdogTrip:       return "Watchdog Trip";
    case bintrace::EV_GpuFenceWaitBegin:  return "GPU Fence Wait Begin";
    case bintrace::EV_GpuFenceWaitEnd:    return "GPU Fence Wait End";
    case bintrace::EV_SnapshotSave:       return "Snapshot Save";
    case bintrace::EV_SnapshotLoad:       return "Snapshot Load";
    case bintrace::EV_PlatformCrumb:      return "Platform Crumb";
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
        os << "\"name\":\"" << codeToName(e.code) << "\",";
        os << "\"ph\":\"i\",";
        os << "\"tid\":" << e.thread << ',';
        os << "\"ts\":" << e.tsc << ',';
        os << "\"args\":{\"a\":" << e.a << ",\"b\":" << e.b << "}}";
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

