#include <gtest/gtest.h>

#include <rt/runtime.hpp>
#include <rt/snapshot.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using CanonicalU64 = std::array<std::byte, sizeof(std::uint64_t)>;

std::uint64_t load_value(const CanonicalU64& bytes) {
    std::uint64_t value = 0;
    EXPECT_TRUE(rt::load_u64_le(bytes, 0, value));
    return value;
}

void store_value(CanonicalU64& bytes, std::uint64_t value) {
    ASSERT_TRUE(rt::store_u64_le(bytes, 0, value));
}

struct DeterministicWorkload {
    rt::Runtime runtime;
    CanonicalU64 input{};
    CanonicalU64 left{};
    CanonicalU64 right{};
    CanonicalU64 total{};

    static rt::CallbackResult update_left(
        void* opaque,
        const rt::CallbackContext& context) {
        auto& self =
            *static_cast<DeterministicWorkload*>(opaque);
        const auto value =
            load_value(self.left) * 17u +
            load_value(self.input) +
            context.frame.frame_index + 3u;
        store_value(self.left, value);
        return rt::CallbackResult::ok;
    }

    static rt::CallbackResult update_right(
        void* opaque,
        const rt::CallbackContext& context) {
        auto& self =
            *static_cast<DeterministicWorkload*>(opaque);
        const auto value =
            load_value(self.right) * 31u +
            (load_value(self.input) ^ 0xa5a5u) +
            context.frame.frame_index * 5u;
        store_value(self.right, value);
        return rt::CallbackResult::ok;
    }

    static rt::CallbackResult combine(
        void* opaque,
        const rt::CallbackContext& context) {
        auto& self =
            *static_cast<DeterministicWorkload*>(opaque);
        const auto value =
            load_value(self.left) +
            (load_value(self.right) << 1u) +
            context.frame.frame_index;
        store_value(self.total, value);
        return rt::CallbackResult::ok;
    }

    rt::Status prepare(
        std::size_t workers,
        rt::DeterminismTier tier =
            rt::DeterminismTier::schedule_independent,
        std::uint32_t state_schema_version = 1,
        const char* first_phase_name = "left") {
        rt::RuntimeConfig config;
        config.callback_capacity = 3;
        config.scratch_bytes = 64;
        config.trace_capacity = 128;
        config.executor_policy =
            rt::ExecutorPolicy::static_deterministic;
        config.worker_count = workers;
        config.executor_queue_capacity = 64;
        config.task_scratch_bytes = 64;
        config.task_scratch_slots = 64;
        config.memory_budget_bytes = 4 * 1024 * 1024;
        config.determinism_tier = tier;
        config.state_capacity = 4;
        config.snapshot_max_bytes = 4096;
        config.replay_input_capacity = 128;
        config.input_log_max_bytes = 16 * 1024;
        const auto config_status = runtime.configure(config);
        if (config_status != rt::Status::ok) {
            return config_status;
        }

        rt::PhaseHandle left_phase;
        rt::PhaseHandle right_phase;
        rt::PhaseHandle combine_phase;
        auto status = runtime.register_callback(
            {first_phase_name, &update_left, this},
            left_phase);
        if (status != rt::Status::ok) {
            return status;
        }
        status = runtime.register_callback(
            {"right", &update_right, this},
            right_phase);
        if (status != rt::Status::ok) {
            return status;
        }
        status = runtime.register_callback(
            {"combine", &combine, this},
            combine_phase);
        if (status != rt::Status::ok) {
            return status;
        }

        rt::ResourceHandle input_resource;
        rt::ResourceHandle left_resource;
        rt::ResourceHandle right_resource;
        rt::ResourceHandle total_resource;
        for (const auto& registration : {
                 std::pair{"input", &input_resource},
                 std::pair{"left", &left_resource},
                 std::pair{"right", &right_resource},
                 std::pair{"total", &total_resource},
             }) {
            status = runtime.register_resource(
                registration.first,
                *registration.second);
            if (status != rt::Status::ok) {
                return status;
            }
        }
        if ((status = runtime.add_dependency(
                 left_phase,
                 combine_phase)) != rt::Status::ok ||
            (status = runtime.add_dependency(
                 right_phase,
                 combine_phase)) != rt::Status::ok) {
            return status;
        }
        for (const auto& declaration : {
                 std::tuple{
                     left_phase,
                     input_resource,
                     rt::ResourceAccess::read},
                 std::tuple{
                     left_phase,
                     left_resource,
                     rt::ResourceAccess::write},
                 std::tuple{
                     right_phase,
                     input_resource,
                     rt::ResourceAccess::read},
                 std::tuple{
                     right_phase,
                     right_resource,
                     rt::ResourceAccess::write},
                 std::tuple{
                     combine_phase,
                     left_resource,
                     rt::ResourceAccess::read},
                 std::tuple{
                     combine_phase,
                     right_resource,
                     rt::ResourceAccess::read},
                 std::tuple{
                     combine_phase,
                     total_resource,
                     rt::ResourceAccess::write},
             }) {
            status = runtime.declare_resource_access(
                std::get<0>(declaration),
                std::get<1>(declaration),
                std::get<2>(declaration));
            if (status != rt::Status::ok) {
                return status;
            }
        }

        for (auto registration : {
                 rt::StateRegistration{
                     "input",
                     state_schema_version,
                     input},
                 rt::StateRegistration{
                     "left",
                     state_schema_version,
                     left},
                 rt::StateRegistration{
                     "right",
                     state_schema_version,
                     right},
                 rt::StateRegistration{
                     "total",
                     state_schema_version,
                     total},
             }) {
            status = runtime.register_state(registration);
            if (status != rt::Status::ok) {
                return status;
            }
        }
        if ((status = runtime.finalize()) != rt::Status::ok) {
            return status;
        }
        return runtime.start();
    }

