#!/usr/bin/env python3
"""Validate the checked-in portable release and compatibility contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys
from typing import Any


EXPECTED_TUPLES = {
    "ubuntu-22.04-x86_64-gcc-11": {
        "operating_system": "Ubuntu 22.04 LTS",
        "compiler": "GCC 11",
        "ci_runner": "ubuntu-22.04",
        "ci_compiler": "gcc",
    },
    "ubuntu-22.04-x86_64-clang-14": {
        "operating_system": "Ubuntu 22.04 LTS",
        "compiler": "Clang 14",
        "ci_runner": "ubuntu-22.04",
        "ci_compiler": "clang",
    },
    "windows-server-2022-x86_64-msvc-v143": {
        "operating_system": "Windows Server 2022",
        "compiler": "MSVC v143",
        "ci_runner": "windows-2022",
        "ci_compiler": "msvc",
    },
}

PINNED_ACTIONS = {
    "actions/checkout": "11d5960a326750d5838078e36cf38b85af677262",
    "actions/dependency-review-action": (
        "2031cfc080254a8a887f58cffee85186f0e49e48"
    ),
    "actions/download-artifact": (
        "d3f86a106a0bac45b974a628896c90dbdf5c8093"
    ),
    "actions/setup-python": "a26af69be951a213d495a4c3e4e4022e16d87065",
    "actions/upload-artifact": (
        "ea165f8d65b6e75b540449e92b4886f43607fa02"
    ),
    "anchore/sbom-action": "e22c389904149dbc22b58101806040fa8d37a610",
}

HASHED_CONTRACT_PATHS = {
    ".gitattributes",
    ".gitignore",
    ".github/workflows/autotune-mapping.yml",
    ".github/workflows/autotune-smoke.yml",
    ".github/workflows/ci.yml",
    ".github/workflows/cuda-qualification.yml",
    ".github/workflows/docs-contract.yml",
    ".github/workflows/release.yml",
    ".github/workflows/scaling-smoke.yml",
    ".github/workflows/xdma-qualification.yml",
    "CHANGELOG.md",
    "CMakeLists.txt",
    "CMakePresets.json",
    "LICENSE",
    "README.md",
    "SECURITY.md",
    "VERSION.txt",
    "abi/rtfw_c_abi_v8.exports",
    "abi/rtfw_c_abi_v8.sha256",
    "cmake/DeterminismTargets.cmake",
    "cmake/rtfwConfig.cmake.in",
    "configs/README.md",
    "configs/default.json",
    "configs/default_fast.json",
    "configs/default_safe.json",
    "core/include/core/units.hpp",
    "docs/DOE_AUTOTUNE.md",
    "docs/architecture.md",
    "docs/build_tooling.md",
    "docs/c_abi.md",
    "docs/compiled_graph.md",
    "docs/cuda_backend.md",
    "docs/cpu_memory_policy.md",
    "docs/cuda_support_matrix.json",
    "docs/determinism_replay.md",
    "docs/devex_governance.md",
    "docs/device_backend.md",
    "docs/executor.md",
    "docs/host_runtime.md",
    "docs/limp_mode.md",
    "docs/memory_plan.md",
    "docs/observability.md",
    "docs/portable_support_matrix.json",
    "docs/product_contract.md",
    "docs/real_time_hardening.md",
    "docs/real_time_readiness_checklist.md",
    "docs/release_policy.md",
    "docs/roadmap.md",
    "docs/runtime_profiles.md",
    "docs/scheduler.md",
    "docs/security_supply_chain.md",
    "docs/testing_ci.md",
    "docs/threat_model.md",
    "docs/time_platform.md",
    "docs/xdma_backend.md",
    "docs/xdma_support_matrix.json",
    "include/rtfw/version.h",
    "include/simcore/SimCore.hpp",
    "profiles/README.md",
    "profiles/example-linux.json",
    "rt/include/rt/c_api.h",
    "rt/include/rt/canonical_bytes.hpp",
    "rt/include/rt/config.hpp",
    "rt/include/rt/cuda_backend.hpp",
    "rt/include/rt/cuda_driver.hpp",
    "rt/include/rt/device.hpp",
    "rt/include/rt/device_abi.h",
    "rt/include/rt/graph.hpp",
    "rt/include/rt/mock_device.hpp",
    "rt/include/rt/observability_export.hpp",
    "rt/include/rt/profile.hpp",
    "rt/include/rt/runtime.hpp",
    "rt/include/rt/snapshot.hpp",
    "rt/include/rt/status.hpp",
    "rt/include/rt/version.hpp",
    "rt/include/rt/xdma_backend.hpp",
    "rt/include/rt/xdma_linux.hpp",
    "rt/src/host_runtime.cpp",
    "rt/src/cpu_memory_policy.cpp",
    "rt/src/cpu_memory_policy.hpp",
    "rt/src/runtime.cpp",
    "rt/src/runtime_compat.cpp",
    "rt/src/runtime_profile.cpp",
    "rt/src/snapshot_codec.cpp",
    "samples/CMakeLists.txt",
    "samples/embed_cpp/mini_app.cpp",
    "src/runtime_profile_demo.cpp",
    "tests/CMakeLists.txt",
    "tests/add_subdirectory_consumer/CMakeLists.txt",
    "tests/add_subdirectory_consumer/main.cpp",
    "tests/package_consumer/CMakeLists.txt",
    "tests/package_consumer/c_consumer.c",
    "tests/package_consumer/compat_consumer.cpp",
    "tests/package_consumer/cpp_consumer.cpp",
    "tests/package_consumer/cuda_consumer.cpp",
    "tests/package_consumer/cuda_driver_consumer.cpp",
    "tests/package_consumer/package_contract.cmake",
    "tests/package_consumer/profile_consumer.cpp",
    "tests/package_consumer/pure_c/CMakeLists.txt",
    "tests/package_consumer/pure_c/main.c",
    "tests/package_consumer/run_pure_c.cmake",
    "tests/package_consumer/warning_consumer.cpp",
    "tests/package_consumer/xdma_consumer.cpp",
    "tests/package_consumer/xdma_linux_consumer.cpp",
    "tests/determinism_artifact.cpp",
    "tests/test_release_tools.py",
    "tests/test_determinism_replay.cpp",
    "tests/test_cpu_memory_policy.cpp",
    "tests/test_rt_pipeline.cpp",
    "tests/runtime_profile_tests.cpp",
    "tools/autotune/config.schema.json",
    "tools/autotune/install_profile.py",
    "tools/autotune/make_config.py",
    "tools/autotune/mapping_smoke.py",
    "tools/autotune/optimize.py",
    "tools/autotune/report.py",
    "tools/autotune/run_experiments.py",
    "tools/autotune/run_one.py",
    "tools/autotune/spec.yaml",
    "tools/autotune/spec_smoke.yaml",
    "tools/autotune/validate_config_mapping.py",
    "tools/check_c_abi.py",
    "tools/check_docs_contract.py",
    "tools/check_hardware_evidence.py",
    "tools/check_release_contract.py",
    "tools/extract_release_archive.py",
    "tools/release_manifest.py",
    "tools/stage_release_artifacts.py",
    "vcpkg.json",
}


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_text(
    root: pathlib.Path,
    relative: str,
    errors: list[str],
) -> str:
    try:
        return (root / relative).read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        errors.append(f"{relative}: cannot read: {exc}")
        return ""


def load_json(
    root: pathlib.Path,
    relative: str,
    errors: list[str],
) -> dict[str, Any]:
    text = load_text(root, relative, errors)
    if not text:
        return {}
    try:
        value = json.loads(text)
    except json.JSONDecodeError as exc:
        errors.append(f"{relative}: invalid JSON: {exc}")
        return {}
    if not isinstance(value, dict):
        errors.append(f"{relative}: top-level value must be an object")
        return {}
    return value


def semantic_version(value: str) -> tuple[int, int, int] | None:
    match = re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", value)
    if not match:
        return None
    return tuple(int(part) for part in match.groups())


def validate_support_matrix(
    matrix: dict[str, Any],
    version: str,
    errors: list[str],
) -> None:
    if matrix.get("schema_version") != 1:
        errors.append("portable support matrix: schema_version must be 1")
    if matrix.get("runtime_version") != version:
        errors.append("portable support matrix: runtime_version mismatch")
    if matrix.get("contract") != (
        "Portable RT0 functionality only; no latency or hard-real-time claim."
    ):
        errors.append("portable support matrix: RT0 contract mismatch")
    if matrix.get("support_policy") != "docs/release_policy.md":
        errors.append("portable support matrix: support policy path mismatch")

    tuples = matrix.get("supported_tuples")
    if not isinstance(tuples, list):
        errors.append("portable support matrix: supported_tuples must be an array")
        return

    by_id: dict[str, dict[str, Any]] = {}
    for index, item in enumerate(tuples):
        if not isinstance(item, dict):
            errors.append(f"portable support matrix: tuple {index} is not an object")
            continue
        tuple_id = item.get("id")
        if not isinstance(tuple_id, str) or not tuple_id:
            errors.append(f"portable support matrix: tuple {index} has no id")
            continue
        if tuple_id in by_id:
            errors.append(f"portable support matrix: duplicate tuple {tuple_id}")
            continue
        by_id[tuple_id] = item

    if set(by_id) != set(EXPECTED_TUPLES):
        errors.append(
            "portable support matrix: supported tuple set differs from the "
            f"reviewed portable set: {sorted(by_id)}"
        )

    for tuple_id, expected_identity in EXPECTED_TUPLES.items():
        item = by_id.get(tuple_id)
        if not item:
            continue
        required = {
            "status",
            "rt_tier",
            "operating_system",
            "architecture",
            "compiler",
            "cxx_standard",
            "cmake_minimum",
            "ci_runner",
            "ci_compiler",
            "validated_surfaces",
        }
        missing = sorted(required - set(item))
        if missing:
            errors.append(
                f"portable support matrix: {tuple_id} missing {missing}"
            )
        if item.get("status") != "supported":
            errors.append(
                f"portable support matrix: {tuple_id} must be supported"
            )
        if item.get("rt_tier") != "RT0":
            errors.append(
                f"portable support matrix: {tuple_id} may claim only RT0"
            )
        if item.get("architecture") != "x86_64":
            errors.append(
                f"portable support matrix: {tuple_id} architecture mismatch"
            )
        if item.get("cxx_standard") != 20:
            errors.append(
                f"portable support matrix: {tuple_id} must require C++20"
            )
        if item.get("cmake_minimum") != "3.20":
            errors.append(
                f"portable support matrix: {tuple_id} CMake minimum mismatch"
            )
        for field, expected in expected_identity.items():
            if item.get(field) != expected:
                errors.append(
                    f"portable support matrix: {tuple_id} {field} mismatch"
                )
        surfaces = item.get("validated_surfaces")
        required_surfaces = {
            "c_shared",
            "c_static",
            "runtime",
            "cpp_runtime",
            "runtime_profiles",
            "relocated_install",
            "host_adapter",
            "portable_cuda_state_machine",
            "portable_xdma_state_machine",
        }
        if (
            not isinstance(surfaces, list)
            or not all(isinstance(value, str) for value in surfaces)
            or set(surfaces) != required_surfaces
        ):
            errors.append(
                f"portable support matrix: {tuple_id} lacks package surfaces"
            )

    boundary = matrix.get("separate_qualification")
    expected_boundary = {
        "RT1": (
            "Requires retained measurements for the complete deployment; "
            "no portable threshold is published."
        ),
        "RT2": (
            "Requires a reviewed PREEMPT_RT deployment record and "
            "predeclared deadline thresholds."
        ),
        "CUDA": (
            "Candidate backend; only tuples in cuda_support_matrix.json "
            "may be called qualified."
        ),
        "XDMA": (
            "Candidate backend; only tuples in xdma_support_matrix.json "
            "may be called qualified."
        ),
    }
    if boundary != expected_boundary:
        errors.append(
            "portable support matrix: separate qualification boundary is incomplete"
        )

    best_effort = matrix.get("best_effort_platforms")
    best_effort_names = (
        {
            item.get("platform")
            for item in best_effort
            if isinstance(item, dict)
            and isinstance(item.get("platform"), str)
        }
        if isinstance(best_effort, list)
        else set()
    )
    if not isinstance(best_effort, list) or best_effort_names != {
        "Windows 10/11 x86_64 with MSVC v143",
        "Other C++20 Linux distributions and toolchain versions",
        "macOS",
    }:
        errors.append("portable support matrix: best-effort set mismatch")


def validate_hardware_matrix(
    name: str,
    matrix: dict[str, Any],
    version: str,
    backend_id: str,
    required_fields: set[str],
    errors: list[str],
) -> None:
    if matrix.get("schema_version") != 1:
        errors.append(f"{name}: schema_version must be 1")
    if matrix.get("runtime_version") != version:
        errors.append(f"{name}: runtime_version mismatch")
    if matrix.get("backend_id") != backend_id:
        errors.append(f"{name}: backend_id mismatch")
    if matrix.get("status") != "candidate":
        errors.append(f"{name}: status must remain candidate without a record")
    if matrix.get("qualified_tuples") != []:
        errors.append(f"{name}: release contains no reviewed qualified tuple")
    claim = str(matrix.get("claim", "")).lower()
    if "no " not in claim or "qualified" not in claim:
        errors.append(f"{name}: empty-matrix claim must reject qualification")
    fields = matrix.get("required_tuple_fields")
    if (
        not isinstance(fields, list)
        or not all(isinstance(value, str) for value in fields)
        or set(fields) != required_fields
    ):
        errors.append(f"{name}: required_tuple_fields is incomplete")


def validate_release_contract(
    root: pathlib.Path,
    contract: dict[str, Any],
    version: str,
    errors: list[str],
) -> None:
    if contract.get("schema_version") != 2:
        errors.append("release contract: schema_version must be 2")
    if contract.get("project") != "rtfw":
        errors.append("release contract: project must be rtfw")
    if contract.get("release_version") != version:
        errors.append("release contract: release_version mismatch")
    if contract.get("release_status") != "portable_rt0":
        errors.append("release contract: status must be portable_rt0")
    if contract.get("digest_scope") != "source_tree":
        errors.append("release contract: digest_scope must be source_tree")
    if contract.get("license") != "Apache-2.0":
        errors.append("release contract: license must be Apache-2.0")

    c_abi = contract.get("c_abi")
    expected_c_abi = {
        "current": 8,
        "minimum_compatible": 8,
        "soname": 8,
        "layout_fingerprint": "0xd0e7a5a14bf35f97",
        "export_allowlist": "abi/rtfw_c_abi_v8.exports",
    }
    if c_abi != expected_c_abi:
        errors.append("release contract: C ABI v8 identity mismatch")

    cpp_api = contract.get("cpp_api")
    if not isinstance(cpp_api, dict) or cpp_api.get("binary_abi") is not False:
        errors.append("release contract: C++ binary ABI must be explicitly false")
    elif cpp_api != {
        "compatibility": "source_within_1.x",
        "entrypoints": [
            "rt/include/rt/runtime.hpp",
            "rt/include/rt/profile.hpp",
        ],
        "preferred_target": "rtfw::runtime",
        "compatibility_target": "rtfw::simcore_rt",
        "binary_abi": False,
    }:
        errors.append("release contract: C++ source compatibility mismatch")

    package = contract.get("cmake_package")
    always_components = (
        package.get("always_components")
        if isinstance(package, dict)
        else None
    )
    optional_components = (
        package.get("build_optional_components")
        if isinstance(package, dict)
        else None
    )
    required_always_components = {
        "c_shared",
        "c_static",
        "runtime",
        "cpp_runtime",
        "cuda_backend",
        "xdma_backend",
    }
    required_optional_components = {
        "cuda_driver",
        "xdma_linux",
    }
    required_headers = {
        "core/units.hpp",
        "rt/c_api.h",
        "rt/canonical_bytes.hpp",
        "rt/config.hpp",
        "rt/cuda_backend.hpp",
        "rt/device.hpp",
        "rt/device_abi.h",
        "rt/graph.hpp",
        "rt/mock_device.hpp",
        "rt/observability_export.hpp",
        "rt/profile.hpp",
        "rt/runtime.hpp",
        "rt/status.hpp",
        "rt/version.hpp",
        "rt/xdma_backend.hpp",
        "rtfw/version.h",
    }
    required_conditional_headers = {
        "cuda_driver": ["rt/cuda_driver.hpp"],
        "xdma_linux": ["rt/xdma_linux.hpp"],
    }
    required_forbidden_targets = {
        "rtfw::experimental",
        "rtfw::simcore",
        "rtfw::simcore_core",
        "rtfw::simcore_platform",
    }
    required_forbidden_prefixes = {
        "api/",
        "gpu/",
        "hal/",
        "simcore/",
        "rt/arch.hpp",
        "rt/crashdump.hpp",
        "rt/fiber_pool.hpp",
        "rt/fixed_point.hpp",
        "rt/numa.hpp",
        "rt/numerics.hpp",
        "rt/plugin_api.h",
        "rt/plugin_manager.hpp",
        "rt/prng.hpp",
        "rt/scheduler.hpp",
        "rt/snapshot.hpp",
        "rt/watchdog.hpp",
    }
    if (
        not isinstance(package, dict)
        or package.get("compatibility") != "same_major"
        or package.get("preferred_component") != "runtime"
        or package.get("preferred_target") != "rtfw::runtime"
        or package.get("compatibility_component") != "cpp_runtime"
        or package.get("compatibility_target") != "rtfw::simcore_rt"
        or not isinstance(always_components, list)
        or not all(
            isinstance(value, str)
            for value in always_components
        )
        or set(always_components) != required_always_components
        or not isinstance(optional_components, list)
        or not all(
            isinstance(value, str)
            for value in optional_components
        )
        or set(optional_components) != required_optional_components
        or set(package.get("default_headers", [])) != required_headers
        or package.get("conditional_headers") != required_conditional_headers
        or set(package.get("forbidden_default_targets", []))
        != required_forbidden_targets
        or set(package.get("forbidden_default_header_prefixes", []))
        != required_forbidden_prefixes
        or package.get("experimental_built_by_default") is not False
        or package.get("experimental_installed_by_default") is not False
        or package.get("install_optional_components") != ["experimental"]
    ):
        errors.append("release contract: CMake component contract mismatch")

    boundary = contract.get("qualification_boundary")
    if not isinstance(boundary, dict) or boundary != {
        "portable": "RT0 on named supported tuples",
        "RT1": "unqualified",
        "RT2": "unqualified",
        "CUDA": "candidate",
        "XDMA": "candidate",
    }:
        errors.append("release contract: qualification boundary mismatch")

    hashes = contract.get("contract_sha256")
    if not isinstance(hashes, dict):
        errors.append("release contract: contract_sha256 must be an object")
        return
    if set(hashes) != HASHED_CONTRACT_PATHS:
        errors.append(
            "release contract: hashed path set differs from reviewed contract"
        )
        return
    for relative in sorted(HASHED_CONTRACT_PATHS):
        path = root / relative
        if not path.is_file():
            errors.append(f"release contract: missing hashed file {relative}")
            continue
        actual = sha256(path)
        if hashes.get(relative) != actual:
            errors.append(f"release contract: digest mismatch for {relative}")


def validate_repository(root: pathlib.Path) -> list[str]:
    root = root.resolve()
    errors: list[str] = []

    version = load_text(root, "VERSION.txt", errors).strip()
    parsed = semantic_version(version)
    if parsed is None:
        errors.append("VERSION.txt: expected canonical MAJOR.MINOR.PATCH")
    elif parsed[0] < 1:
        errors.append("VERSION.txt: portable release contract requires major >= 1")

    license_path = root / "LICENSE"
    if (
        not license_path.is_file()
        or sha256(license_path)
        != "c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4"
    ):
        errors.append("LICENSE: canonical Apache-2.0 digest mismatch")

    contract = load_json(
        root,
        "release/rtfw-release-contract.json",
        errors,
    )
    matrix = load_json(root, "docs/portable_support_matrix.json", errors)
    cuda = load_json(root, "docs/cuda_support_matrix.json", errors)
    xdma = load_json(root, "docs/xdma_support_matrix.json", errors)

    validate_release_contract(root, contract, version, errors)
    validate_support_matrix(matrix, version, errors)
    validate_hardware_matrix(
        "docs/cuda_support_matrix.json",
        cuda,
        version,
        "rtfw.cuda.driver.v1",
        {
            "gpu",
            "pci_topology",
            "operating_system",
            "kernel",
            "cuda_driver",
            "cuda_toolkit",
            "compiler",
            "runtime_build",
            "power_and_clock_policy",
            "workload",
            "warmup",
            "measurement",
            "thresholds",
            "raw_evidence_sha256",
            "result",
        },
        errors,
    )
    validate_hardware_matrix(
        "docs/xdma_support_matrix.json",
        xdma,
        version,
        "rtfw.xdma.xilinx_linux_aximm.v1",
        {
            "cpu_and_numa",
            "pci_device_and_bdf",
            "pci_link",
            "iommu_policy",
            "operating_system",
            "kernel_and_preempt_rt",
            "xdma_driver_revision",
            "xdma_module_parameters",
            "fpga_part",
            "xdma_ip_configuration",
            "bitstream_sha256",
            "clock_configuration",
            "memory_map",
            "compiler",
            "runtime_build",
            "host_memory_policy",
            "worker_affinity_and_priority",
            "workload",
            "warmup",
            "measurement",
            "thresholds",
            "failure_recovery_evidence",
            "raw_evidence_sha256",
            "result",
        },
        errors,
    )

    header = load_text(root, "include/rtfw/version.h", errors)
    if parsed:
        for name, value in zip(
            ("MAJOR", "MINOR", "PATCH"),
            parsed,
            strict=True,
        ):
            if not re.search(
                rf"^#define RTFW_VERSION_{name} {value}$",
                header,
                re.MULTILINE,
            ):
                errors.append(f"public version header: {name} mismatch")
    if f'#define RTFW_VERSION_STRING "{version}"' not in header:
        errors.append("public version header: string mismatch")

    vcpkg = load_json(root, "vcpkg.json", errors)
    if vcpkg.get("version-string") != version:
        errors.append("vcpkg.json: version-string mismatch")

    cmake = load_text(root, "CMakeLists.txt", errors)
    for token in (
        "SameMajorVersion",
        "include(CPack)",
        "CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF",
        "CPACK_PACKAGE_FILE_NAME",
        "RTFW_PACKAGE_TOOLCHAIN",
        "docs/portable_support_matrix.json",
        "docs/release_policy.md",
        "release/rtfw-release-contract.json",
        "add_library(rtfw::runtime ALIAS rtfw_runtime)",
        "add_library(rtfw::rtfw ALIAS rtfw_shared)",
        "add_library(rtfw::rtfw_static ALIAS rtfw_static)",
        "add_library(rtfw::simcore_rt ALIAS simcore_rt)",
        "EXPORT_NAME runtime",
        "RTFW_BUILD_TESTS",
        "RTFW_BUILD_EXPERIMENTAL",
        "RTFW_INSTALL_EXPERIMENTAL",
        "RTFW_CXX_RUNTIME_LIBRARIES",
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>",
        "if (RTFW_TOP_LEVEL)",
        "rtfwCudaTargets",
        "rtfwXdmaTargets",
    ):
        if token not in cmake:
            errors.append(f"CMakeLists.txt: missing release token {token!r}")
    if "$<INSTALL_INTERFACE:include>" in cmake:
        errors.append(
            "CMakeLists.txt: hard-coded installed include directory escaped "
            "the configurable package layout"
        )

    ci = load_text(root, ".github/workflows/ci.yml", errors)
    if "windows-latest" in ci:
        errors.append("CI: windows-latest is not an immutable support tuple")
    for token in (
        "ubuntu-22.04",
        "windows-2022",
        "compiler: [gcc, clang]",
        "gcc-11",
        "g++-11",
        "clang-14",
        "clang++-14",
        "ubuntu-22.04-x86_64-gcc-11",
        "ubuntu-22.04-x86_64-clang-14",
        "windows-server-2022-x86_64-msvc-v143",
        "-A x64 -T v143",
        "Verify portable release contract",
        "Build release archive",
        "Stage verified release archive",
        "tools/stage_release_artifacts.py",
        "Verify release manifest",
        "Extract verified archive to relocated prefix",
        "tools/extract_release_archive.py",
        "CrossInstanceDeviceStateIsIsolated",
        "tests/add_subdirectory_consumer",
        "RTFW_EXPECT_TESTS=ON",
        "CMAKE_INSTALL_INCLUDEDIR=sdk/include",
        "CMAKE_INSTALL_DATADIR=sdk/data",
    ):
        if token not in ci:
            errors.append(f"CI: missing portable release gate {token!r}")

    workflow_paths = sorted(
        set((root / ".github/workflows").glob("*.yml"))
        | set((root / ".github/workflows").glob("*.yaml"))
    )
    if not workflow_paths:
        errors.append("workflows: no YAML workflows found")
    discovered_workflows = {
        path.relative_to(root).as_posix()
        for path in workflow_paths
    }
    contracted_workflows = {
        path
        for path in HASHED_CONTRACT_PATHS
        if path.startswith(".github/workflows/")
    }
    if discovered_workflows != contracted_workflows:
        errors.append(
            "workflows: checked workflow set differs from release contract"
        )
    for workflow_path in workflow_paths:
        workflow_text = load_text(
            root,
            workflow_path.relative_to(root).as_posix(),
            errors,
        )
        for reference in re.findall(
            r"^\s*(?:-\s*)?uses:\s*([^\s#]+)",
            workflow_text,
            re.MULTILINE,
        ):
            if reference.startswith("./"):
                continue
            if reference.count("@") != 1:
                errors.append(
                    f"{workflow_path.relative_to(root)}: "
                    f"invalid action reference {reference}"
                )
                continue
            action, revision = reference.rsplit("@", 1)
            expected = PINNED_ACTIONS.get(action)
            if (
                expected is None
                or revision != expected
                or re.fullmatch(r"[0-9a-f]{40}", revision) is None
            ):
                errors.append(
                    f"{workflow_path.relative_to(root)}: "
                    f"unreviewed action reference {action}@{revision}"
                )

    release_workflow = load_text(
        root,
        ".github/workflows/release.yml",
        errors,
    )
    for token in (
        "workflow_dispatch",
        "tags:",
        "ubuntu-22.04-x86_64-gcc-11",
        "ubuntu-22.04-x86_64-clang-14",
        "windows-server-2022-x86_64-msvc-v143",
        "-A x64 -T v143",
        "gcc-11",
        "clang-14",
        "tools/stage_release_artifacts.py",
        "tools/extract_release_archive.py",
        "tools/check_release_contract.py",
        "tools/release_manifest.py create",
        "tools/release_manifest.py verify",
        (
            "actions/upload-artifact@"
            "ea165f8d65b6e75b540449e92b4886f43607fa02"
        ),
        (
            "actions/download-artifact@"
            "d3f86a106a0bac45b974a628896c90dbdf5c8093"
        ),
        "Run packaged consumer",
        "if: github.ref_type == 'tag'",
        "contents: write",
        "test \"${#assets[@]}\" -eq 9",
        "gh release create",
        "GH_REPO: ${{ github.repository }}",
        "--verify-tag",
        "--draft",
        "gh release edit",
    ):
        if token not in release_workflow:
            errors.append(f"release workflow: missing token {token!r}")

    stager = load_text(root, "tools/stage_release_artifacts.py", errors)
    for token in (
        "expected exactly one",
        "CPack SHA-256 sidecar does not match",
        "artifact directory must not already exist",
    ):
        if token not in stager:
            errors.append(f"artifact stager: missing gate {token!r}")

    extractor = load_text(root, "tools/extract_release_archive.py", errors)
    for token in (
        "archive contains unsafe path",
        "archive contains unsafe link",
        "expected exactly one staged archive",
        "extraction destination must not already exist",
    ):
        if token not in extractor:
            errors.append(f"archive extractor: missing gate {token!r}")

    for backend in ("cuda", "xdma"):
        workflow = load_text(
            root,
            f".github/workflows/{backend}-qualification.yml",
            errors,
        )
        for token in (
            "tools/check_hardware_evidence.py",
            f"--backend {backend}",
            "tools/release_manifest.py create",
            "tools/release_manifest.py verify",
            "evidence-manifest.json",
            "${{ github.sha }}",
            f"RTFW_TEST_{'CUDA_DRIVER' if backend == 'cuda' else 'XDMA_LINUX'}=ON",
        ):
            if token not in workflow:
                errors.append(
                    f"{backend} workflow: missing evidence gate {token!r}"
                )

    package_consumer = load_text(
        root,
        "tests/package_consumer/CMakeLists.txt",
        errors,
    )
    requested_version = ".".join(version.split(".")[:2])
    if f"rtfw {requested_version} CONFIG REQUIRED" not in package_consumer:
        errors.append(
            "package consumer: does not request the current release contract"
        )
    for token in (
        "c_shared",
        "c_static",
        "runtime",
        "cpp_runtime",
        "rtfw_consumer_profile",
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
        "installed_pure_c_project",
        "RTFW_TEST_CUDA_DRIVER",
        "RTFW_TEST_XDMA_LINUX",
        "rtfw::cuda_driver",
        "rtfw::xdma_linux",
    ):
        if token not in package_consumer:
            errors.append(f"package consumer: missing {token!r}")

    package_config = load_text(
        root,
        "cmake/rtfwConfig.cmake.in",
        errors,
    )
    for token in (
        "rtfw_FIND_REQUIRED_cuda_driver",
        "find_dependency(CUDAToolkit)",
        "find_package(CUDAToolkit QUIET)",
        "if (CUDAToolkit_FOUND AND TARGET CUDA::cuda_driver)",
        "RTFW_DATA_DIR",
    ):
        if token not in package_config:
            errors.append(
                f"package config: missing optional-dependency gate {token!r}"
            )

    package_contract = load_text(
        root,
        "tests/package_consumer/package_contract.cmake",
        errors,
    )
    for token in (
        "expected_headers",
        "INTERFACE_COMPILE_OPTIONS",
        "INTERFACE_COMPILE_DEFINITIONS",
        "cxx_std_20",
        "rtfw::experimental",
        "RTFW_DATA_DIR",
        "rtfw::cuda_driver",
        "rtfw::xdma_linux",
        "Installed Apache-2.0 license digest changed",
        "c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4",
    ):
        if token not in package_contract:
            errors.append(f"package contract: missing {token!r}")

    compatibility_consumer = load_text(
        root,
        "tests/package_consumer/compat_consumer.cpp",
        errors,
    )
    if (
        "&rt::build_demo_pipeline" not in compatibility_consumer
        or "&rt::tick_duration" not in compatibility_consumer
        or "volatile legacy_pipeline_factory" not in compatibility_consumer
    ):
        errors.append(
            "package compatibility consumer: legacy facade is not "
            "optimization-resistant link-probed"
        )

    add_subdirectory_consumer = load_text(
        root,
        "tests/add_subdirectory_consumer/CMakeLists.txt",
        errors,
    )
    for token in (
        "set(ENABLE_TESTS ON",
        "rtfw::runtime",
        "rtfw::rtfw",
        "rtfw::rtfw_static",
        "rtfw::simcore_rt",
        "RTFW_EXPECT_TESTS",
        "test_cabi_dlopen",
        "package_source",
        "simcore_tests",
    ):
        if token not in add_subdirectory_consumer:
            errors.append(
                f"add_subdirectory consumer: missing isolation gate {token!r}"
            )

    changelog = load_text(root, "CHANGELOG.md", errors)
    if f"## {version}" not in changelog:
        errors.append("CHANGELOG.md: current release section is missing")

    readme = load_text(root, "README.md", errors)
    if f"**Status: {version} portable RT0 release.**" not in readme:
        errors.append("README.md: portable release status mismatch")
    for phrase in (
        "No RT2 record exists",
        "Candidate; not hardware-qualified",
        "no C++ binary ABI",
    ):
        if phrase.lower() not in readme.lower():
            errors.append(f"README.md: missing qualification phrase {phrase!r}")

    device_test = load_text(root, "tests/test_device_runtime.cpp", errors)
    if "CrossInstanceDeviceStateIsIsolated" not in device_test:
        errors.append("device tests: missing cross-instance isolation gate")

    return errors


def write_hashes(root: pathlib.Path) -> None:
    contract_path = root / "release/rtfw-release-contract.json"
    contract = json.loads(contract_path.read_text(encoding="utf-8"))
    contract["contract_sha256"] = {
        relative: sha256(root / relative)
        for relative in sorted(HASHED_CONTRACT_PATHS)
    }
    contract_path.write_text(
        json.dumps(contract, indent=2, sort_keys=False) + "\n",
        encoding="utf-8",
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1],
    )
    parser.add_argument(
        "--write-hashes",
        action="store_true",
        help="refresh reviewed contract digests before validation",
    )
    parser.add_argument(
        "--tag",
        help="also require an exact v<version> release tag",
    )
    args = parser.parse_args(argv)
    root = args.root.resolve()

    if args.write_hashes:
        write_hashes(root)

    errors = validate_repository(root)
    if args.tag:
        expected_tag = f"v{load_text(root, 'VERSION.txt', errors).strip()}"
        if args.tag != expected_tag:
            errors.append(
                f"release tag mismatch: expected {expected_tag}, got {args.tag}"
            )
    if errors:
        print("Release contract failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print("Release contract OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
