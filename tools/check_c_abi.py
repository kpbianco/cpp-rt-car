#!/usr/bin/env python3
"""Validate the frozen C ABI surface and optional shared-library exports."""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
C_API = ROOT / "rt/include/rt/c_api.h"
DEVICE_ABI = ROOT / "rt/include/rt/device_abi.h"
EXPORTS = ROOT / "abi/rtfw_c_abi_v8.exports"
DIGEST = ROOT / "abi/rtfw_c_abi_v8.sha256"


def fail(message: str) -> None:
    raise SystemExit(f"C ABI check failed: {message}")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def canonical_header(path: Path) -> str:
    text = strip_comments(path.read_text(encoding="utf-8"))
    kept: list[str] = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if "RTFW_C_ABI_LAYOUT_FINGERPRINT" in stripped:
            continue
        if stripped.startswith("#"):
            if not stripped.startswith("#define"):
                continue
            if "RTFW_API" in stripped or stripped.startswith(
                ("#define RT_VERSION_", "#define RTFW_VERSION_")
            ):
                continue
        kept.append(stripped)
    return re.sub(r"\s+", " ", " ".join(kept)).strip()


def surface_digest() -> str:
    canonical = "\n".join(
        (
            "rt/c_api.h " + canonical_header(C_API),
            "rt/device_abi.h " + canonical_header(DEVICE_ABI),
        )
    )
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def header_fingerprint() -> int:
    text = C_API.read_text(encoding="utf-8")
    match = re.search(
        r"#define\s+RTFW_C_ABI_LAYOUT_FINGERPRINT\s+"
        r"UINT64_C\(0x([0-9A-Fa-f]{16})\)",
        text,
    )
    if not match:
        fail("c_api.h must define a 16-hex-digit layout fingerprint")
    return int(match.group(1), 16)


def declared_exports() -> list[str]:
    text = C_API.read_text(encoding="utf-8")
    declarations = re.findall(
        r"^RTFW_API\s+([^;]+);",
        text,
        flags=re.MULTILINE,
    )
    symbols: list[str] = []
    for declaration in declarations:
        match = re.search(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", declaration)
        if not match:
            fail(f"cannot parse exported declaration: {declaration!r}")
        symbols.append(match.group(1))
    if len(symbols) != len(set(symbols)):
        fail("c_api.h contains duplicate exported declarations")
    return sorted(symbols)


def expected_exports() -> list[str]:
    symbols = [
        line.strip()
        for line in EXPORTS.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if symbols != sorted(symbols):
        fail("export allowlist must be sorted")
    if len(symbols) != len(set(symbols)):
        fail("export allowlist contains duplicates")
    return symbols


def binary_exports(library: Path) -> list[str]:
    nm = shutil.which("nm")
    if not nm:
        fail("nm is required for binary export validation")
    result = subprocess.run(
        [nm, "-D", "--defined-only", str(library)],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        fail(f"nm failed for {library}: {result.stderr.strip()}")
    symbols: set[str] = set()
    version_node = "RTFW_8"
    for line in result.stdout.splitlines():
        parts = line.split()
        if not parts:
            continue
        symbol = parts[-1].split("@", 1)[0]
        if symbol == version_node:
            continue
        symbols.add(symbol)
    return sorted(symbols)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--library",
        type=Path,
        help="optional ELF shared library whose dynamic exports are checked",
    )
    args = parser.parse_args()

    digest = surface_digest()
    expected_digest = DIGEST.read_text(encoding="utf-8").strip()
    if digest != expected_digest:
        fail(
            "public ABI surface digest changed; bump the ABI contract or "
            "update the reviewed v8 manifest"
        )
    expected_fingerprint = int(digest[:16], 16)
    if header_fingerprint() != expected_fingerprint:
        fail("c_api.h fingerprint does not match the reviewed ABI digest")

    expected = expected_exports()
    declared = declared_exports()
    if declared != expected:
        fail(
            "header exports differ from allowlist\n"
            f"  missing: {sorted(set(expected) - set(declared))}\n"
            f"  added: {sorted(set(declared) - set(expected))}"
        )

    if args.library:
        actual = binary_exports(args.library)
        if actual != expected:
            fail(
                "shared-library exports differ from allowlist\n"
                f"  missing: {sorted(set(expected) - set(actual))}\n"
                f"  added: {sorted(set(actual) - set(expected))}"
            )

    print(
        f"C ABI v8 verified: {len(expected)} symbols, "
        f"fingerprint 0x{expected_fingerprint:016x}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
