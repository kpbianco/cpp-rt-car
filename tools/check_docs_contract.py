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
    device_header = read("rt/include/rt/device.hpp")
    config_header = read("rt/include/rt/config.hpp")
    status_header = read("rt/include/rt/status.hpp")
    canonical_bytes_header = read("rt/include/rt/canonical_bytes.hpp")
    runtime_source = read("rt/src/host_runtime.cpp")
    resource_policy_header = read("rt/src/resource_policy.hpp")
    resource_policy_source = read("rt/src/resource_policy.cpp")
    memory_policy_header = read("rt/src/memory_policy.hpp")
    memory_policy_source = read("rt/src/memory_policy.cpp")
    thread_policy_header = read("rt/src/thread_policy.hpp")
    thread_policy_source = read("rt/src/thread_policy.cpp")
    profile_header = read("rt/include/rt/profile.hpp")
    profile_source = read("rt/src/runtime_profile.cpp")
    profile_test = read("tests/runtime_profile_tests.cpp")
    profile_consumer = read("tests/package_consumer/profile_consumer.cpp")
    runtime_demo = read("src/runtime_profile_demo.cpp")
    profile_doc = read("docs/runtime_profiles.md")
    profile_schema = read("tools/autotune/config.schema.json")
    autotune_spec = read("tools/autotune/spec.yaml")
    autotune_make_config = read("tools/autotune/make_config.py")
    mapping_smoke = read("tools/autotune/mapping_smoke.py")
    mapping_workflow = read(".github/workflows/autotune-mapping.yml")
    c_header = read("rt/include/rt/c_api.h")
    roadmap = read("docs/roadmap.md")
    host_doc = read("docs/host_runtime.md")
    graph_doc = read("docs/compiled_graph.md")
    graph_source = read("rt/src/compiled_graph.cpp")
    graph_test = read("tests/test_compiled_graph.cpp")
    rate_header = read("rt/src/rate_timeline.hpp")
    rate_source = read("rt/src/rate_timeline.cpp")
    rate_test = read("tests/test_rate_timeline.cpp")
    cross_rate_header = read("rt/src/cross_rate_data.hpp")
    cross_rate_source = read("rt/src/cross_rate_data.cpp")
    cross_rate_test = read("tests/test_cross_rate_data.cpp")
    rate_dispatch_header = read("rt/src/rate_dispatch.hpp")
    rate_dispatch_source = read("rt/src/rate_dispatch.cpp")
    rate_dispatch_test = read("tests/test_rate_dispatch.cpp")
    rate_telemetry_header = read("rt/src/rate_telemetry.hpp")
    rate_telemetry_source = read("rt/src/rate_telemetry.cpp")
    rate_telemetry_test = read("tests/test_rate_telemetry.cpp")
    rate_telemetry_doc = read("docs/rate_telemetry.md")
    executor_doc = read("docs/executor.md")
    memory_doc = read("docs/memory_plan.md")
    cpu_memory_policy_doc = read("docs/cpu_memory_policy.md")
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
    hal_v2_header = read("rt/src/hal_v2.hpp")
    hal_v2_source = read("rt/src/hal_v2.cpp")
    device_manager_header = read("rt/src/device_manager.hpp")
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
    cpu_memory_policy_test = read("tests/test_cpu_memory_policy.cpp")
    memory_policy_test = read("tests/test_memory_policy.cpp")
    thread_policy_test = read("tests/test_thread_policy.cpp")
    periodic_test = read("tests/test_periodic_runtime.cpp")
    preflight_test = read("tests/test_platform_preflight.cpp")
    observability_test = read("tests/test_observability.cpp")
    determinism_test = read("tests/test_determinism_replay.cpp")
    device_test = read("tests/test_device_runtime.cpp")
    hal_v2_test = read("tests/test_hal_v2.cpp")
    cuda_test = read("tests/test_cuda_backend.cpp")
    xdma_test = read("tests/xdma_backend_tests.cpp")
    host_adapter_test = read("tests/host_adapter_tests.cpp")
    add_subdirectory_consumer = read(
        "tests/add_subdirectory_consumer/CMakeLists.txt"
    )
    package_consumer = read("tests/package_consumer/CMakeLists.txt")
    package_cpp_consumer = read("tests/package_consumer/cpp_consumer.cpp")
    package_compat_consumer = read(
        "tests/package_consumer/compat_consumer.cpp"
    )
    package_contract = read("tests/package_consumer/package_contract.cmake")
    package_config = read("cmake/rtfwConfig.cmake.in")
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
    hal_v2_doc = read("docs/hal_v2.md")

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
        if status not in status_header:
            fail(f"rt/include/rt/status.hpp: missing M2 status {status!r}")

    for status in ("platform_preflight_failed", "clock_failure"):
        if status not in status_header:
            fail(f"rt/include/rt/status.hpp: missing M5 status {status!r}")

    for status in ("invalid_artifact", "incompatible_artifact"):
        if status not in status_header:
            fail(f"rt/include/rt/status.hpp: missing M7 status {status!r}")

    for status in (
        "device_queue_full",
        "device_timeout",
        "device_error",
        "device_lost",
        "device_canceled",
        "device_reset_required",
    ):
        if status not in status_header or status.upper() not in c_header:
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
        or "| M13 | Complete |" not in roadmap
        or "| M14 | Complete |" not in roadmap
    ):
        fail("docs/roadmap.md: M1-M14 milestone status is not advanced")

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

    normalized_cpu_memory_policy_doc = " ".join(
        cpu_memory_policy_doc.lower().split()
    )
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
        "thread_role_frame",
        "thread_role_executor_worker",
        "thread_role_watchdog",
        "thread_role_device_service",
        "thread_role_xdma_io",
        "struct CpuSetRequest",
        "enum class SchedulingClass",
        "enum class WaitStrategy",
        "struct ThreadPolicy",
        "struct MemoryPolicy",
        "struct CpuMemoryPolicy",
    ):
        if token not in config_header:
            fail(f"rt/config.hpp: missing M15-01 policy token {token!r}")
    for token in (
        "memory_region_runtime_control",
        "memory_region_executor_control",
        "memory_region_device_control",
        "memory_region_phase_scratch",
        "memory_region_task_scratch",
        "memory_region_trace_storage",
        "memory_region_registered_state",
        "memory_region_backend_control",
        "memory_region_registered_device_buffer",
        "memory_region_runtime_thread_stack",
        "memory_region_external_thread_stack",
        "memory_region_host_provider",
    ):
        if token not in config_header:
            fail(f"rt/config.hpp: missing M15-01 memory identity {token!r}")
    for token in (
        "cpu_memory_policy_schema_version = 1",
        "struct ResourceAccountingKey",
        "struct ThreadPolicyReport",
        "struct MemoryPolicyReport",
        "struct CpuMemoryPolicyReport",
        "Status set_cpu_memory_policy(",
        "bool cpu_memory_policy_report(",
    ):
        if token not in runtime_header:
            fail(f"rt/runtime.hpp: missing M15-01 report API {token!r}")
    for token in (
        "build_cpu_memory_policy_report(",
        "planned_sum != memory_plan.planned_bytes",
        "PolicyResolutionState::unsupported_best_effort",
        "PolicyApplicationMode::verify_only",
        "thread_role_custom_first",
    ):
        if (
            token not in resource_policy_source
            and token not in resource_policy_header
        ):
            fail(f"M15-01 policy implementation is missing {token!r}")
    for forbidden in (
        "mlock(",
        "mlockall(",
        "pthread_setaffinity_np(",
        "sched_setaffinity(",
        "sched_setscheduler(",
        "mbind(",
        "set_mempolicy(",
    ):
        if forbidden in resource_policy_source:
            fail(
                "rt/src/resource_policy.cpp: portable M15-01 model contains "
                f"native mutation {forbidden!r}"
            )
    for phrase in (
        "fail-closed startup transaction",
        "exactly one row for each stable category",
        "externally owned and verify-only",
        "named-host Linux native functional application/readback only",
        "RT1",
        "RT2",
    ):
        if phrase.lower() not in normalized_cpu_memory_policy_doc:
            fail(f"docs/cpu_memory_policy.md: missing M15-01 phrase {phrase!r}")
    for test_name in (
        "DefaultsInventoryEveryStableRoleAndMemoryIdentity",
        "BestEffortRequestsResolveToExplicitPortableNoops",
        "RejectsDuplicateMalformedAndContradictoryRequests",
        "RejectsUnsupportedStrictAndCheckedArithmeticOverflow",
        "FailedFinalizationCanReplacePolicyAndRecover",
        "ExternalHostAndCustomRolesRemainVerifyOnly",
        "CustomUnknownCardinalityPropagatesToExternalStacks",
        "CpuMemoryPolicyTwoRuntimeReportsAreIsolated",
    ):
        if test_name not in cpu_memory_policy_test:
            fail(f"tests/test_cpu_memory_policy.cpp: missing M15-01 gate {test_name!r}")
    for token in (
        "rt/src/resource_policy.cpp",
        "test_cpu_memory_policy.cpp",
    ):
        if token not in cmake and token not in tests_cmake:
            fail(f"CMake M15-01 integration is missing {token!r}")

    for token in (
        "class ThreadStartupGate",
        "class ThreadPolicyProvider",
        "class NativeThread",
        "aggregate_thread_startup_results(",
    ):
        if token not in thread_policy_header and token not in thread_policy_source:
            fail(f"M15-02 thread policy implementation is missing {token!r}")
    for token in (
        "pthread_setaffinity_np(",
        "pthread_setschedparam(",
        "pthread_setname_np(",
        "pthread_getaffinity_np(",
        "pthread_getschedparam(",
        "pthread_getattr_np(",
        "PolicyOperationState::mismatched",
    ):
        if token not in thread_policy_source:
            fail(f"M15-02 native apply/readback is missing {token!r}")
    for token in (
        "thread_startup_gate.commit()",
        "thread_startup_gate.abort()",
        "failed to initialize device backends after thread-policy verification",
    ):
        if token not in runtime_source:
            fail(f"M15-02 startup transaction is missing {token!r}")
    for test_name in (
        "AppliesEveryRuntimeOwnedInstanceBeforeCommitAndJoinsReverse",
        "StrictFailuresRollbackWithoutCallbacksAndCanRecover",
        "BestEffortFailureRemainsObservableAndContinues",
        "SpinYieldAndParkWakeForWorkAndStop",
        "StrictExternalAndUnavailableRequestsFailDuringFinalize",
        "LinuxAppliesAndReadsBackAvailableNativeFields",
    ):
        if test_name not in thread_policy_test:
            fail(f"tests/test_thread_policy.cpp: missing M15-02 gate {test_name!r}")
    for token in (
        "rt/src/thread_policy.cpp",
        "test_thread_policy.cpp",
    ):
        if token not in cmake and token not in tests_cmake:
            fail(f"CMake M15-02 integration is missing {token!r}")

    for token in (
        "memory_provider_api_version = 1",
        "struct MemoryProviderAcquireRequest",
        "struct MemoryProviderAllocation",
        "struct MemoryProviderObservation",
        "struct MemoryProvider",
    ):
        if token not in config_header:
            fail(f"rt/config.hpp: missing permanent M15-03 provider token {token!r}")
    for token in (
        "Status set_memory_provider(",
        "actual_guard_bytes_before",
        "actual_guard_bytes_after",
        "used_explicit_huge_pages",
        "rollback_error",
    ):
        if token not in runtime_header:
            fail(f"rt/runtime.hpp: missing permanent M15-03 report token {token!r}")
    for token in (
        "class ResidentRegionSet",
        "Status acquire(",
        "Status apply_and_verify(",
        "bool rollback(",
        "void release(",
    ):
        if token not in memory_policy_header:
            fail(f"rt/src/memory_policy.hpp: missing M15-03 transaction token {token!r}")
    for token in (
        "memory_region_phase_scratch",
        "memory_region_task_scratch",
        "memory_region_trace_storage",
        "ResidentRegionSet::acquire(",
        "ResidentRegionSet::apply_and_verify(",
        "ResidentRegionSet::rollback(",
        "ResidentRegionSet::release(",
        "::mlock(",
        "::mincore(",
        "MAP_HUGETLB",
    ):
        if token not in memory_policy_source:
            fail(f"rt/src/memory_policy.cpp: missing M15-03 implementation token {token!r}")
    for token in (
        "resident_regions->apply_and_verify(",
        "resident_regions->rollback(",
        "resident_regions->release(",
    ):
        if token not in runtime_source:
            fail(f"rt/src/host_runtime.cpp: missing M15-03 lifecycle token {token!r}")
    for test_name in (
        "ProviderTableValidatesCopiesAndRejectsReentrancy",
        "AcquiresStableOrderAndReleasesReverseExactlyOnce",
        "ReleaseCallbackCannotReenterRuntime",
        "RollbackFailureRetainsTokensUntilCheckedStopRetry",
        "AcquisitionFailureReleasesCompletedTokensInReverse",
        "FailedAcquireCannotReleaseAnotherRuntimesLiveToken",
        "RejectsMalformedProviderAllocationsWithoutPublishing",
        "RejectsMalformedPageAndHugeOutcomes",
        "RejectsGuardClaimsOutsideAllocationExtent",
        "RejectsCapabilityMismatchBeforePublishing",
        "InactiveRowsDoNotInvokeProvider",
        "StrictDeferredRegionFailsBeforeProviderCallbacks",
        "NativeNumaRequiresIndependentProviderObservation",
        "ReportsRequestedCommittedAndVerifiedProviderState",
        "StrictApplyAndReadbackFailuresRollbackAndRetry",
        "LaterThreadFailureRollsBackThreadsThenMemoryAndRetries",
        "WatchdogFailureJoinsBeforeMemoryRollbackAndRetries",
        "BestEffortApplyFailureIsReportedAndRuntimeContinues",
        "RuntimeInstancesIsolateTokensBackingReportsAndRollback",
        "RejectsHugeFallbackWhenRequestForbidsIt",
        "SharedProviderCannotAliasTwoRuntimes",
        "ProviderBackedNestedAndHostAdapterScratchRemainDistinct",
        "ProviderBackedPeriodicWatchdogPreservesTraceLoss",
        "FailedStartDestructionRollsBackBeforeRelease",
        "NativeLinuxPageBackingIsRoundedGuardedAndObserved",
        "NativeLockingUsesIsolatedPagesAndNeverReportsPinning",
    ):
        if test_name not in memory_policy_test:
            fail(f"tests/test_memory_policy.cpp: missing permanent M15-03 gate {test_name!r}")
    if "ReportRetainsPreM15_03AggregatePrefix" not in cpu_memory_policy_test:
        fail("tests/test_cpu_memory_policy.cpp: missing C++ aggregate-prefix compatibility gate")
    for token in (
        "CleanupMemoryProvider",
        "FailedRegistrationRollbackRemainsRecoverable",
        "DeviceServicePolicyFailureRollsBackMemoryAfterJoinAndRetries",
        "memory_provider.release_count",
    ):
        if token not in device_test:
            fail(f"tests/test_device_runtime.cpp: missing M15-03 cleanup gate {token!r}")
    for token in (
        "rt/src/memory_policy.cpp",
        "test_memory_policy.cpp",
    ):
        if token not in cmake and token not in tests_cmake:
            fail(f"CMake M15-03 integration is missing {token!r}")
    if "MemoryPolicy.*" not in ci_workflow:
        fail(".github/workflows/ci.yml: TSan filter is missing MemoryPolicy.*")

    # M15-04 closure checks are permanent product facts. They intentionally do
    # not inspect the active batch or milestone frontier, which later batches
    # are expected to advance.
    for token in (
        "resource_accounting_declaration_capacity = 32",
        "thread_resource_accounting_key(",
        "memory_resource_accounting_key(",
        "struct ResourceAccountingDeclaration",
        "accounting_declaration_count",
        "accounting_declarations",
    ):
        if token not in config_header:
            fail(f"rt/config.hpp: missing permanent M15-04 declaration token {token!r}")
    for token in (
        "enum class ResourceAccountingExactness",
        "struct MemoryAccountingTotal",
        "declared_accounted_bytes",
        "accounting_exactness",
        "planned_total",
        "informational_total",
        "excluded_total",
        "closed_total",
        "accounting_complete",
    ):
        if token not in runtime_header:
            fail(f"rt/runtime.hpp: missing permanent M15-04 report token {token!r}")
    for token in (
        "struct LogicalControlExtent",
        "struct ControlExtentLedger",
        "validate_control_extent_ledger(",
        "refresh_accounting_totals(",
        "validate_accounting_closure(",
    ):
        if token not in resource_policy_header and token not in resource_policy_source:
            fail(f"M15-04 accounting implementation is missing {token!r}")
    for token in (
        "cleanup_stack_current(",
        "cleanup_and_join()",
        "wait_quiescent()",
        "aggregate_runtime_stack_startup_results(",
    ):
        if token not in thread_policy_header and token not in thread_policy_source:
            fail(f"M15-04 stack lifecycle implementation is missing {token!r}")
    for token in (
        "append_control_extents(",
        "lane_cleanup_pending",
        "memory_cleanup_pending",
        "strict runtime-stack policy failed live apply or observation",
        "device/thread/stack cleanup failed; retry stop",
    ):
        if token not in runtime_source and token not in executor_source and token not in device_manager:
            fail(f"M15-04 runtime closure is missing {token!r}")
    for test_name, source in (
        ("ExactControlExtentLedgerRejectsMalformedInventories", memory_test),
        ("BoundedDeclarationsCloseExternalFactsWithoutQualification", cpu_memory_policy_test),
        ("RejectsDuplicateOwnedAndContradictoryDeclarations", cpu_memory_policy_test),
        ("LiveRuntimeStackAccountingIsExactBoundedAndIsolated", thread_policy_test),
        ("StrictRuntimeStackMismatchRollsBackAndRecovers", thread_policy_test),
        ("FailedStartCleanupRetryClearsRetainedStackError", thread_policy_test),
        ("BestEffortRuntimeStackFailureIsReportedWithoutCommitBlock", thread_policy_test),
        ("StrictStackLockingCannotClaimIndependentReadback", thread_policy_test),
        ("StrictClosureDoesNotDoubleCountProviderStorage", memory_policy_test),
        ("PartialRollbackRetriesOnlyTheUnresolvedRegion", memory_policy_test),
        ("StartupRetainsFirstErrorWhenRollbackAlsoFails", memory_policy_test),
        ("DeviceStackCleanupFailureDefersRollbackAndRetriesOnOwner", device_test),
        ("UnresolvedStackCleanupDestructionFailsClosed", device_test),
    ):
        if test_name not in source:
            fail(f"M15-04 permanent test gate is missing {test_name!r}")
    for token in (
        "m15_accounting_closure",
        "m15_stack_lifecycle",
        "m15_cross_category_rollback",
    ):
        if token not in tests_cmake or token not in ci_workflow:
            fail(f"M15-04 CTest/TSan gate is missing {token!r}")
    for token in (
        "pre_m15_04_policy",
        "pre_m15_04_report",
        "accounting_declarations",
        "cpu_memory_policy_report(",
    ):
        if token not in package_cpp_consumer:
            fail(f"preferred package consumer is missing M15-04 token {token!r}")
    for token in (
        "pre_m15_04_thread",
        "pre_m15_04_report",
        "accounting_complete",
    ):
        if token not in package_compat_consumer:
            fail(f"compatibility package consumer is missing M15-04 token {token!r}")
    normalized_m15_doc = " ".join(cpu_memory_policy_doc.lower().split())
    for phrase in (
        "exact logical control extents",
        "declared_only",
        "owning quiescent lane",
        "declared bytes are not independent observation",
        "mandatory ci and human review remain m15 completion gates",
    ):
        if phrase not in normalized_m15_doc:
            fail(f"docs/cpu_memory_policy.md: missing M15-04 phrase {phrase!r}")

    # M16-01 reference-plan checks are permanent product facts. They do not
    # inspect the active-batch/latest milestone frontier, which later batches
    # must advance without invalidating this owned behavior.
    for token in (
        "rate_domain_capacity = 64",
        "rate_domain_substep_capacity = 64",
        "reference_release_capacity = 65'536",
        "enum class RateCriticality",
    ):
        if token not in config_header:
            fail(f"rt/config.hpp: missing permanent M16-01 token {token!r}")
    for token in (
        "struct RateDomainHandle",
        "graph_rate_domain_kind_bit",
        "struct RateDomainRegistration",
        "struct RatePhaseBinding",
        "struct CompiledRateDomain",
        "struct CompiledRateBinding",
        "struct ReferenceRelease",
        "register_rate_domain(",
        "bind_phase_to_rate_domain(",
        "reference_release_at(",
        "rate_plan_bytes",
    ):
        if token not in runtime_header and token not in config_header:
            graph_header = read("rt/include/rt/graph.hpp")
            if token not in graph_header:
                fail(f"public SDK headers: missing permanent M16-01 token {token!r}")
    for token in (
        "compile_rate_timeline(",
        "checked_lcm(",
        "reference-release capacity exceeded",
        "compiled phase order is not a permutation",
        "candidate.releases",
    ):
        if token not in rate_header and token not in rate_source:
            fail(f"rate timeline compiler is missing {token!r}")
    for token in (
        "compiled_rate_plan",
        "compute_graph_id",
        "rate_plan_bytes",
        "add_runtime_extent",
    ):
        if token not in runtime_source:
            fail(f"M16-01 runtime integration is missing {token!r}")
    for test_name in (
        "HarmonicAndNonHarmonicTimelineMatchesIndependentGenerator",
        "ValidationIsBoundedOwnedAndTransactional",
        "FailedCompilationHasNoProviderSideEffectsAndCanRetry",
        "CheckedArithmeticRejectsOverflowCapacityAndRecovers",
        "ReferenceReleaseCapacityBoundaryIsInclusive",
        "ExplicitPlanDoesNotChangeHostOrPeriodicDispatch",
        "ExplicitSemanticsChangeIdentityAndNoPlanIdentityIsStable",
        "DirectCompilerRejectsMalformedNamesAndPhaseOrder",
        "CpuAndMockDeviceBindingsRemainInstanceOwned",
        "RateStorageIsExactlyInsideRuntimeControl",
        "RateIdentityRejectsSemanticallyDifferentPlanTransactionally",
    ):
        if test_name not in rate_test:
            fail(f"tests/test_rate_timeline.cpp: missing permanent M16-01 gate {test_name!r}")
    for token in (
        "RatePlanInspectionAndCpuFramesDoNotAllocate",
        "RatePlanCompleteDeviceFramesDoNotAllocate",
    ):
        if token not in noalloc_test:
            fail(f"tests/test_trace_noalloc.cpp: missing M16-01 allocation gate {token!r}")
    for token in (
        "rt/src/rate_timeline.cpp",
        "test_rate_timeline.cpp",
        "m16_rate_timeline",
    ):
        if token not in cmake and token not in tests_cmake:
            fail(f"M16-01 CMake integration is missing {token!r}")
    for token in ("RateTimeline.*", "m16_rate_timeline"):
        if token not in ci_workflow:
            fail(f"M16-01 TSan/CI coverage is missing {token!r}")
    for token in (
        "pre_m16_plan",
        "register_rate_domain(",
        "compiled_rate_domain_at(",
    ):
        if token not in package_cpp_consumer:
            fail(f"preferred package consumer is missing M16-01 token {token!r}")
    if "pre_m16_plan" not in package_compat_consumer:
        fail("compatibility package consumer is missing the pre-M16 aggregate gate")
    normalized_rate_docs = " ".join(
        (graph_doc + " " + memory_doc + " " + time_doc).lower().split()
    )
    for phrase in (
        "[0, lcm(periods))",
        "complete compiled graph",
        "no seventh planned row",
        "does not pace the clock",
    ):
        if phrase not in normalized_rate_docs:
            fail(f"M16-01 component contracts are missing phrase {phrase!r}")

    # M16-02 checks assert permanent owned behavior, not the moving milestone
    # frontier, so later M16 batches may advance without invalidating them.
    for token in (
        "cross_rate_channel_capacity = 256",
        "cross_rate_payload_capacity = 64 * 1024",
        "cross_rate_snapshot_slot_count = 2",
        "cross_rate_selection_capacity = 262'144",
    ):
        if token not in config_header:
            fail(f"rt/config.hpp: missing permanent M16-02 token {token!r}")
    graph_header = read("rt/include/rt/graph.hpp")
    for token in (
        "struct CrossRateChannelHandle",
        "struct CrossRateChannelRegistration",
        "struct CompiledCrossRateChannel",
        "struct CompiledCrossRateSelection",
        "register_cross_rate_channel(",
        "copy_cross_rate_initial_sample(",
        "cross_rate_snapshot_bytes",
    ):
        if token not in runtime_header and token not in graph_header:
            fail(f"public SDK headers: missing permanent M16-02 token {token!r}")
    for token in (
        "class SnapshotStore",
        "SnapshotStoreResult",
        "compile_cross_rate_data(",
        "std::memory_order_release",
        "source_cycle_offset = has_in_cycle ? 0 : -1",
    ):
        if token not in cross_rate_header and token not in cross_rate_source:
            fail(f"cross-rate compiler/store is missing {token!r}")
    for test_name in (
        "PublicModelCopiesAndFreezesWithLifecycleInspectors",
        "SelectionMatchesIndependentCompleteSupercycle",
        "SelectionAgreementCoversRateAndRegistrationPermutations",
        "SameTimestampVisibilityAndFreshnessBoundariesAreExact",
        "ValidationIsOwnedBoundedTransactionalAndCorrectable",
        "AggregateCapacityFailsBeforeProviderAndCanBeCorrected",
        "DeviceEndpointIsRejectedBeforeOwnershipOrExecution",
        "SnapshotStoreIsExactBoundedAndNeverSubstitutes",
        "SnapshotStoreSpscConcurrencyHasDeterministicBytes",
        "SnapshotStoreSpscContentionPublishesCompleteGenerations",
        "CrossRateStorageIsExactlyInsideRateAndRuntimeControl",
        "CrossRatePlanDoesNotChangeHostOrPeriodicDispatch",
        "CrossRateIdentityCoversEverySemanticAndInitialByte",
        "TwoRuntimeHandlesSamplesAndStoresRemainIsolated",
    ):
        if test_name not in cross_rate_test:
            fail(f"tests/test_cross_rate_data.cpp: missing permanent M16-02 gate {test_name!r}")
    for token in (
        "rt/src/cross_rate_data.cpp",
        "test_cross_rate_data.cpp",
        "m16_cross_rate_data",
    ):
        if token not in cmake and token not in tests_cmake:
            fail(f"M16-02 CMake integration is missing {token!r}")
    for token in ("CrossRateData.*", "m16_cross_rate_data"):
        if token not in ci_workflow:
            fail(f"M16-02 TSan/CI coverage is missing {token!r}")
    for token in (
        "register_cross_rate_channel(",
        "compiled_cross_rate_channel_at(",
        "copy_cross_rate_initial_sample(",
    ):
        if token not in package_cpp_consumer:
            fail(f"preferred package consumer is missing M16-02 token {token!r}")
    if "additive_cross_rate" not in package_compat_consumer:
        fail("compatibility package consumer is missing the additive M16-02 gate")
    normalized_cross_docs = " ".join(
        (graph_doc + " " + host_doc + " " + executor_doc + " " +
         memory_doc + " " + time_doc + " " + determinism_doc).lower().split()
    )
    for phrase in (
        "first supercycle",
        "repeating steady state",
        "two-slot",
        "no seventh planned row",
        "does not change cadence",
    ):
        if phrase not in normalized_cross_docs:
            fail(f"M16-02 component contracts are missing phrase {phrase!r}")

    # M16-03 checks likewise assert permanent opt-in behavior and deliberately
    # avoid naming the moving active/latest batch frontier.
    for token in (
        "enum class RateLateAction",
        "struct RateExecutionPolicy",
        "class RateReleaseView",
        "nominal_release_ns",
        "set_rate_execution_policy(",
        "rate_execution_enabled(",
        "rate_dispatch_state_bytes",
        "rate_checkpoint_state_bytes",
    ):
        if token not in runtime_header:
            fail(f"public SDK headers: missing permanent M16-03 token {token!r}")
    for token in (
        "compile_rate_dispatch(",
        "count_due_rate_work(",
        "domain_group_indices",
        "declared-budget admission found an infeasible mandatory record",
    ):
        if token not in rate_dispatch_header and token not in rate_dispatch_source:
            fail(f"active rate compiler is missing {token!r}")
    for token in (
        "run_active_step(",
        "execute_active_group(",
        "publish_active_channel(",
        "copy_active_channel(",
        "kRateDispatchStateName",
        "restore_committed(",
    ):
        if token not in runtime_source:
            fail(f"M16-03 runtime integration is missing {token!r}")
    if "run_selected(" not in executor_source:
        fail("M16-03 executor integration is missing selected-phase dispatch")
    for test_name in (
        "PolicyIsOptInCopiedFrozenAndLegacyDispatchIsExact",
        "AdmissionRejectsMalformedAndInfeasibleActivePlans",
        "AdmissionMatchesIndependentCompleteSupercycleSimulation",
        "RejectsD1CrossDomainDependenciesAndSkipProducers",
        "ExecutesExactHalfOpenWindowsAndReportsContextAndCaps",
        "CallbackFailureRejectsEveryUnattemptedSubstep",
        "AppliesSkipCatchUpDegradeAndFailPolicies",
        "PublishesCopiesHoldsAndRejectsDuplicateOrMissingPayloads",
        "PublishesEverySubstepAndUsesExactHeldSelection",
        "PeriodicUsesAbsoluteReleaseAndAggregatesRateSummaries",
        "TwoActiveRuntimeEpochsAndCursorsRemainIsolated",
        "ActiveCheckpointRoundTripsAndReplayIsExplicitlyRejected",
    ):
        if test_name not in rate_dispatch_test:
            fail(
                "tests/test_rate_dispatch.cpp: missing permanent "
                f"M16-03 gate {test_name!r}"
            )
    if "RateDispatchOnTimeAndLateDegradeDoNotAllocate" not in noalloc_test:
        fail("tests/test_trace_noalloc.cpp: missing M16-03 allocation gate")
    for token in (
        "rt/src/rate_dispatch.cpp",
        "test_rate_dispatch.cpp",
        "m16_rate_dispatch",
    ):
        if token not in cmake and token not in tests_cmake:
            fail(f"M16-03 CMake integration is missing {token!r}")
    for token in ("RateDispatch.*", "m16_rate_dispatch"):
        if token not in ci_workflow:
            fail(f"M16-03 TSan/CI coverage is missing {token!r}")
    if "additive_active_policy" not in package_cpp_consumer:
        fail("preferred package consumer is missing the additive M16-03 gate")
    if "pre_m16_03_compiled" not in package_compat_consumer:
        fail("compatibility package consumer is missing the pre-M16-03 aggregate gate")
    normalized_dispatch_docs = " ".join(
        (graph_doc + " " + host_doc + " " + executor_doc + " " +
         memory_doc + " " + time_doc + " " + determinism_doc).lower().split()
    )
    for phrase in (
        "conservative serialized",
        "exact half-open interval",
        "canonical generic state",
        "m16-04",
    ):
        if phrase not in normalized_dispatch_docs:
            fail(f"M16-03 component contracts are missing phrase {phrase!r}")

    # M16-04 gates also assert permanent owned behavior rather than the moving
    # milestone frontier.
    for token in (
        "rate_action_schema_version = 1",
        "rate_action_counter_count = 20",
        "host_policy_version = 1",
        "consecutive_late_threshold = 1",
        "consecutive_on_time_threshold = 1",
        "struct RateActionRecord",
        "static_assert(sizeof(RateActionRecord) == 160)",
        "struct RateTelemetryCursor",
        "rate_telemetry_metadata(",
        "rate_counters_snapshot(",
        "read_rate_actions(",
    ):
        if token not in runtime_header:
            fail(f"public SDK headers: missing permanent M16-04 token {token!r}")
    for token in (
        "class RateTelemetryRing",
        "class RateCounters",
        "std::memory_order_release",
        "std::memory_order_acquire",
    ):
        if token not in rate_telemetry_header and token not in rate_telemetry_source:
            fail(f"rate-action telemetry is missing {token!r}")
    for token in (
        "active_shedding_state",
        "RateActionId::optional_shed",
        "RateTransitionId::recover",
        "kRateOptionalStateMagic",
        "rate_shedding_state_bytes",
        "rate_telemetry_storage_bytes",
    ):
        if token not in runtime_source and token not in runtime_header:
            fail(f"M16-04 runtime integration is missing {token!r}")
    for test_name in (
        "NumericSchemaAndCounterTablesAreExact",
        "ConcurrentPublishAndReadRemainBoundedAndRaceFree",
        "InspectionRejectsAnActiveStep",
        "RuntimeBoundCursorsRejectForeignInstancesTransactionally",
        "RateTelemetrySummaryAndInspectionBoundaryAreExact",
        "ThresholdTransitionIsImmediateAndRecoveryIsReverse",
        "ZeroCapacityAndOverwriteReportExactCursorGaps",
        "OptionalOrderIsCriticalityThenReverseRegistration",
        "StreakResetAndOptionalLatenessNeverDriveTransitions",
        "TerminalFailPreventsPolicyTransition",
        "LargeLateWindowAggregatesAfterBoundedTransitions",
        "LargeOptionalLatePrefixAggregatesBeforeShedThreshold",
        "PolicyAndOptionalChannelValidationAreTransactional",
        "CheckpointRoundTripRestoresStreakAndShedState",
        "RateTelemetryIsInsideRuntimeControlExactlyOnce",
    ):
        if test_name not in rate_telemetry_test:
            fail(
                "tests/test_rate_telemetry.cpp: missing permanent "
                f"M16-04 gate {test_name!r}"
            )
    if "RateTelemetryShedRecoverInspectAndStopDoNotAllocate" not in noalloc_test:
        fail("tests/test_trace_noalloc.cpp: missing M16-04 allocation gate")
    for token in (
        "rt/src/rate_telemetry.cpp",
        "test_rate_telemetry.cpp",
        "m16_shedding_telemetry",
    ):
        if token not in cmake and token not in tests_cmake:
            fail(f"M16-04 CMake integration is missing {token!r}")
    for token in ("RateShedding.*", "RateTelemetry.*", "m16_shedding_telemetry"):
        if token not in ci_workflow:
            fail(f"M16-04 TSan/CI coverage is missing {token!r}")
    for token in ("RateActionRecord", "rate_action_schema_version"):
        if token not in package_cpp_consumer:
            fail(f"preferred package consumer is missing M16-04 token {token!r}")
    if "pre_m16_04_rate_execution" not in package_compat_consumer:
        fail("compatibility package consumer is missing the pre-M16-04 prefix gate")
    normalized_m16_04_docs = " ".join(
        (graph_doc + " " + host_doc + " " + executor_doc + " " +
         memory_doc + " " + time_doc + " " + observability_doc + " " +
         determinism_doc + " " + rate_telemetry_doc).lower().split()
    )
    for phrase in (
        "mandatory-only admission",
        "reverse registration",
        "fixed 160-byte",
        "zero telemetry capacity",
        "global observability remains schema 2",
        "not checkpointed",
        "six-row",
    ):
        if phrase not in normalized_m16_04_docs:
            fail(f"M16-04 component contracts are missing phrase {phrase!r}")

    # M17-01 gates retain permanent HAL-v2 and device-ABI-v1 compatibility
    # facts. The moving active/latest milestone frontier is intentionally not
    # part of this owned-batch check.
    for token in (
        "hal_v2_api_version = 2u",
        "hal_v2_identifier_capacity",
        "hal_v2_inline_payload_capacity",
        "hal_v2_buffer_ref_capacity",
        "enum class HalV2Status",
        "enum class HalV2HealthState",
        "struct HalV2Capabilities",
        "struct HalV2InitializeConfig",
        "struct HalV2BufferRegistration",
        "struct HalV2BufferReference",
        "struct HalV2Submission",
        "struct HalV2Completion",
        "struct HalV2Health",
        "struct HalV2BackendApi",
        "struct HalV2BackendRegistration",
    ):
        if token not in device_header:
            fail(f"public HAL v2 header is missing permanent token {token!r}")
    if "const HalV2BackendRegistration&" not in runtime_header:
        fail("runtime header is missing the additive HAL v2 registration overload")
    for token in (
        "enum class HalBackendKind",
        "adapted_device_abi_v1",
        "native_hal_v2",
        "validate_device_v1_api",
        "validate_hal_v2_api",
        "validate_hal_v2_capabilities",
        "validate_hal_v2_health",
        "validate_hal_v2_completion",
        "hal_v2_status_to_runtime",
        "class DeviceV1CompatibilityAdapter",
        "prepare_completion_storage",
    ):
        if token not in hal_v2_header and token not in hal_v2_source:
            fail(f"HAL v2 compatibility core is missing {token!r}")
    for token in (
        "HalV2BackendApi",
        "HalV2Capabilities",
        "HalV2Completion",
        "v1_adapter",
        "estimate_control_storage",
        "append_control_extents",
    ):
        if token not in device_manager_header and token not in device_manager:
            fail(f"canonical HAL v2 device manager is missing {token!r}")
    for token in (
        "device_v1_adapters",
        "HalBackendKind::adapted_device_abi_v1",
        "HalBackendKind::native_hal_v2",
        "prepare_completion_storage",
        "hal_v2_api_version",
    ):
        if token not in runtime_source:
            fail(f"HAL v2 runtime integration is missing {token!r}")
    for test_name in (
        "ApiVersionDefaultsAndLayoutsAreExact",
        "NativeRegistrationAndOperationTranslationAreExact",
        "MalformedTablesAndCapabilitiesFailTransactionally",
        "NativeAndV1StatusesAndExceptionsAreEquivalent",
        "DeviceAbiV1AdapterPreservesEveryCoreFieldAndFailsClosed",
        "NativeIdentityIsSeparateAndV1IdentityRemainsStable",
        "AdaptedV1StorageIsExactInsideSixRowMemoryPlan",
        "AdaptedV1StorageSurvivesConfiguringGrowthAndIsIsolated",
    ):
        if test_name not in hal_v2_test:
            fail(
                "tests/test_hal_v2.cpp: missing permanent "
                f"M17-01 gate {test_name!r}"
            )
    for token in (
        "rt/src/hal_v2.cpp",
        "test_hal_v2.cpp",
        "m17_hal_v2_compatibility",
    ):
        if token not in cmake and token not in tests_cmake:
            fail(f"M17-01 CMake integration is missing {token!r}")
    for token in ("HalV2.*", "m17_hal_v2_compatibility"):
        if token not in ci_workflow:
            fail(f"M17-01 TSan/CI coverage is missing {token!r}")
    for token in ("InstalledHalV2Backend", "HalV2BackendRegistration"):
        if token not in package_cpp_consumer:
            fail(f"preferred package consumer is missing M17-01 token {token!r}")
    for token in ("pre_m17_device_backend", "pre_m17_device_buffer"):
        if token not in package_compat_consumer:
            fail(f"compatibility package consumer is missing {token!r}")
    for consumer, backend in (
        (read("tests/package_consumer/cuda_consumer.cpp"), "CUDA"),
        (read("tests/package_consumer/xdma_consumer.cpp"), "XDMA"),
    ):
        if "DeviceBackendRegistration" not in consumer:
            fail(f"{backend} package consumer no longer covers device ABI v1")
    normalized_hal_docs = " ".join(
        (
            hal_v2_doc
            + " "
            + device_doc
            + " "
            + memory_doc
            + " "
            + determinism_doc
            + " "
            + host_doc
            + " "
            + executor_doc
            + " "
            + observability_doc
            + " "
            + c_abi_doc
        ).lower().split()
    )
    for phrase in (
        "canonical hal v2",
        "device-abi-v1",
        "exact pre-m17",
        "six-row",
        "global schema 2",
        "no c++ binary abi promise",
        "no submission or i/o lane",
    ):
        if phrase not in normalized_hal_docs:
            fail(f"M17-01 component contracts are missing phrase {phrase!r}")

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
        "runtime",
        "cpp_runtime",
        "cuda_backend",
        "xdma_backend",
        "rtfw::c_shared",
        "rtfw::c_static",
        "rtfw::runtime",
        "rtfw::rtfw",
        "rtfw::rtfw_static",
        "rtfw::simcore_rt",
        "rtfw::cuda_backend",
        "rtfw::xdma_backend",
    ):
        if token not in package_consumer:
            fail(f"package consumer is missing {token!r}")

    for token in (
        "add_library(rtfw::runtime ALIAS rtfw_runtime)",
        "add_library(rtfw::rtfw ALIAS rtfw_shared)",
        "add_library(rtfw::rtfw_static ALIAS rtfw_static)",
        "add_library(rtfw::simcore_rt ALIAS simcore_rt)",
        "EXPORT_NAME runtime",
        "RTFW_BUILD_TESTS",
        "RTFW_BUILD_EXPERIMENTAL",
        "RTFW_INSTALL_EXPERIMENTAL",
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>",
        "rtfwCudaTargets",
        "rtfwXdmaTargets",
        "rt/include/rt/canonical_bytes.hpp",
        "rt/include/rt/config.hpp",
        "rt/include/rt/status.hpp",
    ):
        if token not in cmake:
            fail(f"CMakeLists.txt: missing M14 package boundary {token!r}")
    for forbidden in (
        "target_compile_options(simcore INTERFACE -Wall",
        "target_compile_options(simcore INTERFACE -Werror",
        "target_compile_definitions(simcore INTERFACE -DLOG_ENABLED",
        "target_link_libraries(rtfw_static PUBLIC simcore",
        "$<INSTALL_INTERFACE:include>",
    ):
        if forbidden in cmake:
            fail(f"CMakeLists.txt: leaks project policy {forbidden!r}")
    if '#include "rt/runtime.hpp"' not in profile_header:
        fail("rt/profile.hpp dropped its 1.x transitive runtime contract")
    if '#include "rt/config.hpp"' not in profile_header:
        fail("rt/profile.hpp does not import the focused configuration API")
    for token in (
        "enum class Status",
        "status_message(",
    ):
        if token not in status_header:
            fail(f"rt/status.hpp: missing M14 API token {token!r}")
    for token in (
        "struct RuntimeConfig",
        "set_runtime_config_value(",
        "runtime_config_schema_version = 7",
    ):
        if token not in config_header:
            fail(f"rt/config.hpp: missing M14 API token {token!r}")
    if 'rt/include/rt/config.hpp' not in autotune_make_config:
        fail("autotune config generator does not read the canonical config API")
    if "runtime.hpp does not declare a config schema" in autotune_make_config:
        fail("autotune config generator still scans the compatibility facade")
    for token in (
        "store_u32_le(",
        "store_u64_le(",
        "load_u32_le(",
        "load_u64_le(",
    ):
        if token not in canonical_bytes_header:
            fail(f"rt/canonical_bytes.hpp: missing M14 helper {token!r}")
    for token in (
        "expected_headers",
        "INTERFACE_COMPILE_OPTIONS",
        "INTERFACE_COMPILE_DEFINITIONS",
        "cxx_std_20",
        "RTFW_DATA_DIR",
        "rtfw::cuda_driver",
        "rtfw::xdma_linux",
        "Installed Apache-2.0 license digest changed",
    ):
        if token not in package_contract:
            fail(f"package boundary test is missing {token!r}")
    for token in (
        "Relocated package consumer",
        "check_c_abi.py --library",
        "rtfw-relocated",
        "tests/add_subdirectory_consumer",
        "RTFW_EXPECT_TESTS=ON",
        "CMAKE_INSTALL_INCLUDEDIR=sdk/include",
        "CMAKE_INSTALL_DATADIR=sdk/data",
    ):
        if token not in ci_workflow:
            fail(f".github/workflows/ci.yml: missing M11 gate {token!r}")

    for token in (
        "set(ENABLE_TESTS ON",
        "rtfw::rtfw",
        "rtfw::rtfw_static",
        "rtfw::simcore_rt",
        "RTFW_EXPECT_TESTS",
        "test_cabi_dlopen",
        "package_source",
        "simcore_tests",
    ):
        if token not in add_subdirectory_consumer:
            fail(f"add_subdirectory consumer is missing {token!r}")
    for token in (
        "RTFW_DATA_DIR",
        "rtfw_FIND_REQUIRED_cuda_driver",
        "find_package(CUDAToolkit QUIET)",
    ):
        if token not in package_config:
            fail(f"package config is missing {token!r}")
    if "RTFW_TEST_CUDA_DRIVER=ON" not in cuda_workflow:
        fail("CUDA workflow does not consume the installed cuda_driver component")
    if "RTFW_TEST_XDMA_LINUX=ON" not in xdma_workflow:
        fail("XDMA workflow does not consume the installed xdma_linux component")

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
        "runtime_profile_schema_version = 1",
        "runtime_profile_max_bytes = 64 * 1024",
        "parse_runtime_profile(",
        "RuntimeProfileMetadata",
        "RuntimeProfileErrorCode",
    ):
        if token not in profile_header:
            fail(f"rt/include/rt/profile.hpp: missing M13 token {token!r}")
    for token in (
        "valid_utf8(",
        "kMaximumNesting = 16",
        "class Parser",
        "set_runtime_config_value(",
        "Status::incompatible_artifact",
    ):
        if token not in profile_source:
            fail(f"rt/src/runtime_profile.cpp: missing M13 evidence {token!r}")
    for forbidden in (
        "std::vector<",
        "std::map<",
        "std::unordered_map<",
        "std::basic_string<",
        "operator new",
    ):
        if forbidden in profile_source:
            fail(
                "rt/src/runtime_profile.cpp: profile parser contains "
                f"allocating surface {forbidden!r}"
            )
    for token in (
        "valid_profile_is_transactional_and_allocation_free",
        "malformed_profiles_fail_without_mutation",
        "compatibility_and_diagnostics_are_explicit",
        "mutation_corpus_stays_bounded_and_transactional",
        "input_too_large",
        "allocation_count",
    ):
        if token not in profile_test:
            fail(f"tests/runtime_profile_tests.cpp: missing M13 gate {token!r}")
    for token in (
        "parse_runtime_profile(",
        "run_periodic_frames(",
        "parallel_for(",
        "declare_resource_access(",
        "profile_id",
        "p99_frame_ms",
        "queue_rejections",
        "trace_events_dropped",
        "RuntimeMetricId::trace_events_dropped",
    ):
        if token not in runtime_demo:
            fail(f"src/runtime_profile_demo.cpp: missing M13 evidence {token!r}")
    for phrase in (
        "complete configurations, not overlays",
        "allocation-free",
        "transactional",
        "64 KiB",
        "not authentication",
        "C ABI v8",
    ):
        if phrase.lower() not in profile_doc.lower():
            fail(f"docs/runtime_profiles.md: missing M13 phrase {phrase!r}")
    try:
        profile_schema_json = json.loads(profile_schema)
    except json.JSONDecodeError as exc:
        fail(f"tools/autotune/config.schema.json: invalid JSON: {exc}")
    else:
        profile_properties = profile_schema_json.get("properties", {})
        runtime_schema = (
            profile_properties.get("runtime", {})
            if isinstance(profile_schema_json, dict)
            else {}
        )
        required_runtime_fields = (
            set(runtime_schema.get("required", []))
            if isinstance(runtime_schema, dict)
            else set()
        )
        if required_runtime_fields != schema_keys:
            fail(
                "tools/autotune/config.schema.json: runtime required fields "
                "differ from RuntimeConfig schema"
            )
        if (
            profile_schema_json.get("additionalProperties") is not False
            or runtime_schema.get("additionalProperties") is not False
        ):
            fail("tools/autotune/config.schema.json: profile schema is not closed")
        version_parts = read("VERSION.txt").strip().split(".")
        compatibility_schema = profile_properties.get(
            "runtime_compatibility",
            {},
        )
        compatibility_properties = compatibility_schema.get(
            "properties",
            {},
        )
        if (
            len(version_parts) != 3
            or profile_properties.get("schema_version", {}).get("const") != 1
            or compatibility_properties.get("major", {}).get("const")
            != int(version_parts[0])
            or compatibility_properties.get("minimum_minor", {}).get("maximum")
            != int(version_parts[1])
            or profile_properties.get("runtime_config_schema", {}).get("const")
            != 7
        ):
            fail(
                "tools/autotune/config.schema.json: version/schema "
                "compatibility differs from the runtime contract"
            )
    for token in (
        'path: "../../build/rtfw_runtime_demo"',
        "worker_count:",
        "executor_policy:",
        "executor_queue_capacity:",
        'seed_arg: "--seed"',
    ):
        if token not in autotune_spec:
            fail(f"tools/autotune/spec.yaml: missing M13 token {token!r}")
    for token in (
        "run_round_trip(",
        '"rtfw_runtime_demo profile round trip"',
        "check_mapping_coverage",
        "profile_id",
    ):
        if token not in mapping_smoke:
            fail(f"tools/autotune/mapping_smoke.py: missing M13 gate {token!r}")
    for token in (
        "--target rtfw_runtime_demo",
        "mapping_smoke.py --demo build/rtfw_runtime_demo",
    ):
        if token not in mapping_workflow:
            fail(
                ".github/workflows/autotune-mapping.yml: missing M13 "
                f"round-trip gate {token!r}"
            )
    if "rt::parse_runtime_profile(" not in profile_consumer:
        fail("installed package consumer is missing M13 profile parsing")

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
            "docs/cpu_memory_policy.md",
            "docs/time_platform.md",
            "docs/observability.md",
            "docs/rate_telemetry.md",
            "docs/determinism_replay.md",
            "docs/device_backend.md",
            "docs/hal_v2.md",
            "docs/cuda_backend.md",
            "docs/cuda_support_matrix.json",
            "docs/xdma_backend.md",
            "docs/xdma_support_matrix.json",
            "docs/portable_support_matrix.json",
            "docs/release_policy.md",
            "docs/runtime_profiles.md",
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
            "rt/include/rt/config.hpp",
            "rt/include/rt/status.hpp",
            "rt/include/rt/canonical_bytes.hpp",
            "rt/include/rt/profile.hpp",
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
            "rt/src/rate_dispatch.hpp",
            "rt/src/rate_dispatch.cpp",
            "rt/src/rate_telemetry.hpp",
            "rt/src/rate_telemetry.cpp",
            "rt/src/aligned_storage.hpp",
            "rt/src/executor.cpp",
            "rt/src/host_runtime.cpp",
            "rt/src/memory_policy.hpp",
            "rt/src/memory_policy.cpp",
            "rt/src/resource_policy.hpp",
            "rt/src/resource_policy.cpp",
            "rt/src/runtime_profile.cpp",
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
            "rt/src/hal_v2.hpp",
            "rt/src/hal_v2.cpp",
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
            "tests/test_rate_dispatch.cpp",
            "tests/test_rate_telemetry.cpp",
            "tests/test_compiled_graph.cpp",
            "tests/test_executor.cpp",
            "tests/test_memory_plan.cpp",
            "tests/test_cpu_memory_policy.cpp",
            "tests/test_memory_policy.cpp",
            "tests/test_periodic_runtime.cpp",
            "tests/test_platform_preflight.cpp",
            "tests/test_observability.cpp",
            "tests/test_determinism_replay.cpp",
            "tests/test_device_runtime.cpp",
            "tests/test_hal_v2.cpp",
            "tests/test_release_tools.py",
            "tests/test_cuda_backend.cpp",
            "tests/xdma_backend_tests.cpp",
            "tests/host_adapter_tests.cpp",
            "tests/runtime_profile_tests.cpp",
            "tests/package_consumer/CMakeLists.txt",
            "tests/package_consumer/c_consumer.c",
            "tests/package_consumer/cpp_consumer.cpp",
            "tests/package_consumer/cuda_consumer.cpp",
            "tests/package_consumer/xdma_consumer.cpp",
            "tests/package_consumer/warning_consumer.cpp",
            "tests/package_consumer/package_contract.cmake",
            "tests/package_consumer/profile_consumer.cpp",
            "tests/determinism_artifact.cpp",
            "tests/snapshot_fuzz.cpp",
            "abi/rtfw_c_abi_v8.exports",
            "abi/rtfw_c_abi_v8.sha256",
            "tools/check_c_abi.py",
            "tools/autotune/config.schema.json",
            "tools/autotune/make_config.py",
            "tools/autotune/mapping_smoke.py",
            "tools/autotune/spec.yaml",
            "tools/check_release_contract.py",
            "tools/check_hardware_evidence.py",
            "tools/release_manifest.py",
            "tools/extract_release_archive.py",
            "tools/stage_release_artifacts.py",
            "release/rtfw-release-contract.json",
            "src/runtime_profile_demo.cpp",
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
