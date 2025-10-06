"""Common IO helpers for autotune tooling."""

from __future__ import annotations

import json
import os
from typing import Mapping, Union

PathLike = Union[str, os.PathLike[str]]


def append_jsonl(path: PathLike, obj: Mapping[str, object], *, sort_keys: bool = False) -> None:
    """Append a JSON object to a JSON Lines file crash-safely."""
    path_str = os.fspath(path)
    directory = os.path.dirname(path_str)
    if directory:
        os.makedirs(directory, exist_ok=True)
    with open(path_str, "a", encoding="utf-8") as fh:
        fh.write(json.dumps(obj, sort_keys=sort_keys))
        fh.write("\n")
        fh.flush()
        os.fsync(fh.fileno())