    rt::Status run_frame(
        std::uint64_t frame,
        std::uint64_t input_value) {
        store_value(input, input_value);
        return runtime.step(rt::HostFrameContext{
            frame,
            std::chrono::nanoseconds(1'000'000),
            std::nullopt,
        });
    }

    std::array<CanonicalU64, 4> state() const {
        return {input, left, right, total};
    }
};

std::vector<std::byte> write_checkpoint(
    DeterministicWorkload& workload,
    std::uint64_t frame,
    rt::CheckpointMetadata* metadata = nullptr) {
    std::size_t required = 0;
    EXPECT_EQ(
        workload.runtime.checkpoint_size(required),
        rt::Status::ok);
    std::vector<std::byte> checkpoint(required);
    rt::ArtifactWriteResult write_result;
    EXPECT_EQ(
        workload.runtime.write_checkpoint(
            frame,
            checkpoint,
            write_result),
        rt::Status::ok);
    EXPECT_EQ(write_result.required_bytes, required);
    EXPECT_EQ(write_result.bytes_written, required);
    if (metadata) {
        EXPECT_EQ(
            rt::inspect_checkpoint_artifact(
                checkpoint,
                *metadata),
            rt::Status::ok);
    }
    return checkpoint;
}

struct ReplayInputTarget {
    CanonicalU64* input = nullptr;
    std::size_t calls = 0;
};

rt::CallbackResult apply_replay_input(
    void* opaque,
    const rt::ReplayInputView& input) {
    auto& target = *static_cast<ReplayInputTarget*>(opaque);
    if (!target.input ||
        input.input_type != 7u ||
        input.payload.size() != target.input->size()) {
        return rt::CallbackResult::error;
    }
    std::memcpy(
        target.input->data(),
        input.payload.data(),
        input.payload.size());
    ++target.calls;
    return rt::CallbackResult::ok;
}

struct D1Summary {
    std::uint64_t config_id = 0;
    std::uint64_t replay_id = 0;
    std::uint64_t state_hash = 0;
    std::array<CanonicalU64, 4> state{};
};

