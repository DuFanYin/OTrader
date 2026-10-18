"""systemd's watchdog (WatchdogSec= in the unit): told the server is alive from a task of its event loop, so a server
whose loop is stuck stops telling and systemd restarts it. Nothing to do without $NOTIFY_SOCKET (not under systemd)."""

from __future__ import annotations

import asyncio
import os
import socket


def _notify(message: bytes) -> None:
    path = os.environ["NOTIFY_SOCKET"]
    address = "\0" + path[1:] if path.startswith("@") else path
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as s:
        s.sendto(message, address)


async def run() -> None:
    if "NOTIFY_SOCKET" not in os.environ or "WATCHDOG_USEC" not in os.environ:
        return
    interval = int(os.environ["WATCHDOG_USEC"]) / 1e6 / 3
    _notify(b"READY=1")
    while True:
        _notify(b"WATCHDOG=1")
        await asyncio.sleep(interval)
