#!/usr/bin/env python3
"""Validate version, CLI, links, and executable documentation claims."""

from __future__ import annotations

import hashlib
import json
import pathlib
import re
import sys
from typing import Iterable


ROOT = pathlib.Path(__file__).resolve().parents[1]
FAILURES: list[str] = []


def fail(message: str) -> None:
    FAILURES.append(message)


def read(relative: str) -> str:
    path = ROOT / relative
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        fail(f"{relative}: cannot read: {exc}")
        return ""


def require_files(paths: Iterable[str]) -> None:
    for relative in paths:
        if not (ROOT / relative).is_file():
            fail(f"{relative}: required contract file is missing")


def check_version() -> str:
    version = read("VERSION.txt").strip()
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", version)
    if not match:
        fail("VERSION.txt: expected MAJOR.MINOR.PATCH")
        return version

    major, minor, patch = match.groups()
    header = read("include/rtfw/version.h")
    expected_header = {
        "RTFW_VERSION_MAJOR": major,
        "RTFW_VERSION_MINOR": minor,
        "RTFW_VERSION_PATCH": patch,
        "RTFW_VERSION_STRING": f'"{version}"',
    }
    for name, value in expected_header.items():
        if not re.search(
            rf"^\s*#define\s+{re.escape(name)}\s+{re.escape(value)}\s*$",
            header,
            re.MULTILINE,
        ):
            fail(f"include/rtfw/version.h: {name} does not match VERSION.txt")

    try:
        manifest = json.loads(read("vcpkg.json"))
    except json.JSONDecodeError as exc:
        fail(f"vcpkg.json: invalid JSON: {exc}")
    else:
        if manifest.get("version-string") != version:
            fail("vcpkg.json: version-string does not match VERSION.txt")

    try:
        cuda_matrix = json.loads(read("docs/cuda_support_matrix.json"))
    except json.JSONDecodeError as exc:
        fail(f"docs/cuda_support_matrix.json: invalid JSON: {exc}")
    else:
        if cuda_matrix.get("runtime_version") != version:
            fail(
                "docs/cuda_support_matrix.json: runtime_version does not "
                "match VERSION.txt"
            )

    try:
        xdma_matrix = json.loads(read("docs/xdma_support_matrix.json"))
    except json.JSONDecodeError as exc:
        fail(f"docs/xdma_support_matrix.json: invalid JSON: {exc}")
    else:
        if xdma_matrix.get("runtime_version") != version:
            fail(
                "docs/xdma_support_matrix.json: runtime_version does not "
                "match VERSION.txt"
            )

    try:
        portable_matrix = json.loads(read("docs/portable_support_matrix.json"))
    except json.JSONDecodeError as exc:
        fail(f"docs/portable_support_matrix.json: invalid JSON: {exc}")
    else:
        if portable_matrix.get("runtime_version") != version:
            fail(
                "docs/portable_support_matrix.json: runtime_version does not "
                "match VERSION.txt"
            )

    cmake = read("CMakeLists.txt")
    required_cmake = (
        'file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION.txt"',
        'project(rtfw VERSION "${RTFW_VERSION}"',
        "write_basic_package_version_file(",
        'VERSION "${PROJECT_VERSION}"',
    )
    for snippet in required_cmake:
        if snippet not in cmake:
            fail(f"CMakeLists.txt: missing version contract snippet {snippet!r}")

    readme = read("README.md")
    if f"**Status: {version} portable RT0 release.**" not in readme:
        fail("README.md: status version does not match VERSION.txt")

    return version


def check_license() -> None:
    license_path = ROOT / "LICENSE"
    if not license_path.is_file():
        return
    digest = hashlib.sha256(license_path.read_bytes()).hexdigest()
    expected = "c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4"
    if digest != expected:
        fail("LICENSE: expected the unmodified Apache License 2.0 text")


def markdown_files() -> list[pathlib.Path]:
    paths = [ROOT / "README.md"]
    paths.extend((ROOT / "docs").rglob("*.md"))
    paths.extend(
        [
            ROOT / "tools/autotune/README.md",
            ROOT / "profiles/README.md",
            ROOT / "results/README.md",
            ROOT / "reports/README.md",
        ]
    )
    return sorted({path for path in paths if path.is_file()})


def check_markdown_links() -> None:
    link_pattern = re.compile(r"(?<!!)\[[^\]]+\]\(([^)]+)\)")
    for source in markdown_files():
        text = source.read_text(encoding="utf-8")
        for raw_target in link_pattern.findall(text):
            target = raw_target.strip().split(maxsplit=1)[0].strip("<>")
            if (
                not target
                or target.startswith("#")
                or re.match(r"^[a-z][a-z0-9+.-]*:", target, re.IGNORECASE)
            ):
                continue
            relative = target.split("#", 1)[0]
            if not relative:
                continue
            destination = (source.parent / relative).resolve()
            try:
                destination.relative_to(ROOT.resolve())
            except ValueError:
                fail(f"{source.relative_to(ROOT)}: link escapes repository: {target}")
                continue
            if not destination.exists():
                fail(f"{source.relative_to(ROOT)}: broken link: {target}")


def check_cli_contract() -> None:
    source = read("src/main.cpp")
    implemented = set(
        re.findall(r'std::strcmp\(argv\[i\],\s*"(--[a-z0-9-]+)"\)', source)
    )

    readme = read("README.md")
    match = re.search(
        r"<!-- cli-options:start -->(.*?)<!-- cli-options:end -->",
        readme,
        re.DOTALL,
    )
    if not match:
        fail("README.md: missing CLI option contract markers")
        return
    documented = set(re.findall(r"`(--[a-z0-9-]+)(?:\s+[^`]*)?`", match.group(1)))
    if implemented != documented:
        missing = sorted(implemented - documented)
        extra = sorted(documented - implemented)
        if missing:
            fail(f"README.md: implemented CLI options not documented: {missing}")
        if extra:
            fail(f"README.md: documented CLI options not implemented: {extra}")

    if "Unknown option:" not in source:
        fail("src/main.cpp: unknown CLI options are not rejected")


def check_verified_commands() -> None:
    readme = read("README.md")
    marker_pattern = re.compile(
        r"<!-- ci-verified: ([^ ]+) -->\s*```bash\n(.*?)```",
        re.DOTALL,
    )
    matches = marker_pattern.findall(readme)
    if not matches:
        fail("README.md: no ci-verified bash block found")
        return

    for workflow_name, block in matches:
        workflow_path = ROOT / workflow_name
        if not workflow_path.is_file():
            fail(f"README.md: verification workflow does not exist: {workflow_name}")
            continue
        workflow = workflow_path.read_text(encoding="utf-8")
        for raw_line in block.splitlines():
            command = raw_line.strip()
            if not command or command.startswith("#"):
                continue
            if command not in workflow:
                fail(
                    f"README.md: verified command is absent from "
                    f"{workflow_name}: {command}"
                )