D1Summary run_d1_workload(std::size_t workers) {
    DeterministicWorkload workload;
    EXPECT_EQ(workload.prepare(workers), rt::Status::ok);
    for (std::uint64_t frame = 0; frame < 40; ++frame) {
        EXPECT_EQ(
            workload.run_frame(frame, frame * 19u + 11u),
            rt::Status::ok);
    }
    rt::CheckpointMetadata metadata;
    (void)write_checkpoint(workload, 39, &metadata);
    EXPECT_EQ(workload.runtime.stop(), rt::Status::ok);
    return {
        metadata.config_id,
        metadata.replay_id,
        metadata.state_hash,
        workload.state(),
    };
}

} // namespace

TEST(DeterminismReplay, D1RegisteredStateMatchesAcrossWorkerCounts) {
    const auto one = run_d1_workload(1);
    const auto two = run_d1_workload(2);
    const auto four = run_d1_workload(4);

    EXPECT_NE(one.config_id, two.config_id);
    EXPECT_NE(two.config_id, four.config_id);
    EXPECT_EQ(one.replay_id, two.replay_id);
    EXPECT_EQ(two.replay_id, four.replay_id);
    EXPECT_EQ(one.state_hash, two.state_hash);
    EXPECT_EQ(two.state_hash, four.state_hash);
    EXPECT_EQ(one.state, two.state);
    EXPECT_EQ(two.state, four.state);
}

TEST(DeterminismReplay, D1CheckpointTransfersAcrossWorkerCounts) {
    DeterministicWorkload producer;
    ASSERT_EQ(producer.prepare(1), rt::Status::ok);
    for (std::uint64_t frame = 0; frame < 12; ++frame) {
        ASSERT_EQ(
            producer.run_frame(frame, frame * 23u + 5u),
            rt::Status::ok);
    }
    const auto expected = producer.state();
    rt::CheckpointMetadata produced_metadata;
    const auto checkpoint =
        write_checkpoint(producer, 11, &produced_metadata);

    DeterministicWorkload consumer;
    ASSERT_EQ(consumer.prepare(4), rt::Status::ok);
    rt::CheckpointMetadata restored_metadata;
    ASSERT_EQ(
        consumer.runtime.restore_checkpoint(
            checkpoint,
            &restored_metadata),
        rt::Status::ok);
    EXPECT_EQ(consumer.state(), expected);
    EXPECT_EQ(
        restored_metadata.replay_id,
        produced_metadata.replay_id);
    rt::CheckpointMetadata consumer_metadata;
    (void)write_checkpoint(consumer, 11, &consumer_metadata);
    EXPECT_NE(
        produced_metadata.config_id,
        consumer_metadata.config_id);
    EXPECT_EQ(
        produced_metadata.replay_id,
        consumer_metadata.replay_id);

    EXPECT_EQ(producer.runtime.stop(), rt::Status::ok);
    EXPECT_EQ(consumer.runtime.stop(), rt::Status::ok);
}

TEST(DeterminismReplay, D0RequiresExactResolvedConfiguration) {
    DeterministicWorkload producer;
    ASSERT_EQ(
        producer.prepare(
            1,
            rt::DeterminismTier::unspecified),
        rt::Status::ok);
    ASSERT_EQ(producer.run_frame(0, 17), rt::Status::ok);
    const auto checkpoint = write_checkpoint(producer, 0);

    DeterministicWorkload exact;
    ASSERT_EQ(
        exact.prepare(
            1,
            rt::DeterminismTier::unspecified),
        rt::Status::ok);
    EXPECT_EQ(
        exact.runtime.restore_checkpoint(checkpoint),
        rt::Status::ok);
    EXPECT_EQ(exact.state(), producer.state());

    DeterministicWorkload different_workers;
    ASSERT_EQ(
        different_workers.prepare(
            2,
            rt::DeterminismTier::unspecified),
        rt::Status::ok);
    const auto before = different_workers.state();
    EXPECT_EQ(
        different_workers.runtime.restore_checkpoint(
            checkpoint),
        rt::Status::incompatible_artifact);
    EXPECT_EQ(different_workers.state(), before);

    EXPECT_EQ(producer.runtime.stop(), rt::Status::ok);
    EXPECT_EQ(exact.runtime.stop(), rt::Status::ok);
    EXPECT_EQ(
        different_workers.runtime.stop(),
        rt::Status::ok);
}

