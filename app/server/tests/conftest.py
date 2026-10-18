"""Every test session runs with its own OPTRA_HOME, so no test reads or writes the user's: cached chains, the run
history, the saved Treasury curve (tests that need a home of their own still set one)."""

import os
import shutil
import tempfile

import pytest

_home = tempfile.TemporaryDirectory(prefix="optra-tests-")
os.environ["OPTRA_HOME"] = _home.name


@pytest.fixture
def own_strategy_dirs(monkeypatch):
    """strategy/test_<pid>_research, _paper, _live and _configs in place of research, the accounts' folders and the
    experiment configs: built by CMake as every strategy is, loaded by no engine, gone after the test with their
    libraries. Their folders by name."""
    from optra.research import configs, strategies, code_identity

    folders = {name: strategies.strategy_dir() / f"test_{os.getpid()}_{name}"
               for name in ("research", "paper", "live", "configs")}
    monkeypatch.setattr(strategies, "source_dir", lambda: folders["research"])
    monkeypatch.setattr(strategies, "installed_dir", lambda account: folders[account])
    monkeypatch.setattr(configs, "config_dir", lambda: folders["configs"])
    yield folders
    for folder in folders.values():
        shutil.rmtree(folder, ignore_errors=True)
        shutil.rmtree(strategies.library_of(folder / "any.cpp").parent, ignore_errors=True)
        shutil.rmtree(code_identity.engine_dir() / "build" / "strategies-linked" / folder.name, ignore_errors=True)
