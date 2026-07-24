#include <rt/observability_export.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <ostream>
#include <string_view>
#include <vector>

namespace {

template <std::size_t Capacity>
std::string_view identifier_view(
    const std::array<char, Capacity>& identifier) noexcept {
    const auto end = std::find(
        identifier.begin(),
        identifier.end(),
        '\0');
    return {
        identifier.data(),
        static_cast<std::size_t>(end - identifier.begin()),
    };
}

void write_json_string(
    std::ostream& output,
    std::string_view value) {
    output.put('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (byte < 0x20u) {
                output << "\\u00"
                       << hex[(byte >> 4u) & 0x0fu]
                       << hex[byte & 0x0fu];
            } else {
                output.put(static_cast<char>(byte));
            }
            break;
        }
    }
    output.put('"');
}

} // namespace

namespace rt {

Status write_observability_json(
    Runtime& runtime,
    std::ostream& output,
    RuntimeMetricWindow window,
    RuntimeMetricCursor* metric_cursor,
    RuntimeTraceCursor* trace_cursor) noexcept {
    try {
        if (window == RuntimeMetricWindow::interval &&
            metric_cursor == nullptr) {
            return Status::invalid_argument;
        }

        ObservabilityMetadata metadata;
        auto status =
            runtime.observability_metadata(metadata);
        if (status != Status::ok) {
            return status;
        }

        RuntimeMetricCursor metric_candidate =
            metric_cursor
            ? *metric_cursor
            : RuntimeMetricCursor{};
        RuntimeMetricSnapshot metrics;
        status = runtime.metrics_snapshot(
            window,
            window == RuntimeMetricWindow::interval
                ? &metric_candidate
                : nullptr,
            metrics);
        if (status != Status::ok) {
            return status;
        }

        RuntimeTraceCursor trace_candidate =
            trace_cursor
            ? *trace_cursor
            : RuntimeTraceCursor{};
        std::vector<RuntimeTraceEvent> events(
            static_cast<std::size_t>(
                metadata.trace_capacity));
        RuntimeTraceReadResult trace_result;
        status = runtime.read_trace(
            trace_candidate,
            events,
            trace_result);
        if (status != Status::ok) {
            return status;
        }

        output << "{\"schema_version\":"
               << metadata.schema_version
               << ",\"runtime_version\":{\"major\":"
               << metadata.runtime_version_major
               << ",\"minor\":"
               << metadata.runtime_version_minor
               << ",\"patch\":"
               << metadata.runtime_version_patch
               << "},\"trace_event_size\":"
               << metadata.trace_event_size
               << ",\"metric_sample_size\":"
               << metadata.metric_sample_size
               << ",\"metric_count\":"
               << metadata.metric_count
               << ",\"trace_capacity\":"
               << metadata.trace_capacity
               << ",\"build_id\":";
        write_json_string(
            output,
            identifier_view(metadata.build_id));
        output << ",\"config_id\":"
               << metadata.config_id
               << ",\"runtime_id\":"
               << metadata.runtime_id
               << ",\"workload_id\":";
        write_json_string(
            output,
            identifier_view(metadata.workload_id));
        output << ",\"metrics\":{\"window\":";
        write_json_string(
            output,
            window == RuntimeMetricWindow::interval
                ? "interval"
                : "cumulative");
        output << ",\"sequence\":"
               << metrics.snapshot_sequence
               << ",\"start_ns\":"
               << metrics.window_start_ns
               << ",\"end_ns\":"
               << metrics.window_end_ns
               << ",\"samples\":[";

        for (std::size_t index = 0;
             index < metrics.sample_count;
             ++index) {
            RuntimeMetricDefinition definition;
            if (!runtime_metric_definition(
                    index,
                    definition)) {
                return Status::internal_error;
            }
            if (index != 0) {
                output.put(',');
            }
            output << "{\"id\":"
                   << static_cast<std::uint16_t>(
                          definition.id)
                   << ",\"name\":";
            write_json_string(output, definition.name);
            output << ",\"kind\":";
            write_json_string(
                output,
                definition.kind ==
                        RuntimeMetricKind::gauge
                    ? "gauge"
                    : "counter");
            output << ",\"value\":"
                   << metrics.samples[index].value
                   << '}';
        }

        output << "]},\"trace\":{\"first_sequence\":"
               << trace_result.first_sequence
               << ",\"next_sequence\":"
               << trace_result.next_sequence
               << ",\"lost_events\":"
               << trace_result.lost_events
               << ",\"events\":[";
        for (std::size_t index = 0;
             index < trace_result.events_read;
             ++index) {
            const auto& event = events[index];
            if (index != 0) {
                output.put(',');
            }
            output << "{\"sequence\":"
                   << event.sequence
                   << ",\"type\":"
                   << static_cast<std::uint16_t>(event.type)
                   << ",\"name\":";
            write_json_string(
                output,
                runtime_trace_event_name(event.type));
            output << ",\"status\":"
                   << static_cast<std::int32_t>(event.status)
                   << ",\"timestamp_ns\":"
                   << event.timestamp_ns
                   << ",\"frame_index\":"
                   << event.frame_index
                   << ",\"producer\":";
            write_json_string(
                output,
                event.producer ==
                        RuntimeTraceProducer::worker
                    ? "worker"
                    : "host");
            output << ",\"callback_index\":"
                   << event.callback_index
                   << ",\"worker_index\":"
                   << event.worker_index
                   << ",\"value\":"
                   << event.value
                   << '}';
        }
        output << "]}}\n";
        if (!output) {
            return Status::internal_error;
        }

        if (window == RuntimeMetricWindow::interval &&
            metric_cursor) {
            *metric_cursor = metric_candidate;
        }
        if (trace_cursor) {
            *trace_cursor = trace_candidate;
        }
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::resource_exhausted;
    } catch (...) {
        return Status::internal_error;
    }
}

} // namespace rt