TEST(DeterminismReplay, CheckpointAndInputLogReproduceState) {
    DeterministicWorkload workload;
    ASSERT_EQ(workload.prepare(4), rt::Status::ok);
    for (std::uint64_t frame = 0; frame < 10; ++frame) {
        ASSERT_EQ(
            workload.run_frame(frame, frame * 7u + 1u),
            rt::Status::ok);
    }
    const auto checkpoint = write_checkpoint(workload, 9);

    std::array<CanonicalU64, 20> payloads{};
    std::array<rt::ReplayInputRecord, 20> records{};
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto frame = static_cast<std::uint64_t>(index + 10);
        store_value(payloads[index], frame * 13u + 9u);
        records[index] = rt::ReplayInputRecord{
            rt::HostFrameContext{
                frame,
                std::chrono::nanoseconds(1'000'000),
                std::nullopt,
            },
            7,
            payloads[index],
        };
        ASSERT_EQ(
            workload.run_frame(
                frame,
                load_value(payloads[index])),
            rt::Status::ok);
    }
    const auto expected_state = workload.state();
    std::uint64_t expected_hash = 0;
    ASSERT_EQ(
        workload.runtime.registered_state_hash(expected_hash),
        rt::Status::ok);

    std::vector<std::byte> input_log(16 * 1024);
    rt::ArtifactWriteResult input_result;
    ASSERT_EQ(
        workload.runtime.write_input_log(
            records,
            input_log,
            input_result),
        rt::Status::ok);
    input_log.resize(input_result.bytes_written);

    store_value(workload.input, 0x1111u);
    store_value(workload.left, 0x2222u);
    store_value(workload.right, 0x3333u);
    store_value(workload.total, 0x4444u);
    ReplayInputTarget target{&workload.input, 0};
    rt::ReplayResult replay_result;
    ASSERT_EQ(
        workload.runtime.replay(
            checkpoint,
            input_log,
            &apply_replay_input,
            &target,
            &replay_result),
        rt::Status::ok);

    EXPECT_EQ(target.calls, records.size());
    EXPECT_EQ(replay_result.records_processed, records.size());
    EXPECT_EQ(replay_result.frames_replayed, records.size());
    EXPECT_EQ(replay_result.final_state_hash, expected_hash);
    EXPECT_EQ(workload.state(), expected_state);
    EXPECT_EQ(workload.runtime.stop(), rt::Status::ok);
}

TEST(DeterminismReplay, CorruptCheckpointNeverMutatesState) {
    DeterministicWorkload workload;
    ASSERT_EQ(workload.prepare(2), rt::Status::ok);
    for (std::uint64_t frame = 0; frame < 4; ++frame) {
        ASSERT_EQ(
            workload.run_frame(frame, frame + 10u),
            rt::Status::ok);
    }
    const auto checkpoint = write_checkpoint(workload, 3);

    store_value(workload.input, 101u);
    store_value(workload.left, 202u);
    store_value(workload.right, 303u);
    store_value(workload.total, 404u);
    const auto before = workload.state();

    for (const std::size_t offset : {
             std::size_t{0},
             std::size_t{8},
             std::size_t{36},
             std::size_t{72},
             std::size_t{112},
             std::size_t{255},
             std::size_t{256},
             checkpoint.size() - 1,
         }) {
        auto corrupt = checkpoint;
        corrupt[offset] ^= std::byte{0x01};
        EXPECT_EQ(
            workload.runtime.restore_checkpoint(corrupt),
            rt::Status::invalid_artifact);
        EXPECT_EQ(workload.state(), before);
    }
    EXPECT_EQ(workload.runtime.stop(), rt::Status::ok);
}

