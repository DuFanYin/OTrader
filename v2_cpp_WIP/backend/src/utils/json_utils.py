from __future__ import annotations

import json
from typing import Iterable

from src.utils.files import FileInfo


def escape_json(value: str) -> str:
    """JSON-escaped string content without surrounding quotes."""
    # json.dumps includes quotes; strip them
    if len(dumped) >= 2 and dumped[0] == '"' and dumped[-1] == '"':
        return dumped[1:-1]
    return dumped


def files_to_json(files: Iterable[FileInfo]) -> str:
    """Serialize FileInfo iterable to JSON array."""
    payload = [
        {
            "name": f.name,
            "path": f.path,
            "type": f.type,
            "size_bytes": f.size_bytes,
        }
        for f in files
    ]
    return json.dumps(payload)


