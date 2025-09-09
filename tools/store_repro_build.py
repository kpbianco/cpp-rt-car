#!/usr/bin/env python3
"""Store build artifacts with a content hash for reproducibility.

This helper copies the specified build artifact into a destination
folder and records its SHA256 hash in a JSON sidecar file.  The
produced metadata can be used to guarantee reproducible builds and is
intended to be preserved alongside performance artifacts.
"""

import hashlib
import json
import os
import shutil
import sys
from pathlib import Path


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(8192), b''):
            h.update(chunk)
    return h.hexdigest()


def main(argv):
    if len(argv) != 3:
        print('usage: store_repro_build.py <artifact> <dest>', file=sys.stderr)
        return 1
    artifact = Path(argv[1])
    dest = Path(argv[2])
    dest.mkdir(parents=True, exist_ok=True)
    sha = digest(artifact)
    shutil.copy2(artifact, dest / artifact.name)
    meta = {'artifact': artifact.name, 'sha256': sha}
    (dest / 'build.json').write_text(json.dumps(meta, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
