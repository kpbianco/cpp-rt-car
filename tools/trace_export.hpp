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
    case bintrace::EV_QueuePush:          return "queue";
    case bintrace::EV_QueuePop:           return "queue";
    case bintrace::EV_WorkSteal:          return "WorkSteal";
    case bintrace::EV_EmergencySpawn:     return "WorkerPool";
    case bintrace::EV_PriorityEnqueue:    return "WorkerPool";
    case bintrace::EV_WorkerMeta:         return "WorkerPool";
    case bintrace::EV_WatchdogTrip:       return "WatchdogTrip";
    case bintrace::EV_GpuFenceWaitBegin:  return "GpuFenceWaitBegin";
    case bintrace::EV_GpuFenceWaitEnd:    return "GpuFenceWaitEnd";
    case bintrace::EV_SnapshotSave:       return "SnapshotSave";
    case bintrace::EV_SnapshotLoad:       return "SnapshotLoad";
    case bintrace::EV_PlatformCrumb:      return "PlatformCrumb";
    default: return "Unknown";
    }
}

inline bool decodeEmergencyRateAllowed(std::uint64_t payload) {
    return (payload >> 63) != 0;
}

inline std::uint32_t decodeEmergencyPriority(std::uint64_t payload) {
    return static_cast<std::uint32_t>(payload & 0xFFFFFFFFu);
}

inline std::uint32_t decodeEmergencyCategory(std::uint64_t payload) {
    return static_cast<std::uint32_t>((payload >> 32) & 0x7FFFFFFFu);
}

inline const char* codeToName(std::uint32_t code) {
    switch (code) {
    case bintrace::EV_PhaseBegin:         return "Phase Begin";
    case bintrace::EV_PhaseEnd:           return "Phase End";
    case bintrace::EV_ChunkStart:         return "Chunk Start";
    case bintrace::EV_ChunkDone:          return "Chunk Done";
    case bintrace::EV_GovernorRung:       return "Governor Rung";
    case bintrace::EV_QueuePush:          return "queue_push";
    case bintrace::EV_QueuePop:           return "queue_pop";
    case bintrace::EV_WorkSteal:          return "Work Steal";
    case bintrace::EV_EmergencySpawn:     return "Emergency Spawn";
    case bintrace::EV_PriorityEnqueue:    return "Priority Enqueue";
    case bintrace::EV_WorkerMeta:         return "Worker Meta";
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
        if (e.code == bintrace::EV_WorkerMeta) {
            if (!first) os << ',';
            first = false;
            const std::int32_t node = bintrace::decode_worker_meta_node(e.b);
            os << "{\"cat\":\"" << codeToCat(e.code) << "\",";
            os << "\"name\":\"thread_name\",";
            os << "\"ph\":\"M\",";
            os << "\"tid\":" << e.thread << ',';
            os << "\"ts\":0,\"args\":{\"name\":\"Worker " << e.a << "\"}}";
            os << ',';
            os << "{\"cat\":\"" << codeToCat(e.code) << "\",";
            os << "\"name\":\"worker_meta\",";
            os << "\"ph\":\"M\",";
            os << "\"tid\":" << e.thread << ',';
            os << "\"ts\":0,\"args\":{\"worker_index\":" << e.a
               << ",\"numa_node\":" << node << "}}";
            continue;
        }
        if (!first) os << ',';
        first = false;
        os << "{\"cat\":\"" << codeToCat(e.code) << "\",";
        os << "\"name\":\"" << codeToName(e.code) << "\",";
        os << "\"ph\":\"i\",";
        os << "\"tid\":" << e.thread << ',';
        os << "\"ts\":" << e.tsc << ',';
        os << "\"args\":";
        switch (e.code) {
        case bintrace::EV_QueuePush:
        case bintrace::EV_QueuePop:
            os << "{\"depth\":" << e.a;
            if (e.b) {
                os << ",\"capacity\":" << e.b;
            }
            os << "}";
            break;
        case bintrace::EV_EmergencySpawn:
            os << "{\"outstanding\":" << e.a
               << ",\"priority\":" << decodeEmergencyPriority(e.b)
               << ",\"category\":" << decodeEmergencyCategory(e.b)
               << ",\"rate_allowed\":"
               << (decodeEmergencyRateAllowed(e.b) ? "true" : "false") << "}";
            break;
        case bintrace::EV_PriorityEnqueue:
            os << "{\"outstanding\":" << e.a
               << ",\"priority\":" << decodeEmergencyPriority(e.b)
               << ",\"category\":" << decodeEmergencyCategory(e.b)
               << "}";
            break;
        default:
            os << "{\"a\":" << e.a << ",\"b\":" << e.b << "}";
            break;
        }
        os << "}";
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