TEST(DeterminismReplay, ForeignStateSchemaAndGraphAreRejected) {
    DeterministicWorkload producer;
    ASSERT_EQ(producer.prepare(1), rt::Status::ok);
    ASSERT_EQ(producer.run_frame(0, 17), rt::Status::ok);
    const auto checkpoint = write_checkpoint(producer, 0);

    DeterministicWorkload foreign_schema;
    ASSERT_EQ(
        foreign_schema.prepare(
            1,
            rt::DeterminismTier::schedule_independent,
            2),
        rt::Status::ok);
    const auto schema_before = foreign_schema.state();
    EXPECT_EQ(
        foreign_schema.runtime.restore_checkpoint(checkpoint),
        rt::Status::incompatible_artifact);
    EXPECT_EQ(foreign_schema.state(), schema_before);

    DeterministicWorkload foreign_graph;
    ASSERT_EQ(
        foreign_graph.prepare(
            1,
            rt::DeterminismTier::schedule_independent,
            1,
            "left.v2"),
        rt::Status::ok);
    const auto graph_before = foreign_graph.state();
    EXPECT_EQ(
        foreign_graph.runtime.restore_checkpoint(checkpoint),
        rt::Status::incompatible_artifact);
    EXPECT_EQ(foreign_graph.state(), graph_before);

    EXPECT_EQ(producer.runtime.stop(), rt::Status::ok);
    EXPECT_EQ(foreign_schema.runtime.stop(), rt::Status::ok);
    EXPECT_EQ(foreign_graph.runtime.stop(), rt::Status::ok);
}

TEST(DeterminismReplay, InvalidInputLogIsRejectedBeforeRestore) {
    DeterministicWorkload workload;
    ASSERT_EQ(workload.prepare(1), rt::Status::ok);
    ASSERT_EQ(workload.run_frame(0, 9), rt::Status::ok);
    const auto checkpoint = write_checkpoint(workload, 0);

    CanonicalU64 payload{};
    store_value(payload, 77);
    const std::array records{
        rt::ReplayInputRecord{
            rt::HostFrameContext{
                1,
                std::chrono::nanoseconds(1),
                std::nullopt,
            },
            7,
            payload,
        },
    };
    std::vector<std::byte> log(1024);
    rt::ArtifactWriteResult write_result;
    ASSERT_EQ(
        workload.runtime.write_input_log(
            records,
            log,
            write_result),
        rt::Status::ok);
    log.resize(write_result.bytes_written);
    log.back() ^= std::byte{0x80};

    store_value(workload.input, 501);
    store_value(workload.left, 502);
    store_value(workload.right, 503);
    store_value(workload.total, 504);
    const auto before = workload.state();
    ReplayInputTarget target{&workload.input, 0};
    EXPECT_EQ(
        workload.runtime.replay(
            checkpoint,
            log,
            &apply_replay_input,
            &target),
        rt::Status::invalid_artifact);
    EXPECT_EQ(target.calls, 0u);
    EXPECT_EQ(workload.state(), before);
    EXPECT_EQ(workload.runtime.stop(), rt::Status::ok);
}