def check_claims() -> None:
    surfaces = "\n".join(path.read_text(encoding="utf-8") for path in markdown_files())
    banned = (
        "production-grade realtime",
        "guarantees bounded latency",
        "A lightweight work-stealing job system drives",
        "Percentiles (`p50/p95/p99`) accumulate for the entire run.",
        "./build/bin/rtfw_demo",
    )
    for claim in banned:
        if claim.lower() in surfaces.lower():
            fail(f"documentation contains retired claim/path: {claim!r}")

    stale_version = re.compile(r"\b(?:RTFW|version|demo)\s+0\.1\b", re.IGNORECASE)
    if stale_version.search(surfaces):
        fail("documentation contains a stale 0.1 release reference")

    readme = read("README.md")
    required_qualifiers = (
        "portable RT0 release",
        "no hard-real-time",
        "no C++ binary ABI",
        "No RT2 record exists yet.",
        "Legacy GPU stub | Experimental compatibility path",
        "Xilinx XDMA AXI-MM backend | Candidate; not hardware-qualified",
    )
    for text in required_qualifiers:
        if text not in readme:
            fail(f"README.md: missing required qualification: {text!r}")


def check_runtime_contract() -> None:
    cmake = read("CMakeLists.txt")
    samples_cmake = read("samples/CMakeLists.txt")
    tests_cmake = read("tests/CMakeLists.txt")
    runtime_header = read("rt/include/rt/runtime.hpp")
    runtime_source = read("rt/src/host_runtime.cpp")
    c_header = read("rt/include/rt/c_api.h")
    roadmap = read("docs/roadmap.md")
    host_doc = read("docs/host_runtime.md")
    graph_doc = read("docs/compiled_graph.md")
    graph_source = read("rt/src/compiled_graph.cpp")
    graph_test = read("tests/test_compiled_graph.cpp")
    executor_doc = read("docs/executor.md")
    memory_doc = read("docs/memory_plan.md")
    time_doc = read("docs/time_platform.md")
    observability_doc = read("docs/observability.md")
    determinism_doc = read("docs/determinism_replay.md")
    device_doc = read("docs/device_backend.md")
    cuda_doc = read("docs/cuda_backend.md")
    xdma_doc = read("docs/xdma_backend.md")
    c_abi_doc = read("docs/c_abi.md")
    device_abi = read("rt/include/rt/device_abi.h")
    cuda_header = read("rt/include/rt/cuda_backend.hpp")
    xdma_header = read("rt/include/rt/xdma_backend.hpp")
    xdma_linux_header = read("rt/include/rt/xdma_linux.hpp")
    device_manager = read("rt/src/device_manager.cpp")
    mock_device = read("rt/src/mock_device.cpp")
    cuda_backend = read("rt/src/cuda_backend.cpp")
    cuda_driver = read("rt/src/cuda_driver.cpp")
    xdma_backend = read("rt/src/xdma_backend.cpp")
    xdma_linux = read("rt/src/xdma_linux.cpp")
    aligned_storage = read("rt/src/aligned_storage.hpp")
    executor_source = read("rt/src/executor.cpp")
    watchdog_source = read("rt/src/watchdog_monitor.cpp")
    preflight_source = read("rt/src/native_platform_preflight.cpp")
    telemetry_source = read("rt/src/telemetry.cpp")
    snapshot_codec_header = read("rt/src/snapshot_codec.hpp")
    snapshot_codec_source = read("rt/src/snapshot_codec.cpp")
    host_test = read("tests/test_host_runtime.cpp")
    executor_test = read("tests/test_executor.cpp")
    memory_test = read("tests/test_memory_plan.cpp")
    periodic_test = read("tests/test_periodic_runtime.cpp")
    preflight_test = read("tests/test_platform_preflight.cpp")
    observability_test = read("tests/test_observability.cpp")
    determinism_test = read("tests/test_determinism_replay.cpp")
    device_test = read("tests/test_device_runtime.cpp")
    cuda_test = read("tests/test_cuda_backend.cpp")
    xdma_test = read("tests/xdma_backend_tests.cpp")
    host_adapter_test = read("tests/host_adapter_tests.cpp")
    package_consumer = read("tests/package_consumer/CMakeLists.txt")
    snapshot_fuzz = read("tests/snapshot_fuzz.cpp")
    noalloc_test = read("tests/test_trace_noalloc.cpp")
    ci_workflow = read(".github/workflows/ci.yml")
    c_sample = read("samples/embed_c/mini_app.c")
    cpp_sample = read("samples/embed_cpp/mini_app.cpp")
    device_sample = read("samples/device_mock.cpp")
    cuda_sample = read("samples/cuda_qualification.cpp")
    xdma_sample = read("samples/xdma_qualification.cpp")
    cuda_workflow = read(".github/workflows/cuda-qualification.yml")
    xdma_workflow = read(".github/workflows/xdma-qualification.yml")
    release_workflow = read(".github/workflows/release.yml")
    release_contract = read("release/rtfw-release-contract.json")
    release_checker = read("tools/check_release_contract.py")
    release_manifest = read("tools/release_manifest.py")
    release_stager = read("tools/stage_release_artifacts.py")
    release_extractor = read("tools/extract_release_archive.py")
    hardware_checker = read("tools/check_hardware_evidence.py")
    release_test = read("tests/test_release_tools.py")
    portable_matrix = read("docs/portable_support_matrix.json")
    release_policy = read("docs/release_policy.md")

    for method in (
        "Status configure(",
        "Status register_callback(",
        "Status finalize()",
        "Status start()",
        "Status step(",
        "Status stop()",
    ):
        if method not in runtime_header:
            fail(f"rt/include/rt/runtime.hpp: missing M1 lifecycle method {method!r}")

    forbidden_host_pacing = ("sleep_for", "std::thread")
    for token in forbidden_host_pacing:
        if token in runtime_source:
            fail(f"rt/src/host_runtime.cpp: host-driven runtime contains {token!r}")
    if "HostDrivenStepDoesNotPace" not in host_test:
        fail("tests/test_host_runtime.cpp: missing host-driven no-pacing gate")

    schema_keys = {
        "callback_capacity",
        "scratch_bytes",
        "trace_capacity",
        "numerical_mode",
        "executor_policy",
        "worker_count",
        "executor_queue_capacity",
        "scratch_alignment",
        "task_scratch_bytes",
        "task_scratch_slots",
        "memory_budget_bytes",
        "overload_policy",
        "watchdog_timeout_ns",
        "watchdog_max_degradation_level",
        "platform_preflight_mode",
        "workload_id",
        "determinism_tier",
        "state_capacity",
        "snapshot_max_bytes",
        "replay_input_capacity",
        "input_log_max_bytes",
        "device_backend_capacity",
        "device_buffer_capacity",
        "device_outstanding_capacity",
        "device_completion_batch",
    }
    implemented_keys = set(
        re.findall(r'key\s*==\s*"([a-z0-9_]+)"', runtime_source)
    )
    if implemented_keys != schema_keys:
        fail(
            "rt/src/host_runtime.cpp: runtime schema keys differ from contract: "
            f"implemented={sorted(implemented_keys)}, expected={sorted(schema_keys)}"
        )
    for key in schema_keys:
        if f"`{key}`" not in host_doc:
            fail(f"docs/host_runtime.md: missing schema key {key!r}")

    for function in (
        "rtfw_status_message(",
        "rtfw_create(",
        "rtfw_register_callback(",
        "rtfw_finalize(",
        "rtfw_start(",
        "rtfw_step(",
        "rtfw_stop(",
    ):
        if function not in c_header:
            fail(f"rt/include/rt/c_api.h: missing M1 C ABI function {function!r}")

    for method in (
        "Status register_resource(",
        "Status add_dependency(",
        "Status declare_resource_access(",
        "bool compiled_phase_at(",
    ):
        if method not in runtime_header:
            fail(f"rt/include/rt/runtime.hpp: missing M2 graph method {method!r}")

    for function in (
        "rtfw_register_phase(",
        "rtfw_register_resource(",
        "rtfw_add_dependency(",
        "rtfw_declare_resource_access(",
    ):
        if function not in c_header:
            fail(f"rt/include/rt/c_api.h: missing M2 C ABI function {function!r}")

    for method in (
        "Status parallel_for(",
        "Status parallel_reduce(",
        "bool static_phase_assignment_at(",
        "ExecutorStats executor_stats()",
    ):
        if method not in runtime_header:
            fail(f"rt/include/rt/runtime.hpp: missing M3 executor method {method!r}")

    for function in (
        "rtfw_parallel_for(",
        "rtfw_parallel_reduce(",
        "rtfw_task_worker_index(",
    ):
        if function not in c_header:
            fail(f"rt/include/rt/c_api.h: missing M3 C ABI function {function!r}")

    for method in (
        "scratch() const noexcept",
        "bool memory_plan(",
    ):
        if method not in runtime_header:
            fail(f"rt/include/rt/runtime.hpp: missing M4 method {method!r}")

    for function in (
        "rtfw_memory_plan_init(",
        "rtfw_task_scratch(",
        "rtfw_get_memory_plan(",
    ):
        if function not in c_header:
            fail(f"rt/include/rt/c_api.h: missing M4 C ABI function {function!r}")

    for method in (
        "Status run_periodic(",
        "bool platform_preflight_report(",
        "std::uint32_t degradation_level()",
        "sleep_until_ns(",
    ):
        if method not in runtime_header:
            fail(f"rt/include/rt/runtime.hpp: missing M5 method {method!r}")

    for function in (
        "rtfw_run_periodic(",
        "rtfw_get_platform_preflight_report(",
        "rtfw_get_degradation_level(",
        "rtfw_periodic_config_init(",
        "rtfw_periodic_run_result_init(",
        "rtfw_platform_preflight_report_init(",
    ):
        if function not in c_header:
            fail(f"rt/include/rt/c_api.h: missing M5 C ABI function {function!r}")

    for method in (
        "Status observability_metadata(",
        "Status metrics_snapshot(",
        "Status read_trace(",
    ):
        if method not in runtime_header:
            fail(f"rt/include/rt/runtime.hpp: missing M6 method {method!r}")

    for function in (
        "rtfw_get_observability_metadata(",
        "rtfw_get_metrics(",
        "rtfw_read_trace(",
        "rtfw_metric_cursor_init(",
        "rtfw_trace_cursor_init(",
    ):
        if function not in c_header:
            fail(f"rt/include/rt/c_api.h: missing M6 C ABI function {function!r}")

    for method in (
        "Status register_state(",
        "Status checkpoint_size(",
        "Status write_checkpoint(",
        "Status restore_checkpoint(",
        "Status write_input_log(",
        "Status replay(",
        "Status registered_state_hash(",
        "inspect_checkpoint_artifact(",
        "inspect_input_log_artifact(",
    ):
        if method not in runtime_header:
            fail(f"rt/include/rt/runtime.hpp: missing M7 method {method!r}")

    for function in (
        "rtfw_register_state(",
        "rtfw_checkpoint_size(",
        "rtfw_checkpoint_write(",
        "rtfw_checkpoint_inspect(",
        "rtfw_checkpoint_restore(",
        "rtfw_input_log_write(",
        "rtfw_input_log_inspect(",
        "rtfw_replay(",
        "rtfw_registered_state_hash(",
    ):
        if function not in c_header:
            fail(f"rt/include/rt/c_api.h: missing M7 C ABI function {function!r}")

    for method in (
        "Status register_device_backend(",
        "Status register_device_buffer(",
        "Status register_device_phase(",
        "Status device_health(",
        "Status reset_device(",
    ):
        if method not in runtime_header:
            fail(f"rt/include/rt/runtime.hpp: missing M8 method {method!r}")

    for function in (
        "rtfw_register_device_backend(",
        "rtfw_register_device_buffer(",
        "rtfw_register_device_phase(",
        "rtfw_get_device_health(",
        "rtfw_reset_device(",
    ):
        if function not in c_header:
            fail(f"rt/include/rt/c_api.h: missing M8 C ABI function {function!r}")

    for status in (
        "invalid_handle",
        "graph_cycle",
        "resource_conflict",
    ):
        if status not in runtime_header:
            fail(f"rt/include/rt/runtime.hpp: missing M2 status {status!r}")

    for status in ("platform_preflight_failed", "clock_failure"):
        if status not in runtime_header:
            fail(f"rt/include/rt/runtime.hpp: missing M5 status {status!r}")

    for status in ("invalid_artifact", "incompatible_artifact"):
        if status not in runtime_header:
            fail(f"rt/include/rt/runtime.hpp: missing M7 status {status!r}")

    for status in (
        "device_queue_full",
        "device_timeout",
        "device_error",
        "device_lost",
        "device_canceled",
        "device_reset_required",
    ):
        if status not in runtime_header or status.upper() not in c_header:
            fail(f"M8 status contract is missing {status!r}")

    for capability in (
        "compiled_graph",
        "host_driven_time",
        "unified_cpu_executor",
        "host_executor_adapter",
        "bounded_memory_plan",
        "self_paced_time",
        "frame_watchdog",
        "strict_platform_preflight",
        "versioned_observability",
        "deterministic_replay",
        "bounded_device_backend",
    ):
        if capability not in runtime_header or capability not in c_header:
            fail(f"M1 capability contract is missing {capability!r}")

    for snippet in (
        "target_compile_definitions(rtfw_shared PRIVATE RTFW_BUILD=1)",
        "target_compile_definitions(rtfw_static PUBLIC RTFW_STATIC_DEFINE=1)",
        "sample_embed_c_static",
    ):
        if snippet not in cmake and snippet not in samples_cmake:
            fail(f"CMake embedding contract is missing {snippet!r}")

    if (
        "| M1 | Complete |" not in roadmap
        or "| M2 | Complete |" not in roadmap
        or "| M3 | Complete |" not in roadmap
        or "| M4 | Complete |" not in roadmap
        or "| M5 | Complete |" not in roadmap
        or "| M6 | Complete |" not in roadmap
        or "| M7 | Complete |" not in roadmap
        or "| M8 | Complete |" not in roadmap
        or "| M9 | Candidate |" not in roadmap
        or "| M10 | Candidate |" not in roadmap
        or "| M11 | Complete |" not in roadmap
        or "| M12 | Complete |" not in roadmap
    ):
        fail("docs/roadmap.md: M8-M12 milestone status is not advanced")

    if not re.search(
        r"return\s*\{\s*true\s*,\s*true\s*,\s*true\s*,\s*true\s*,"
        r"\s*true\s*,\s*true\s*,\s*true\s*,\s*true\s*,"
        r"\s*true\s*,\s*true\s*,\s*true\s*\}\s*;",
        runtime_source,
    ):
        fail(
            "rt/src/host_runtime.cpp: M11 capability tuple is not eleven true values"
        )

    for token in (
        "std::priority_queue",
        "Status::graph_cycle",
        "Status::resource_conflict",
        "mark_reachable(",
    ):
        if token not in graph_source:
            fail(f"rt/src/compiled_graph.cpp: missing M2 compiler evidence {token!r}")

    for phrase in (
        "registration index",
        "Read | Read",
        "Write | Write",
        "dependency path",
        "bounded_memory_plan",
    ):
        if phrase not in graph_doc:
            fail(f"docs/compiled_graph.md: missing M2 contract phrase {phrase!r}")

    if "RandomizedDagsAgreeWithReferenceExecutor" not in graph_test:
        fail("tests/test_compiled_graph.cpp: missing randomized DAG evidence")
    if "CompiledGraphFirstFrameDoesNotAllocate" not in noalloc_test:
        fail("tests/test_trace_noalloc.cpp: missing M2 allocation gate")

    for token in (
        "ExecutorPolicy::static_deterministic",
        "ExecutorPolicy::bounded_throughput",
        "successful_steals_",
        "Status::queue_full",
        "kQueueCasAttemptLimit",
    ):
        if token not in executor_source:
            fail(f"rt/src/executor.cpp: missing M3 executor evidence {token!r}")
    for forbidden in (".detach(", "handleEmergency", "std::async"):
        if forbidden in executor_source:
            fail(
                "rt/src/executor.cpp: unified executor contains forbidden "
                f"emergency/detached path {forbidden!r}"
            )
    for phrase in (
        "static_deterministic",
        "bounded_throughput",
        "queue_full",
        "successful steals",
        "accepted prefix",
    ):
        if phrase not in executor_doc:
            fail(f"docs/executor.md: missing M3 contract phrase {phrase!r}")
    for test_name in (
        "StaticAssignmentMetadataAndExecutionAreStable",
        "QueueFullSubmissionReturnsWithinBound",
        "FailFrameEscalatesIgnoredQueueRejection",
        "IndependentNestedWorkPassesStaticStress",
        "ThroughputUsesLocalQueuesAndSuccessfulSteals",
    ):
        if test_name not in executor_test:
            fail(f"tests/test_executor.cpp: missing M3 gate {test_name!r}")

    for token in (
        "Status::scratch_exhausted",
        "kScratchCasAttemptLimit",
        "release_scratch_slot(",
        "OverloadPolicy::fail_frame",
    ):
        if token not in executor_source and token not in runtime_source:
            fail(f"M4 implementation is missing {token!r}")
    for forbidden in (
        "std::mutex",
        "std::condition_variable",
        "std::ifstream",
        "std::ofstream",
        "std::fstream",
    ):
        if forbidden in runtime_source or forbidden in executor_source:
            fail(f"M4 target CPU path contains forbidden RT-lane primitive {forbidden!r}")
    for token in (
        "::operator new(bytes, std::align_val_t(alignment))",
        "::operator delete(",
        "std::align_val_t(alignment_)",
    ):
        if token not in aligned_storage:
            fail(f"rt/src/aligned_storage.hpp: missing paired storage evidence {token!r}")
    for phrase in (
        "planned_bytes =",
        "reject_submission",
        "fail_frame",
        "allocator metadata",
        "OS thread stacks",
        "accepted prefix",
    ):
        if phrase not in memory_doc:
            fail(f"docs/memory_plan.md: missing M4 contract phrase {phrase!r}")
    for test_name in (
        "FinalizedPlanMatchesConfigurationAndAlignment",
        "NestedExecutionContextsOwnDistinctScratch",
        "RejectSubmissionReportsScratchExhaustion",
        "FailFrameEscalatesIgnoredScratchExhaustion",
    ):
        if test_name not in memory_test:
            fail(f"tests/test_memory_plan.cpp: missing M4 gate {test_name!r}")
    if "CompleteCpuFramesDoNotAllocate" not in noalloc_test:
        fail("tests/test_trace_noalloc.cpp: missing complete M4 allocation gate")

    for token in (
        "Runtime::run_periodic(",
        "clock_sleep_until(",
        "RuntimeTraceEventType::watchdog_fired",
        "PlatformPreflightMode::strict",
    ):
        if token not in runtime_source:
            fail(f"rt/src/host_runtime.cpp: missing M5 evidence {token!r}")
    for token in (
        "active_state_.compare_exchange",
        "service_cv_.wait_for(",
        "WatchdogMonitor::complete(",
    ):
        if token not in watchdog_source:
            fail(f"rt/src/watchdog_monitor.cpp: missing M5 evidence {token!r}")
    for forbidden in (
        "mlockall(",
        "sched_setaffinity(",
        "sched_setscheduler(",
        "setrlimit(",
    ):
        if forbidden in preflight_source:
            fail(
                "rt/src/native_platform_preflight.cpp: strict preflight "
                f"contains host mutation {forbidden!r}"
            )
    for phrase in (
        "first_release + i * period",
        "at most one event",
        "frame thread",
        "fails closed",
        "does not establish RT2",
    ):
        if phrase not in time_doc:
            fail(f"docs/time_platform.md: missing M5 contract phrase {phrase!r}")
    for test_name in (
        "UsesAbsoluteEpochBasedReleasesAndDeadlines",
        "LateFramesDoNotShiftTheReleaseEpoch",
        "WatchdogIsOneShotAndDegradesOnTheFrameThread",
        "WatchdogServiceDetectsExpiryWithoutRuntimeClockAdvance",
        "PropagatesClockFailureWithoutExecutingAFrame",
    ):
        if test_name not in periodic_test:
            fail(f"tests/test_periodic_runtime.cpp: missing M5 gate {test_name!r}")
    for test_name in (
        "StrictModePassesOnlyACompleteUniqueReport",
        "StrictFailureIsReportedBeforeWorkersStart",
        "DuplicatePrerequisiteCannotPassStrictMode",
        "DisabledModeDoesNotProbeOrMutateTheHost",
    ):
        if test_name not in preflight_test:
            fail(f"tests/test_platform_preflight.cpp: missing M5 gate {test_name!r}")
    if "watchdog_timeout_ns" not in noalloc_test:
        fail("tests/test_trace_noalloc.cpp: missing armed-watchdog allocation gate")
    if "#define RTFW_C_ABI_VERSION 8u" not in c_header:
        fail("rt/include/rt/c_api.h: M11 requires stable ABI version 8")
    if "rtfw_run_periodic(" not in c_sample:
        fail("samples/embed_c/mini_app.c: missing M5 periodic sample")
    if "runtime.run_periodic(" not in cpp_sample:
        fail("samples/embed_cpp/mini_app.cpp: missing M5 periodic sample")

    for token in (
        "ExecutorPolicy::host_adapter",
        "HostExecutorAdapter",
        "HostExecutorJob",
        "set_host_executor(",
        "host_completion_sequence_",
    ):
        if token not in runtime_header and token not in executor_source:
            fail(f"M11 host-adapter implementation is missing {token!r}")
    for token in (
        "RTFW_EXECUTOR_HOST_ADAPTER",
        "rtfw_host_executor",
        "rtfw_set_host_executor(",
        "RTFW_C_ABI_LAYOUT_FINGERPRINT",
        "rtfw_check_abi(",
    ):
        if token not in c_header:
            fail(f"rt/include/rt/c_api.h: missing M11 ABI token {token!r}")
    for token in (
        "cpp_host_adapter_contract",
        "c_host_adapter_contract",
        "worker_starts == 0",
        "allocation_count",
        "retained.completion_token",
    ):
        if token not in host_adapter_test:
            fail(f"tests/host_adapter_tests.cpp: missing M11 gate {token!r}")
    for phrase in (
        "first stable C ABI",
        "allowlist",
        "layout fingerprint",
        "host job system",
        "same process architecture",
    ):
        if phrase.lower() not in c_abi_doc.lower():
            fail(f"docs/c_abi.md: missing M11 contract phrase {phrase!r}")
    for token in (
        "rtfw_c_abi_v8.exports",
        "CXX_VISIBILITY_PRESET hidden",
        "SOVERSION \"${RTFW_C_ABI_VERSION}\"",
        "SameMinorVersion",
    ):
        if token not in cmake:
            fail(f"CMakeLists.txt: missing M11 distribution gate {token!r}")
    for token in (
        "COMPONENTS",
        "c_shared",
        "c_static",
        "cpp_runtime",
        "cuda_backend",
        "xdma_backend",
        "rtfw::rtfw",
        "rtfw::rtfw_static",
        "rtfw::simcore_rt",
        "rtfw::cuda_backend",
        "rtfw::xdma_backend",
    ):
        if token not in package_consumer:
            fail(f"package consumer is missing {token!r}")
    for token in (
        "Relocated package consumer",
        "check_c_abi.py --library",
        "rtfw-relocated",
    ):
        if token not in ci_workflow:
            fail(f".github/workflows/ci.yml: missing M11 gate {token!r}")

    for token, surface in (
        ("CrossInstanceDeviceStateIsIsolated", device_test),
        ("SameMajorVersion", cmake),
        ("include(CPack)", cmake),
        ("CPACK_PACKAGE_CHECKSUM SHA256", cmake),
        ("portable_rt0", release_contract),
        ("supported_tuples", portable_matrix),
        ("Source compatibility", release_policy),
        ("tools/check_release_contract.py", release_workflow),
        ("tools/release_manifest.py create", release_workflow),
        ("tools/release_manifest.py verify", release_workflow),
        (
            "actions/upload-artifact@"
            "ea165f8d65b6e75b540449e92b4886f43607fa02",
            release_workflow,
        ),
        ("HASHED_CONTRACT_PATHS", release_checker),
        ("source_commit", release_manifest),
        ("CPack SHA-256 sidecar does not match", release_stager),
        ("archive contains unsafe path", release_extractor),
        ("qualification_claim", hardware_checker),
        ("evidence_only", hardware_checker),
        ("test_round_trip_and_corruption_rejection", release_test),
    ):
        if token not in surface:
            fail(f"M12 portable release gate is missing {token!r}")

    for token in (
        "TelemetryRing::emit(",
        "committed_sequence",
        "overwritten_",
        "dropped_",
        "TelemetryCounters::increment(",
    ):
        if token not in telemetry_source:
            fail(f"rt/src/telemetry.cpp: missing M6 evidence {token!r}")
    for forbidden in (
        "std::mutex",
        "std::condition_variable",
        ".wait(",
        ".detach(",
    ):
        if forbidden in telemetry_source:
            fail(
                "rt/src/telemetry.cpp: M6 emission contains forbidden "
                f"RT-lane primitive {forbidden!r}"
            )
    for phrase in (
        "caller-owned",
        "Gauges are sampled",
        "version/build/config/workload",
        "non-RT host",
        "exact skipped sequence count",
    ):
        if phrase not in observability_doc:
            fail(
                "docs/observability.md: missing M6 contract phrase "
                f"{phrase!r}"
            )
    for test_name in (
        "MetadataCarriesStableProvenance",
        "IntervalWindowsPartitionCumulativeCounters",
        "TraceCursorReportsOverwriteLossExactly",
        "RuntimeInstancesRejectForeignCursors",
        "RejectsMalformedFreshCursors",
        "JsonExportCommitsCursorsOnlyAfterSuccess",
        "ContendedRingDropsInsteadOfWaiting",
    ):
        if test_name not in observability_test:
            fail(f"tests/test_observability.cpp: missing M6 gate {test_name!r}")
    if "Observability.*" not in ci_workflow:
        fail(".github/workflows/ci.yml: M6 is missing from the TSAN filter")
    if "rtfw_get_metrics(" not in c_sample:
        fail("samples/embed_c/mini_app.c: missing M6 metric sample")
    if "runtime.metrics_snapshot(" not in cpp_sample:
        fail("samples/embed_cpp/mini_app.cpp: missing M6 metric sample")

    for token in (
        "checkpoint_header_size = 256",
        "checkpoint_record_header_size = 88",
        "input_log_header_size = 192",
        "input_log_record_header_size = 48",
        "encode_checkpoint_artifact(",
        "parse_checkpoint_artifact(",
        "encode_input_log_artifact(",
        "parse_input_log_artifact(",
    ):
        if token not in snapshot_codec_header:
            fail(f"rt/src/snapshot_codec.hpp: missing M7 codec evidence {token!r}")
    for token in (
        "checked_artifact_add(",
        "artifact_checksum(",
        "store_u64_le(",
        "load_u64_le(",
    ):
        if token not in snapshot_codec_source:
            fail(f"rt/src/snapshot_codec.cpp: missing M7 codec evidence {token!r}")
    for forbidden in (
        "std::vector<",
        "std::basic_string<",
        "std::map<",
        "std::unordered_map<",
    ):
        if forbidden in snapshot_codec_header or forbidden in snapshot_codec_source:
            fail(
                "M7 artifact codec contains a forbidden allocating container "
                f"{forbidden!r}"
            )
    for token in (
        "DeterminismTier::schedule_independent",
        "Runtime::register_state(",
        "Runtime::restore_checkpoint(",
        "Runtime::write_input_log(",
        "Runtime::replay(",
        "registered state storage regions must not overlap",
        "input-log output cannot overlap registered state",
    ):
        if token not in runtime_source:
            fail(f"rt/src/host_runtime.cpp: missing M7 evidence {token!r}")
    for phrase in (
        "caller-owned",
        "transactionally restore",
        "little-endian",
        "D2",
        "D3",
        "non-RT host",
        "overlapping storage regions",
    ):
        if phrase not in determinism_doc:
            fail(
                "docs/determinism_replay.md: missing M7 contract phrase "
                f"{phrase!r}"
            )
    for test_name in (
        "D1RegisteredStateMatchesAcrossWorkerCounts",
        "D1CheckpointTransfersAcrossWorkerCounts",
        "D0RequiresExactResolvedConfiguration",
        "CheckpointAndInputLogReproduceState",
        "CorruptCheckpointNeverMutatesState",
        "ForeignStateSchemaAndGraphAreRejected",
        "InvalidInputLogIsRejectedBeforeRestore",
        "ParserMutationCorpusStaysBounded",
        "D1RejectsTimingAndThroughputPolicies",
        "StateAndArtifactStorageMustNotOverlap",
        "InputLogRequiresStrictFrameOrder",
    ):
        if test_name not in determinism_test:
            fail(f"tests/test_determinism_replay.cpp: missing M7 gate {test_name!r}")
    if "CheckpointAndInputCodecDoNotAllocate" not in noalloc_test:
        fail("tests/test_trace_noalloc.cpp: missing M7 allocation gate")
    for token in (
        "inspect_checkpoint_artifact(",
        "inspect_input_log_artifact(",
    ):
        if token not in snapshot_fuzz:
            fail(f"tests/snapshot_fuzz.cpp: missing M7 fuzz evidence {token!r}")
    for token in (
        "rtfw_determinism_artifact",
        "snapshot_fuzz",
        "DeterminismReplay.*",
        'cmp "$baseline" "$artifact"',
    ):
        if token not in ci_workflow:
            fail(f".github/workflows/ci.yml: missing M7 evidence {token!r}")
    for token in (
        "rt/src/snapshot_codec.cpp",
        "test_determinism_replay.cpp",
        "determinism_artifact.cpp",
        "snapshot_fuzz.cpp",
    ):
        if token not in cmake and token not in tests_cmake:
            fail(f"CMake M7 integration is missing {token!r}")
    for token, surface in (
        ("rtfw_register_state(", c_sample),
        ("rtfw_checkpoint_write(", c_sample),
        ("rtfw_checkpoint_inspect(", c_sample),
        ("runtime.register_state(", cpp_sample),
        ("runtime.write_checkpoint(", cpp_sample),
        ("inspect_checkpoint_artifact(", cpp_sample),
    ):
        if token not in surface:
            fail(f"embedding samples are missing M7 usage {token!r}")

    for token in (
        "#define RTFW_DEVICE_ABI_VERSION 1u",
        "rtfw_device_backend_api",
        "rtfw_device_submission",
        "rtfw_device_completion",
        "rtfw_device_health",
    ):
        if token not in device_abi:
            fail(f"rt/include/rt/device_abi.h: missing M8 ABI evidence {token!r}")
    for token in (
        "kOutstandingEarlyReady",
        "wake_sequence_.wait(",
        "api.submit(",
        "api.poll(",
        "complete_external(",
    ):
        if token not in device_manager:
            fail(f"rt/src/device_manager.cpp: missing M8 manager evidence {token!r}")
    for forbidden in (".detach(", "std::async", "std::condition_variable"):
        if forbidden in device_manager or forbidden in mock_device:
            fail(f"M8 target device path contains forbidden primitive {forbidden!r}")
    for phrase in (
        "CPU compute workers never wait",
        "callback exists in the ABI",
        "device_queue_full",
        "deterministic fault-injectable",
        "early-ready",
    ):
        if phrase not in device_doc:
            fail(f"docs/device_backend.md: missing M8 contract phrase {phrase!r}")
    for test_name in (
        "BoundedQueueSaturatesWithoutBlocking",
        "DelayedCompletionReleasesOnlyDependentSuccessors",
        "ConcurrentSubmissionsPreserveCausalPublication",
        "FaultsMapToStableRuntimeStatuses",
        "RejectsMalformedTablesAndAccountsMemory",
        "D1RejectsUndeclaredDeterministicBackend",
        "DestructionShutsBackendDown",
    ):
        if test_name not in device_test:
            fail(f"tests/test_device_runtime.cpp: missing M8 gate {test_name!r}")
    if "CompleteDeviceFramesDoNotAllocate" not in noalloc_test:
        fail("tests/test_trace_noalloc.cpp: missing M8 allocation gate")
    for token in (
        "rt/src/device_manager.cpp",
        "rt/src/mock_device.cpp",
        "test_device_runtime.cpp",
        "device_mock.cpp",
    ):
        if (
            token not in cmake
            and token not in tests_cmake
            and token not in samples_cmake
        ):
            fail(f"CMake M8 integration is missing {token!r}")
    if "DeviceRuntime.*:DeviceMock.*" not in ci_workflow:
        fail(".github/workflows/ci.yml: M8 is missing from the TSAN filter")
    for token in (
        "runtime.register_device_backend(",
        "runtime.register_device_buffer(",
        "runtime.register_device_phase(",
    ):
        if token not in device_sample:
            fail(f"samples/device_mock.cpp: missing M8 usage {token!r}")

    for token in (
        "struct CudaDriverApi",
        "class CudaDeviceBackend",
        "bind_device_buffer(",
        "register_kernel(",
        "cuda_device_opcode_copy_host_to_device",
        "cuda_device_opcode_launch_kernel",
        "struct CudaKernelLaunch",
    ):
        if token not in cuda_header:
            fail(f"rt/include/rt/cuda_backend.hpp: missing M9 evidence {token!r}")
    for token in (
        "kSlotQuarantined",
        "driver.event_query(",
        "driver.host_register(",
        "driver.stream_synchronize(",
        "RTFW_DEVICE_STATUS_TIMEOUT",
        "RTFW_DEVICE_HEALTH_LOST",
    ):
        if token not in cuda_backend:
            fail(f"rt/src/cuda_backend.cpp: missing M9 evidence {token!r}")
    for forbidden in (".detach(", "std::async", "std::condition_variable"):
        if forbidden in cuda_backend:
            fail(f"M9 CUDA state machine contains forbidden primitive {forbidden!r}")
    for token in (
        "cuEventQuery(",
        "cuMemHostRegister(",
        "cuMemcpyHtoDAsync(",
        "cuMemcpyDtoHAsync(",
        "cuLaunchKernel(",
    ):
        if token not in cuda_driver:
            fail(f"rt/src/cuda_driver.cpp: missing M9 Driver API call {token!r}")
    for phrase in (
        "host supplies a live `CUcontext`",
        "timeout quarantine",
        "No RT1, RT2",
        "qualified tuple",
        "drain-before-release",
    ):
        if phrase.lower() not in cuda_doc.lower():
            fail(f"docs/cuda_backend.md: missing M9 contract phrase {phrase!r}")
    for test_name in (
        "BoundedQueueSaturatesAndTimeoutQuarantinesUntilReady",
        "CopiesLaunchesKernelAndReturnsDeviceData",
        "DeviceCopyAndMemsetValidateAndPreserveRanges",
        "ExternalDeviceBindingRetainsCallerOwnership",
        "FailedRegistrationRetainsOwnershipForShutdown",
        "FailedEnqueueIsQuarantinedAndResetDrainsIt",
        "QueryFailureRequiresSuccessfulDrainBeforeReuse",
        "ShutdownDrainsOutstandingWorkBeforeFreeingBuffers",
        "FailedShutdownRetainsResourcesAndCanBeRetried",
        "ContextLossCompletesAsLostAndCannotSoftReset",
        "ConcurrentSubmissionsStayWithinFixedCapacity",
        "RuntimeGraphAcceptsCandidateBackendAtD0",
    ):
        if test_name not in cuda_test:
            fail(f"tests/test_cuda_backend.cpp: missing M9 gate {test_name!r}")
    if "CudaSubmitAndPollDoNotAllocateAfterInitialization" not in noalloc_test:
        fail("tests/test_trace_noalloc.cpp: missing M9 allocation gate")
    for token in (
        "rt/src/cuda_backend.cpp",
        "rt/src/cuda_driver.cpp",
        "test_cuda_backend.cpp",
        "cuda_qualification.cpp",
        "RTFW_ENABLE_CUDA",
        "target_compile_features(rtfw_cuda_backend PUBLIC cxx_std_20)",
    ):
        if (
            token not in cmake
            and token not in tests_cmake
            and token not in samples_cmake
        ):
            fail(f"CMake M9 integration is missing {token!r}")
    if "CudaBackend.*" not in ci_workflow:
        fail(".github/workflows/ci.yml: M9 is missing from the TSAN filter")
    for token in (
        "sample_cuda_qualification",
        "cuda-qualification.json",
        "workflow_dispatch",
        "self-hosted",
        "--warmup",
    ):
        if token not in cuda_workflow:
            fail(f"CUDA qualification workflow is missing {token!r}")
    for token in (
        "cuDevicePrimaryCtxRetain(",
        "cuda_driver_api()",
        "cuda_kernel_add_buffer_argument(",
        "qualification_claim",
        "warmup_iterations",
        "measurement_iterations",
    ):
        if token not in cuda_sample:
            fail(f"samples/cuda_qualification.cpp: missing M9 evidence {token!r}")

    try:
        cuda_support = json.loads(read("docs/cuda_support_matrix.json"))
    except json.JSONDecodeError as exc:
        fail(f"docs/cuda_support_matrix.json: invalid JSON: {exc}")
    else:
        if (
            cuda_support.get("schema_version") != 1
            or cuda_support.get("status") != "candidate"
            or cuda_support.get("qualified_tuples") != []
        ):
            fail(
                "docs/cuda_support_matrix.json: candidate matrix must be "
                "schema 1 with no qualified tuples"
            )

    for token in (
        "struct XdmaDriverApi",
        "class XdmaDeviceBackend",
        "xdma_device_opcode_host_to_card",
        "xdma_device_opcode_card_to_host",
        "struct XdmaTransfer",
    ):
        if token not in xdma_header:
            fail(f"rt/include/rt/xdma_backend.hpp: missing M10 evidence {token!r}")
    if "class LinuxXdmaDriver" not in xdma_linux_header:
        fail("rt/include/rt/xdma_linux.hpp: missing production adapter surface")
    for token in (
        "kSlotQueued",
        "driver.transfer(",
        "work_epoch.notify_one(",
        "timed_out",
        "RTFW_DEVICE_STATUS_TIMEOUT",
        "std::thread",
    ):
        if token not in xdma_backend:
            fail(f"rt/src/xdma_backend.cpp: missing M10 evidence {token!r}")
    for forbidden in (".detach(", "std::async", "std::condition_variable"):
        if forbidden in xdma_backend:
            fail(f"M10 XDMA state machine contains forbidden primitive {forbidden!r}")
    for token in (
        "O_CLOEXEC",
        "::pread(",
        "::pwrite(",
        "EINTR",
        "close_all(",
    ):
        if token not in xdma_linux:
            fail(f"rt/src/xdma_linux.cpp: missing M10 Linux adapter call {token!r}")
    for phrase in (
        "deliberately narrow",
        "fixed worker",
        "timeout quarantine",
        "No tuple has completed",
        "not a PCIe function-level reset",
    ):
        if phrase.lower() not in xdma_doc.lower():
            fail(f"docs/xdma_backend.md: missing M10 contract phrase {phrase!r}")
    for test_name in (
        "basic_round_trip",
        "saturation_and_timeout_quarantine",
        "validation_rejects_malformed_work",
        "recovery_and_no_allocation",
        "concurrent_submit_poll",
    ):
        if test_name not in xdma_test:
            fail(f"tests/xdma_backend_tests.cpp: missing M10 gate {test_name!r}")
    for token in (
        "rt/src/xdma_backend.cpp",
        "rt/src/xdma_linux.cpp",
        "xdma_backend_tests.cpp",
        "xdma_qualification.cpp",
        "RTFW_ENABLE_XDMA",
        "target_compile_features(rtfw_xdma_backend PUBLIC cxx_std_20)",
    ):
        if (
            token not in cmake
            and token not in tests_cmake
            and token not in samples_cmake
        ):
            fail(f"CMake M10 integration is missing {token!r}")
    for token in (
        "rtfw_xdma_backend_tests",
        "xdma_backend",
    ):
        if token not in ci_workflow:
            fail(f".github/workflows/ci.yml: M10 is missing TSAN evidence {token!r}")
    for token in (
        "sample_xdma_qualification",
        "xdma-qualification.json",
        "workflow_dispatch",
        "self-hosted",
        "bitstream_id",
        "--warmup",
    ):
        if token not in xdma_workflow:
            fail(f"XDMA qualification workflow is missing {token!r}")
    for token in (
        "LinuxXdmaDriver",
        "set_xdma_transfer(",
        "bitstream_id",
        "warmup_iterations",
        "measurement_iterations",
    ):
        if token not in xdma_sample:
            fail(f"samples/xdma_qualification.cpp: missing M10 evidence {token!r}")

    try:
        xdma_support = json.loads(read("docs/xdma_support_matrix.json"))
    except json.JSONDecodeError as exc:
        fail(f"docs/xdma_support_matrix.json: invalid JSON: {exc}")
    else:
        named_stack = xdma_support.get("named_stack", {})
        if (
            xdma_support.get("schema_version") != 1
            or xdma_support.get("status") != "candidate"
            or xdma_support.get("qualified_tuples") != []
            or named_stack.get("driver_repository") != "Xilinx/dma_ip_drivers"
            or not named_stack.get("driver_revision")
        ):
            fail(
                "docs/xdma_support_matrix.json: candidate matrix must name "
                "the Xilinx driver revision and contain no qualified tuple"
            )


