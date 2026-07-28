#!/usr/bin/env python3
"""Negative and round-trip tests for the M12 release tooling."""

from __future__ import annotations

import copy
import importlib.util
import io
import json
import pathlib
import shutil
import tarfile
import tempfile
import unittest
import zipfile


ROOT = pathlib.Path(__file__).resolve().parents[1]


def load_module(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {relative}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


release_manifest = load_module(
    "rtfw_release_manifest",
    "tools/release_manifest.py",
)
release_contract = load_module(
    "rtfw_release_contract",
    "tools/check_release_contract.py",
)
hardware_evidence = load_module(
    "rtfw_hardware_evidence",
    "tools/check_hardware_evidence.py",
)
release_staging = load_module(
    "rtfw_release_staging",
    "tools/stage_release_artifacts.py",
)
release_extraction = load_module(
    "rtfw_release_extraction",
    "tools/extract_release_archive.py",
)


class ReleaseManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.artifacts = self.root / "artifacts"
        self.artifacts.mkdir()
        (self.artifacts / "rtfw-linux.tar.gz").write_bytes(b"linux")
        nested = self.artifacts / "windows"
        nested.mkdir()
        (nested / "rtfw-windows.zip").write_bytes(b"windows")
        self.version = self.root / "VERSION.txt"
        self.version.write_text("1.0.0\n", encoding="utf-8")
        self.manifest = self.artifacts / "rtfw-release-manifest.json"
        self.commit = "0123456789abcdef0123456789abcdef01234567"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def create(self) -> None:
        value = release_manifest.create_manifest(
            self.artifacts,
            self.manifest,
            self.version,
            self.commit,
        )
        release_manifest.write_manifest(self.manifest, value)

    def test_round_trip_and_corruption_rejection(self) -> None:
        self.create()
        self.assertEqual(
            release_manifest.verify_manifest(
                self.manifest,
                self.artifacts,
                self.version,
            ),
            [],
        )
        (self.artifacts / "rtfw-linux.tar.gz").write_bytes(b"corrupt")
        errors = release_manifest.verify_manifest(
            self.manifest,
            self.artifacts,
            self.version,
        )
        self.assertTrue(any("digest mismatch" in error for error in errors))

    def test_unlisted_and_unsafe_artifacts_are_rejected(self) -> None:
        self.create()
        (self.artifacts / "unexpected.bin").write_bytes(b"unexpected")
        errors = release_manifest.verify_manifest(
            self.manifest,
            self.artifacts,
            self.version,
        )
        self.assertTrue(any("unlisted" in error for error in errors))

        data = json.loads(self.manifest.read_text(encoding="utf-8"))
        data["artifacts"][0]["path"] = "../escape"
        self.manifest.write_text(
            json.dumps(data),
            encoding="utf-8",
        )
        errors = release_manifest.verify_manifest(
            self.manifest,
            self.artifacts,
            self.version,
        )
        self.assertTrue(any("unsafe path" in error for error in errors))

    def test_partial_commit_and_empty_directory_are_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "40-character"):
            release_manifest.create_manifest(
                self.artifacts,
                self.manifest,
                self.version,
                "deadbeef",
            )
        for path in sorted(self.artifacts.rglob("*"), reverse=True):
            if path.is_file():
                path.unlink()
            else:
                path.rmdir()
        with self.assertRaisesRegex(ValueError, "no release artifacts"):
            release_manifest.create_manifest(
                self.artifacts,
                self.manifest,
                self.version,
                self.commit,
            )

    def test_missing_duplicate_order_and_nonportable_paths_are_rejected(
        self,
    ) -> None:
        self.create()
        data = json.loads(self.manifest.read_text(encoding="utf-8"))

        missing_path = self.artifacts / data["artifacts"][0]["path"]
        missing_path.unlink()
        errors = release_manifest.verify_manifest(
            self.manifest,
            self.artifacts,
            self.version,
        )
        self.assertTrue(any("missing" in error for error in errors))

        missing_path.write_bytes(b"linux")
        duplicate = copy.deepcopy(data)
        duplicate["artifacts"].append(
            copy.deepcopy(duplicate["artifacts"][0])
        )
        self.manifest.write_text(
            json.dumps(duplicate),
            encoding="utf-8",
        )
        errors = release_manifest.verify_manifest(
            self.manifest,
            self.artifacts,
            self.version,
        )
        self.assertTrue(any("duplicate" in error for error in errors))

        reversed_entries = copy.deepcopy(data)
        reversed_entries["artifacts"].reverse()
        self.manifest.write_text(
            json.dumps(reversed_entries),
            encoding="utf-8",
        )
        errors = release_manifest.verify_manifest(
            self.manifest,
            self.artifacts,
            self.version,
        )
        self.assertTrue(any("sorted" in error for error in errors))

        self.assertIsNone(
            release_manifest.safe_relative_path(r"..\escape")
        )
        self.assertIsNone(
            release_manifest.safe_relative_path("control\u0001name")
        )
        self.assertIsNone(
            release_manifest.safe_relative_path("C:/escape")
        )


class ReleaseStagingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.cpack = self.root / "cpack-output"
        self.cpack.mkdir()
        (self.cpack / "_CPack_Packages").mkdir()
        self.archive = (
            self.cpack /
            "rtfw-1.0.0-Linux-x86_64-GNU-11.4.0.tar.gz"
        )
        self.archive.write_bytes(b"archive")
        self.sidecar = self.archive.with_name(
            self.archive.name + ".sha256"
        )
        self.sidecar.write_text(
            f"{release_staging.sha256(self.archive)}  {self.archive.name}\n",
            encoding="utf-8",
        )
        self.version = self.root / "VERSION.txt"
        self.version.write_text("1.0.0\n", encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_stages_only_verified_archive_and_sidecar(self) -> None:
        destination = self.root / "artifacts"
        staged = release_staging.stage(
            self.cpack,
            destination,
            "TGZ",
            self.version,
        )
        self.assertEqual(
            [path.name for path in staged],
            [self.archive.name, self.sidecar.name],
        )
        self.assertEqual(
            sorted(path.name for path in destination.iterdir()),
            sorted((self.archive.name, self.sidecar.name)),
        )

    def test_rejects_corruption_stale_output_and_unexpected_files(self) -> None:
        self.sidecar.write_text("0" * 64 + "\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "does not match"):
            release_staging.stage(
                self.cpack,
                self.root / "corrupt",
                "TGZ",
                self.version,
            )

        self.sidecar.write_text(
            f"{release_staging.sha256(self.archive)}\n",
            encoding="utf-8",
        )
        (self.cpack / "unexpected.txt").write_text(
            "unexpected",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValueError, "unexpected top-level"):
            release_staging.stage(
                self.cpack,
                self.root / "unexpected",
                "TGZ",
                self.version,
            )

        (self.cpack / "unexpected.txt").unlink()
        unexpected_directory = self.cpack / "unexpected-directory"
        unexpected_directory.mkdir()
        with self.assertRaisesRegex(ValueError, "unexpected top-level"):
            release_staging.stage(
                self.cpack,
                self.root / "unexpected-directory-output",
                "TGZ",
                self.version,
            )
        unexpected_directory.rmdir()

        stale = self.root / "stale"
        stale.mkdir()
        with self.assertRaisesRegex(ValueError, "must not already exist"):
            release_staging.stage(
                self.cpack,
                stale,
                "TGZ",
                self.version,
            )


class ReleaseExtractionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    @staticmethod
    def add_tar_file(
        archive: tarfile.TarFile,
        name: str,
        content: bytes,
    ) -> None:
        member = tarfile.TarInfo(name)
        member.size = len(content)
        member.mode = 0o644
        archive.addfile(member, io.BytesIO(content))

    def test_extracts_flat_tgz_with_safe_library_symlink(self) -> None:
        artifacts = self.root / "tgz-artifacts"
        artifacts.mkdir()
        archive_path = artifacts / "rtfw-1.0.0-test.tar.gz"
        with tarfile.open(archive_path, mode="w:gz") as archive:
            self.add_tar_file(
                archive,
                "include/rtfw/version.h",
                b"#define RTFW_VERSION_MAJOR 1\n",
            )
            self.add_tar_file(
                archive,
                "lib/librtfw.so.8",
                b"library",
            )
            link = tarfile.TarInfo("lib/librtfw.so")
            link.type = tarfile.SYMTYPE
            link.linkname = "librtfw.so.8"
            archive.addfile(link)

        destination = self.root / "tgz-prefix"
        release_extraction.extract(artifacts, destination)
        self.assertEqual(
            (destination / "lib/librtfw.so.8").read_bytes(),
            b"library",
        )
        self.assertTrue((destination / "lib/librtfw.so").is_symlink())

    def test_extracts_flat_zip(self) -> None:
        artifacts = self.root / "zip-artifacts"
        artifacts.mkdir()
        archive_path = artifacts / "rtfw-1.0.0-test.zip"
        with zipfile.ZipFile(archive_path, mode="w") as archive:
            archive.writestr(
                "include/rtfw/version.h",
                "#define RTFW_VERSION_MAJOR 1\n",
            )
            archive.writestr(
                "lib/cmake/rtfw/rtfwConfig.cmake",
                "set(RTFW_VERSION 1.0.0)\n",
            )

        destination = self.root / "zip-prefix"
        release_extraction.extract(artifacts, destination)
        self.assertTrue(
            (destination / "lib/cmake/rtfw/rtfwConfig.cmake").is_file()
        )

    def test_rejects_traversal_and_removes_partial_destination(self) -> None:
        artifacts = self.root / "unsafe-artifacts"
        artifacts.mkdir()
        archive_path = artifacts / "rtfw-1.0.0-unsafe.tar.gz"
        with tarfile.open(archive_path, mode="w:gz") as archive:
            self.add_tar_file(archive, "../escape", b"unsafe")

        destination = self.root / "unsafe-prefix"
        with self.assertRaisesRegex(ValueError, "unsafe path"):
            release_extraction.extract(artifacts, destination)
        self.assertFalse(destination.exists())
        self.assertFalse((self.root / "escape").exists())

    def test_rejects_escaping_and_cyclic_links(self) -> None:
        unsafe_artifacts = self.root / "unsafe-link-artifacts"
        unsafe_artifacts.mkdir()
        unsafe_archive = unsafe_artifacts / "rtfw-1.0.0-unsafe-link.tar.gz"
        with tarfile.open(unsafe_archive, mode="w:gz") as archive:
            self.add_tar_file(archive, "lib/target", b"target")
            link = tarfile.TarInfo("lib/link")
            link.type = tarfile.SYMTYPE
            link.linkname = "../../escape"
            archive.addfile(link)
        with self.assertRaisesRegex(ValueError, "unsafe link"):
            release_extraction.extract(
                unsafe_artifacts,
                self.root / "unsafe-link-prefix",
            )

        cyclic_artifacts = self.root / "cyclic-link-artifacts"
        cyclic_artifacts.mkdir()
        cyclic_archive = cyclic_artifacts / "rtfw-1.0.0-cycle.tar.gz"
        with tarfile.open(cyclic_archive, mode="w:gz") as archive:
            first = tarfile.TarInfo("lib/first")
            first.type = tarfile.SYMTYPE
            first.linkname = "second"
            archive.addfile(first)
            second = tarfile.TarInfo("lib/second")
            second.type = tarfile.SYMTYPE
            second.linkname = "first"
            archive.addfile(second)
        with self.assertRaisesRegex(ValueError, "cyclic link"):
            release_extraction.extract(
                cyclic_artifacts,
                self.root / "cyclic-link-prefix",
            )


class ReleaseContractTests(unittest.TestCase):
    def fixture(self) -> pathlib.Path:
        destination = pathlib.Path(self.temporary.name) / "repo"
        required = {
            "VERSION.txt",
            "vcpkg.json",
            "CMakeLists.txt",
            "README.md",
            "CHANGELOG.md",
            "include/rtfw/version.h",
            "docs/portable_support_matrix.json",
            "docs/cuda_support_matrix.json",
            "docs/xdma_support_matrix.json",
            "docs/release_policy.md",
            "release/rtfw-release-contract.json",
            ".github/workflows/ci.yml",
            ".github/workflows/release.yml",
            ".github/workflows/cuda-qualification.yml",
            ".github/workflows/xdma-qualification.yml",
            "tools/extract_release_archive.py",
            "tools/stage_release_artifacts.py",
            "tests/package_consumer/CMakeLists.txt",
            "tests/test_device_runtime.cpp",
        } | release_contract.HASHED_CONTRACT_PATHS
        for relative in sorted(required):
            source = ROOT / relative
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        return destination

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_repository_contract_and_negative_mutations(self) -> None:
        fixture = self.fixture()
        self.assertEqual(release_contract.validate_repository(fixture), [])

        (fixture / "VERSION.txt").write_text("1.0\n", encoding="utf-8")
        errors = release_contract.validate_repository(fixture)
        self.assertTrue(any("MAJOR.MINOR.PATCH" in error for error in errors))

        shutil.copy2(ROOT / "VERSION.txt", fixture / "VERSION.txt")
        matrix_path = fixture / "docs/portable_support_matrix.json"
        matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
        matrix["supported_tuples"][0]["rt_tier"] = "RT2"
        matrix_path.write_text(json.dumps(matrix), encoding="utf-8")
        errors = release_contract.validate_repository(fixture)
        self.assertTrue(any("may claim only RT0" in error for error in errors))

        shutil.copy2(
            ROOT / "docs/portable_support_matrix.json",
            matrix_path,
        )
        header = fixture / "rt/include/rt/c_api.h"
        header.write_text(
            header.read_text(encoding="utf-8") + "\n/* mutation */\n",
            encoding="utf-8",
        )
        errors = release_contract.validate_repository(fixture)
        self.assertTrue(any("digest mismatch" in error for error in errors))

    def test_malformed_nested_contract_values_fail_closed(self) -> None:
        fixture = self.fixture()

        portable_path = fixture / "docs/portable_support_matrix.json"
        portable = json.loads(portable_path.read_text(encoding="utf-8"))
        portable["supported_tuples"][0]["validated_surfaces"] = [{}]
        portable_path.write_text(json.dumps(portable), encoding="utf-8")

        cuda_path = fixture / "docs/cuda_support_matrix.json"
        cuda = json.loads(cuda_path.read_text(encoding="utf-8"))
        cuda["required_tuple_fields"] = [{}]
        cuda_path.write_text(json.dumps(cuda), encoding="utf-8")

        contract_path = fixture / "release/rtfw-release-contract.json"
        contract = json.loads(contract_path.read_text(encoding="utf-8"))
        contract["cmake_package"]["always_components"] = [{}]
        contract_path.write_text(json.dumps(contract), encoding="utf-8")

        errors = release_contract.validate_repository(fixture)
        self.assertTrue(any("lacks package surfaces" in error for error in errors))
        self.assertTrue(
            any("required_tuple_fields is incomplete" in error for error in errors)
        )
        self.assertTrue(
            any("CMake component contract mismatch" in error for error in errors)
        )

    def test_movable_or_uncontracted_workflow_is_rejected(self) -> None:
        fixture = self.fixture()
        workflow_path = fixture / ".github/workflows/ci.yml"
        workflow = workflow_path.read_text(encoding="utf-8")
        workflow_path.write_text(
            workflow.replace(
                (
                    "actions/checkout@"
                    "11d5960a326750d5838078e36cf38b85af677262"
                ),
                "actions/checkout@v4",
                1,
            ),
            encoding="utf-8",
        )
        errors = release_contract.validate_repository(fixture)
        self.assertTrue(
            any("unreviewed action reference" in error for error in errors)
        )

        shutil.copy2(ROOT / ".github/workflows/ci.yml", workflow_path)
        rogue = fixture / ".github/workflows/uncontracted.yaml"
        rogue.write_text(
            "name: uncontracted\non: workflow_dispatch\njobs: {}\n",
            encoding="utf-8",
        )
        errors = release_contract.validate_repository(fixture)
        self.assertTrue(
            any("workflow set differs" in error for error in errors)
        )


class HardwareEvidenceTests(unittest.TestCase):
    @staticmethod
    def evidence(backend: str) -> dict:
        if backend == "cuda":
            labels = ("host_to_device", "kernel", "device_to_host")
            label_key = "stage"
            identity = {
                "backend_id": "rtfw.cuda.driver.v1",
                "os": "linux",
                "device_name": "test gpu",
                "pci_bus_id": "0000:01:00.0",
                "compute_capability": "8.0",
                "cuda_toolkit_version": 12000,
                "cuda_driver_version": 12000,
            }
        else:
            labels = ("h2c", "c2h")
            label_key = "direction"
            identity = {
                "backend_id": "rtfw.xdma.xilinx_linux_aximm.v1",
                "pci_bdf": "0000:01:00.0",
                "driver_id": "test-driver",
                "bitstream_id": "test-bitstream",
                "h2c_path": "/dev/xdma0_h2c_0",
                "c2h_path": "/dev/xdma0_c2h_0",
                "device_offset": 0,
                "transfer_bytes": 4096,
            }
        warmup = 1
        measurement = 2
        samples = []
        for iteration in range(measurement):
            for label in labels:
                samples.append(
                    {
                        "iteration": iteration,
                        label_key: label,
                        "submit_call_ns": 1,
                        "completion_wait_ns": 2,
                        "poll_call_ns": 1,
                        "poll_count": 1,
                    }
                )
        total = (warmup + measurement) * len(labels)
        return {
            "schema_version": 1,
            "runtime_version": "1.0.0",
            "result": "pass",
            "qualification_claim": "evidence_only",
            **identity,
            "warmup_iterations": warmup,
            "measurement_iterations": measurement,
            "health": {
                "submissions": total,
                "completions": total,
                "timeouts": 0,
                "errors": 0,
                "losses": 0,
            },
            "samples": samples,
        }

    def test_cuda_and_xdma_evidence_only_schemas(self) -> None:
        for backend in ("cuda", "xdma"):
            with self.subTest(backend=backend):
                evidence = self.evidence(backend)
                self.assertEqual(
                    hardware_evidence.validate_evidence(
                        evidence,
                        backend,
                        "1.0.0",
                    ),
                    [],
                )

                promoted = copy.deepcopy(evidence)
                promoted["qualification_claim"] = "qualified"
                errors = hardware_evidence.validate_evidence(
                    promoted,
                    backend,
                    "1.0.0",
                )
                self.assertTrue(
                    any("evidence_only" in error for error in errors)
                )

                duplicated = copy.deepcopy(evidence)
                duplicated["samples"][1] = copy.deepcopy(
                    duplicated["samples"][0]
                )
                errors = hardware_evidence.validate_evidence(
                    duplicated,
                    backend,
                    "1.0.0",
                )
                self.assertTrue(any("duplicates" in error for error in errors))

                unhealthy = copy.deepcopy(evidence)
                unhealthy["health"]["errors"] = 1
                errors = hardware_evidence.validate_evidence(
                    unhealthy,
                    backend,
                    "1.0.0",
                )
                self.assertTrue(
                    any("must be zero" in error for error in errors)
                )


if __name__ == "__main__":
    unittest.main()