TEST(DeterminismReplay, ParserMutationCorpusStaysBounded) {
    DeterministicWorkload workload;
    ASSERT_EQ(workload.prepare(1), rt::Status::ok);
    ASSERT_EQ(workload.run_frame(0, 42), rt::Status::ok);
    const auto checkpoint = write_checkpoint(workload, 0);

    std::uint64_t seed = 0x6a09e667f3bcc909ull;
    for (std::size_t iteration = 0;
         iteration < 5'000;
         ++iteration) {
        seed ^= seed << 13u;
        seed ^= seed >> 7u;
        seed ^= seed << 17u;
        const auto size = static_cast<std::size_t>(
            seed % (checkpoint.size() + 257u));
        std::vector<std::byte> candidate(size);
        for (std::size_t index = 0; index < size; ++index) {
            seed ^= seed << 13u;
            seed ^= seed >> 7u;
            seed ^= seed << 17u;
            candidate[index] =
                static_cast<std::byte>(seed & 0xffu);
        }
        rt::CheckpointMetadata checkpoint_metadata;
        EXPECT_NE(
            rt::inspect_checkpoint_artifact(
                candidate,
                checkpoint_metadata),
            rt::Status::ok);
        rt::InputLogMetadata input_metadata;
        EXPECT_NE(
            rt::inspect_input_log_artifact(
                candidate,
                input_metadata),
            rt::Status::ok);
    }
    EXPECT_EQ(workload.runtime.stop(), rt::Status::ok);
}

TEST(DeterminismReplay, D1RejectsTimingAndThroughputPolicies) {
    rt::RuntimeConfig config;
    config.determinism_tier =
        rt::DeterminismTier::schedule_independent;
    config.executor_policy =
        rt::ExecutorPolicy::bounded_throughput;
    rt::Runtime throughput;
    EXPECT_EQ(
        throughput.configure(config),
        rt::Status::invalid_config);

    config.executor_policy =
        rt::ExecutorPolicy::static_deterministic;
    config.watchdog_timeout_ns = 1;
    rt::Runtime watchdog;
    EXPECT_EQ(
        watchdog.configure(config),
        rt::Status::invalid_config);

    config.watchdog_timeout_ns = 0;
    config.determinism_tier =
        rt::DeterminismTier::reproducible_build;
    rt::Runtime d2;
    EXPECT_EQ(d2.configure(config), rt::Status::invalid_config);
}

TEST(DeterminismReplay, StateAndArtifactStorageMustNotOverlap) {
    rt::RuntimeConfig config;
    config.state_capacity = 2;
    config.snapshot_max_bytes = 1024;
    config.input_log_max_bytes = 1024;

    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    std::array<std::byte, 32> backing{};
    ASSERT_EQ(
        runtime.register_state(
            {"primary", 1, std::span(backing).first<16>()}),
        rt::Status::ok);
    EXPECT_EQ(
        runtime.register_state(
            {"overlap", 1, std::span(backing).subspan<8, 16>()}),
        rt::Status::invalid_argument);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::ArtifactWriteResult result;
    EXPECT_EQ(
        runtime.write_input_log(
            {},
            std::span(backing).first<16>(),
            result),
        rt::Status::invalid_argument);
    EXPECT_EQ(result.bytes_written, 0u);

    std::array<rt::ReplayInputRecord, 1> records{};
    auto record_storage = std::as_writable_bytes(
        std::span(records));
    EXPECT_EQ(
        runtime.write_input_log(
            records,
            record_storage,
            result),
        rt::Status::invalid_argument);
    EXPECT_EQ(result.bytes_written, 0u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(DeterminismReplay, InputLogRequiresStrictFrameOrder) {
    DeterministicWorkload workload;
    ASSERT_EQ(workload.prepare(1), rt::Status::ok);
    CanonicalU64 payload{};
    std::array records{
        rt::ReplayInputRecord{
            rt::HostFrameContext{
                2,
                std::chrono::nanoseconds(1),
                std::nullopt,
            },
            7,
            payload,
        },
        rt::ReplayInputRecord{
            rt::HostFrameContext{
                2,
                std::chrono::nanoseconds(1),
                std::nullopt,
            },
            7,
            payload,
        },
    };
    std::array<std::byte, 1024> output{};
    rt::ArtifactWriteResult result;
    EXPECT_EQ(
        workload.runtime.write_input_log(
            records,
            output,
            result),
        rt::Status::invalid_argument);
    EXPECT_EQ(result.bytes_written, 0u);
    EXPECT_EQ(workload.runtime.stop(), rt::Status::ok);
}