def main() -> int:
    require_files(
        (
            "LICENSE",
            "VERSION.txt",
            "CHANGELOG.md",
            "SECURITY.md",
            "include/rtfw/version.h",
            "docs/product_contract.md",
            "docs/host_runtime.md",
            "docs/compiled_graph.md",
            "docs/executor.md",
            "docs/memory_plan.md",
            "docs/time_platform.md",
            "docs/observability.md",
            "docs/determinism_replay.md",
            "docs/device_backend.md",
            "docs/cuda_backend.md",
            "docs/cuda_support_matrix.json",
            "docs/xdma_backend.md",
            "docs/xdma_support_matrix.json",
            "docs/portable_support_matrix.json",
            "docs/release_policy.md",
            "docs/c_abi.md",
            "docs/roadmap.md",
            "docs/adr/0001-one-executor-boundary.md",
            "docs/adr/0002-host-driven-time.md",
            "docs/adr/0003-device-backend-boundary.md",
            ".github/workflows/docs-contract.yml",
            ".github/workflows/release.yml",
            ".github/workflows/cuda-qualification.yml",
            ".github/workflows/xdma-qualification.yml",
            "rt/include/rt/runtime.hpp",
            "rt/include/rt/graph.hpp",
            "rt/include/rt/c_api.h",
            "rt/include/rt/device_abi.h",
            "rt/include/rt/device.hpp",
            "rt/include/rt/mock_device.hpp",
            "rt/include/rt/cuda_backend.hpp",
            "rt/include/rt/cuda_driver.hpp",
            "rt/include/rt/xdma_backend.hpp",
            "rt/include/rt/xdma_linux.hpp",
            "rt/src/compiled_graph.cpp",
            "rt/src/aligned_storage.hpp",
            "rt/src/executor.cpp",
            "rt/src/host_runtime.cpp",
            "rt/src/watchdog_monitor.hpp",
            "rt/src/watchdog_monitor.cpp",
            "rt/src/native_platform_preflight.hpp",
            "rt/src/native_platform_preflight.cpp",
            "rt/src/telemetry.hpp",
            "rt/src/telemetry.cpp",
            "rt/src/snapshot_codec.hpp",
            "rt/src/snapshot_codec.cpp",
            "rt/src/device_manager.hpp",
            "rt/src/device_manager.cpp",
            "rt/src/mock_device.cpp",
            "rt/src/cuda_backend.cpp",
            "rt/src/cuda_driver.cpp",
            "rt/src/xdma_backend.cpp",
            "rt/src/xdma_linux.cpp",
            "rt/include/rt/observability_export.hpp",
            "rt/src/observability_export.cpp",
            "samples/embed_c/mini_app.c",
            "samples/embed_cpp/mini_app.cpp",
            "samples/device_mock.cpp",
            "samples/cuda_qualification.cpp",
            "samples/xdma_qualification.cpp",
            "tests/test_host_runtime.cpp",
            "tests/test_compiled_graph.cpp",
            "tests/test_executor.cpp",
            "tests/test_memory_plan.cpp",
            "tests/test_periodic_runtime.cpp",
            "tests/test_platform_preflight.cpp",
            "tests/test_observability.cpp",
            "tests/test_determinism_replay.cpp",
            "tests/test_device_runtime.cpp",
            "tests/test_release_tools.py",
            "tests/test_cuda_backend.cpp",
            "tests/xdma_backend_tests.cpp",
            "tests/host_adapter_tests.cpp",
            "tests/package_consumer/CMakeLists.txt",
            "tests/package_consumer/c_consumer.c",
            "tests/package_consumer/cpp_consumer.cpp",
            "tests/determinism_artifact.cpp",
            "tests/snapshot_fuzz.cpp",
            "abi/rtfw_c_abi_v8.exports",
            "abi/rtfw_c_abi_v8.sha256",
            "tools/check_c_abi.py",
            "tools/check_release_contract.py",
            "tools/check_hardware_evidence.py",
            "tools/release_manifest.py",
            "tools/extract_release_archive.py",
            "tools/stage_release_artifacts.py",
            "release/rtfw-release-contract.json",
        )
    )
    check_version()
    check_license()
    check_markdown_links()
    check_cli_contract()
    check_verified_commands()
    check_claims()
    check_runtime_contract()

    if FAILURES:
        print("Documentation contract failed:", file=sys.stderr)
        for failure in FAILURES:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("Documentation contract OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
