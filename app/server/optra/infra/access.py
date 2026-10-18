"""Who is asking: the line between the agent and the rest.

- The agent: every request by default. It reads, writes research strategies, backtests, registers versions, asks
  for review, runs a version's holdout report; nothing that trades, nothing that counts or judges its own work.
- The person (the owner): a request through Optra's front door (dufanyin.dev/optra/) signed in with a passkey, which
  adds the key (OPTRA_HOME/access.key) as X-Optra-Key; on a development machine, the page scripts/dev.sh starts
  (Vite, on localhost) adds it the same way: there the machine's user is the person. Approves a version into paper, pilot and live, raises a
  family's budget, installs strategies, works the engine (strategies on it, risk limits, the gateway, the database).
- The system: the research workers, which hand back trial results with the same key; and the server's own runs, such
  as a version's holdout report, which reach the holdout inside system().

The key is a file only its Unix user can read. While the agent runs as that user too, the line is the tools': an
agent that reads the file crosses it. Run the agent as a user of its own and the file is the wall.

In the demo (OPTRA_DEMO) every visitor works the simulated engine: its routes are open there."""

from __future__ import annotations

import hmac
import os
import secrets
from contextlib import contextmanager
from contextvars import ContextVar
from pathlib import Path
from typing import Iterator

from fastapi import Request

from optra import settings

HEADER = "x-optra-key"
_trusted: ContextVar[bool] = ContextVar("trusted", default=False)  # the person's request, or the system's own run


def key_file() -> Path:
    return settings.home() / "access.key"


def key() -> str:
    """The key, made the first time (readable by its user only)."""
    path = key_file()
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        made = path.with_name(f".{path.name}.{os.getpid()}")
        fd = os.open(made, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(secrets.token_urlsafe(32))
        try:
            os.link(made, path)  # whole or not at all; the first of two made at once is the key
        except FileExistsError:
            pass
        finally:
            made.unlink()
    return path.read_text().strip()


def holds_key(value: str | None) -> bool:
    return bool(value) and hmac.compare_digest(value, key())


def trusted() -> bool:
    """The person, or the system: not the agent."""
    return _trusted.get()


@contextmanager
def system() -> Iterator[None]:
    """What runs inside is the system's (or, in a request with the key, the person's)."""
    token = _trusted.set(True)
    try:
        yield
    finally:
        _trusted.reset(token)


class NotTheAgents(PermissionError):
    """A request the agent may not make."""


def require(what: str) -> None:
    if not trusted() and not settings.demo():
        raise NotTheAgents(f"{what} is the person's, not the agent's: on Optra's page (dufanyin.dev/optra/), "
                           "signed in")


def anyone(request: Request) -> None:
    """A route's dependency: the agent's too (it reads or computes; a tool checks its own `person`)."""


def person_only(request: Request) -> None:
    """A route's dependency: the person's (or anyone's in the demo)."""
    require(f"{request.method} {request.url.path}")


def system_only(request: Request) -> None:
    """A route's dependency: the system's (the workers'), demo or not."""
    if not trusted():
        raise NotTheAgents(f"{request.method} {request.url.path} is the research workers' (with the key)")


WHO = (anyone, person_only, system_only)


def undeclared(routes: list) -> list[str]:
    """The HTTP routes that do not say whose they are (one of WHO as a dependency): none may be open by default."""
    return [f"{','.join(sorted(r.methods))} {r.path}" for r in routes
            if hasattr(r, "dependant") and hasattr(r, "methods")
            and not {d.call for d in r.dependant.dependencies} & set(WHO)]
