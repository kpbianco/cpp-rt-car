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
    if f"**Status: {version} experimental.**" not in readme:
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
        "not production-ready",
        "no hard-real-time",
        "No RT2 record exists yet.",
        "GPU | CPU mock only",
        "XDMA | Planned",
    )
    for text in required_qualifiers:
        if text not in readme:
            fail(f"README.md: missing required qualification: {text!r}")


def check_runtime_contract() -> None:
    cmake = read("CMakeLists.txt")
    samples_cmake = read("samples/CMakeLists.txt")
    runtime_header = read("rt/include/rt/runtime.hpp")
    runtime_source = read("rt/src/host_runtime.cpp")
    c_header = read("rt/include/rt/c_api.h")
    roadmap = read("docs/roadmap.md")
    host_doc = read("docs/host_runtime.md")
    graph_doc = read("docs/compiled_graph.md")
    graph_source = read("rt/src/compiled_graph.cpp")
    graph_test = read("tests/test_compiled_graph.cpp")
    executor_doc = read("docs/executor.md")
    executor_source = read("rt/src/executor.cpp")
    executor_test = read("tests/test_executor.cpp")
    noalloc_test = read("tests/test_trace_noalloc.cpp")

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

    forbidden_host_pacing = ("sleep_for", "sleep_until", "std::thread")
    for token in forbidden_host_pacing:
        if token in runtime_source:
            fail(f"rt/src/host_runtime.cpp: host-driven runtime contains {token!r}")

    schema_keys = {
        "callback_capacity",
        "scratch_bytes",
        "trace_capacity",
        "numerical_mode",
        "executor_policy",
        "worker_count",
        "executor_queue_capacity",
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

    for status in (
        "invalid_handle",
        "graph_cycle",
        "resource_conflict",
    ):
        if status not in runtime_header:
            fail(f"rt/include/rt/runtime.hpp: missing M2 status {status!r}")

    for capability in (
        "compiled_graph",
        "host_driven_time",
        "unified_cpu_executor",
        "bounded_memory_plan",
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
        or "| M4 | Next |" not in roadmap
    ):
        fail("docs/roadmap.md: M3/M4 milestone status is not advanced")

    if not re.search(
        r"return\s*\{\s*true\s*,\s*true\s*,\s*true\s*,\s*false\s*\}\s*;",
        runtime_source,
    ):
        fail(
            "rt/src/host_runtime.cpp: M3 capability tuple is not "
            "true/true/true/false"
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
        "IndependentNestedWorkPassesStaticStress",
        "ThroughputUsesLocalQueuesAndSuccessfulSteals",
    ):
        if test_name not in executor_test:
            fail(f"tests/test_executor.cpp: missing M3 gate {test_name!r}")


def main() -> int:
    require_files(
        (
            "LICENSE",
            "VERSION.txt",
            "include/rtfw/version.h",
            "docs/product_contract.md",
            "docs/host_runtime.md",
            "docs/compiled_graph.md",
            "docs/executor.md",
            "docs/roadmap.md",
            "docs/adr/0001-one-executor-boundary.md",
            "docs/adr/0002-host-driven-time.md",
            "docs/adr/0003-device-backend-boundary.md",
            ".github/workflows/docs-contract.yml",
            "rt/include/rt/runtime.hpp",
            "rt/include/rt/graph.hpp",
            "rt/include/rt/c_api.h",
            "rt/src/compiled_graph.cpp",
            "rt/src/executor.cpp",
            "rt/src/host_runtime.cpp",
            "samples/embed_c/mini_app.c",
            "samples/embed_cpp/mini_app.cpp",
            "tests/test_host_runtime.cpp",
            "tests/test_compiled_graph.cpp",
            "tests/test_executor.cpp",
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
