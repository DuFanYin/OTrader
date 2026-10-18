"""The C++ numerics library (libanalytics_c.so), loaded here: in the processes of numerics.py's pool, never in the
server's own process, so a crash in it costs one computation, not the server. Run as a module, it answers requests on
stdin, each a JSON line {"task", "threads", "buffer", and for a matrix task "matrix": its number of values} and the
task's JSON request on the next line, then the matrix's float64 values as bytes (or the line {} alone for why it cannot
compute), with one JSON line each on stdout. The request and the answer pass through as bytes, never
decoded here.

    python -m optra.infra.analytics_lib"""

from __future__ import annotations

import json
import os
import sys
from ctypes import CDLL, POINTER, byref, c_char, c_char_p, c_double, c_int, c_longlong, create_string_buffer
from pathlib import Path

from optra.version import CONTRACT_VERSION

_lib: CDLL | None = None
_problem = ""


def path() -> Path | None:
    """Where the library is: $OPTRA_ANALYTICS_LIB, else the repository's engine build."""
    repo = Path(__file__).resolve().parents[4]  # the repository (this file is app/server/optra/infra/)
    paths = [repo / "engine" / "build" / "quant" / "analytics" / "libanalytics_c.so"]
    if os.environ.get("OPTRA_ANALYTICS_LIB"):
        paths.insert(0, Path(os.environ["OPTRA_ANALYTICS_LIB"]))
    return next((p for p in paths if p.exists()), None)


def load() -> None:
    """The library into this process (the pool's initializer), or why it cannot be (problem())."""
    global _lib, _problem
    where = path()
    if where is None:
        _problem = "the C++ engine is not built: run ./build.sh r at the repository root"
        return
    lib = CDLL(str(where))
    version = None
    if hasattr(lib, "quant_contract_version"):
        lib.quant_contract_version.restype = c_char_p
        version = lib.quant_contract_version().decode()
    if version != CONTRACT_VERSION:
        _problem = (f"the C++ engine at {where} speaks contract {version or 'v1'}, the server {CONTRACT_VERSION}: "
                    "rebuild it (./build.sh r at the repository root)")
        return
    lib.quant_run_json.argtypes = [c_char_p, c_char_p, POINTER(c_char), c_int, POINTER(c_int)]
    lib.quant_run_json.restype = c_int
    lib.quant_run_matrix.argtypes = [c_char_p, c_char_p, POINTER(c_double), c_longlong, POINTER(c_char), c_int,
                                     POINTER(c_int)]
    lib.quant_run_matrix.restype = c_int
    lib.quant_set_num_threads.argtypes = [c_int]
    _lib = lib


def problem() -> str:
    """Why the library cannot compute here; empty when it can."""
    return _problem


def error(code: str, message: str) -> dict:
    """An answer in the library's one shape (engine/quant/analytics/engine.h), for a task it could not answer."""
    return {"contract_version": CONTRACT_VERSION, "status": "error", "result_summary": {}, "result_details": {},
            "runtime_ms": None, "error": {"code": code, "message": message}}


def _encoded(answer: dict) -> bytes:
    return json.dumps(answer, separators=(",", ":")).encode()


def call(task_type: str, raw: bytes, threads: int, buffer: int, matrix: bytes | None = None) -> bytes:
    """One task of the library on its JSON request (and, for a matrix task, its float64 values), on `threads`
    threads, answered in a buffer of at least `buffer` bytes and the request's size (grown when the library asks for
    more)."""
    if _lib is None:
        return _encoded(error("engine_unavailable", _problem))
    _lib.quant_set_num_threads(max(1, threads))
    cap = max(buffer, 2 * len(raw))  # a batch's answer is rarely over twice its request: no rerun for room
    buf = create_string_buffer(cap)
    written = c_int(0)
    task = task_type.encode()
    if matrix is None:
        def run() -> int:
            return int(_lib.quant_run_json(task, c_char_p(raw), buf, cap, byref(written)))
    else:
        values = (c_double * (len(matrix) // 8)).from_buffer_copy(matrix)

        def run() -> int:
            return int(_lib.quant_run_matrix(task, c_char_p(raw), values, len(values), buf, cap, byref(written)))
    rc = run()
    for _ in range(3):
        if rc != 1:
            break
        # Larger than the buffer: the library said what it needs. The rerun can come out a few bytes longer
        # (timings are part of it), so leave headroom.
        cap = written.value + written.value // 4 + 4096
        buf = create_string_buffer(cap)
        rc = run()
    if rc == 3:
        return _encoded(error("unsupported_task_type", f"unsupported task_type: {task_type}"))
    if rc != 0:
        return _encoded(error(f"engine_rc_{rc}", f"{task_type} failed"))
    return buf.value


def serve() -> None:
    load()
    for line in sys.stdin.buffer:
        header = json.loads(line)
        if "task" in header:
            raw = sys.stdin.buffer.readline().rstrip(b"\n")
            matrix = sys.stdin.buffer.read(8 * header["matrix"]) if "matrix" in header else None
            answer = call(header["task"], raw, header["threads"], header["buffer"], matrix)
        else:
            answer = _encoded({"problem": problem()})
        sys.stdout.buffer.write(answer + b"\n")
        sys.stdout.buffer.flush()


if __name__ == "__main__":
    serve()
