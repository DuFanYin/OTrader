"""Where the server keeps its files (the tools' run history and caches).

$OPTRA_HOME (default ~/.optra) holds the cached option chains (chains/), the run history (runs.db), the saved
Treasury curve (rates.json) and an optional config.json, for example:
    {"runs": {"max_runs": 5000, "max_days": 7}}
"""

import json
import os
from pathlib import Path


def demo() -> bool:
    """A public demo ($OPTRA_DEMO=1): its engine trades a simulated account on synthetic days, and the page says so."""
    return os.environ.get("OPTRA_DEMO") == "1"


def home() -> Path:
    return Path(os.environ.get("OPTRA_HOME", Path.home() / ".optra"))


def config() -> dict:
    try:
        return json.loads((home() / "config.json").read_text())
    except (OSError, ValueError):
        return {}
