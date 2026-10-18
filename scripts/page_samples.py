"""The pages' samples of tool results (app/web/public/samples/sample_<view>.json, each a view's slots as it saved
them), made again with the tools as they are now: every result in a sample run again on the inputs it was made with,
an input the tool no longer takes left out. A tool that needs the network keeps its saved result.

    uv run --project app/server python scripts/page_samples.py pricing multileg risk validation numerics
"""

from __future__ import annotations

import asyncio
import inspect
import json
import sys
from pathlib import Path
from time import perf_counter

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "app" / "server"))  # the optra package
from optra.tools import TOOLS  # noqa: E402
from optra.tools.base import envelope  # noqa: E402

SAMPLES = Path(__file__).resolve().parents[1] / "app" / "web" / "public" / "samples"


def remade(data):
    """A tool's result made again by its tool (one that needs the network kept); several (a slot's dict or list of
    them) each; anything else as it is."""
    if isinstance(data, list):
        return [remade(d) for d in data]
    if not isinstance(data, dict):
        return data
    if data.get("tool") not in TOOLS:
        return {k: remade(v) for k, v in data.items()}
    tool = TOOLS[data["tool"]]
    if tool.network:
        return data
    taken = {k: v for k, v in data["input_params"].items() if k in tool.input.model_fields}
    params = tool.input(**taken)
    t0 = perf_counter()
    result = asyncio.run(tool.run(params)) if inspect.iscoroutinefunction(tool.run) else tool.run(params)
    return envelope(tool, params, result, (perf_counter() - t0) * 1000.0)


def again(saved: dict) -> dict:
    """A slot made again; one that did not succeed kept."""
    return {**saved, "data": remade(saved.get("data"))} if saved.get("state") == "ok" else saved


def main(views: list[str]) -> None:
    for view in views:
        path = SAMPLES / f"sample_{view}.json"
        sample = json.loads(path.read_text())
        for part in ("slots", "pre"):
            sample[part] = {name: again(saved) for name, saved in sample.get(part, {}).items()}
        path.write_text(json.dumps(sample, separators=(",", ":")))
        print(f"{path.name}: {', '.join(sample['slots'])}")


if __name__ == "__main__":
    main(sys.argv[1:])
