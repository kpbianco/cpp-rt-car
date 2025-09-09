#!/usr/bin/env python3
"""Generate a simple SBOM and verify third-party dependencies.

The script enumerates git submodules and outputs their pinned commit
hashes as a JSON document.  When given an expected signature file, the
hashes are verified to match.
"""

import json
import subprocess
import sys
from pathlib import Path


def submodule_status():
    out = subprocess.check_output([
        'git', 'submodule', 'status', '--recursive'
    ], text=True)
    modules = []
    for line in out.strip().splitlines():
        parts = line.strip().split()
        if len(parts) >= 2:
            commit = parts[0].lstrip('-+')
            path = parts[1]
            modules.append({'path': path, 'commit': commit})
    return modules


def verify(modules, expected_path: Path):
    expected = json.loads(expected_path.read_text())
    exp = expected.get('submodules', {})
    for m in modules:
        if m['path'] in exp and exp[m['path']] != m['commit']:
            raise SystemExit(
                f"mismatch for {m['path']}: {m['commit']} != {exp[m['path']]}")


def main(argv):
    modules = submodule_status()
    if len(argv) > 1:
        verify(modules, Path(argv[1]))
    json.dump({'submodules': modules}, sys.stdout, indent=2)


if __name__ == '__main__':
    main(sys.argv)
