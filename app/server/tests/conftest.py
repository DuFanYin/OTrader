"""Every test session runs with its own OPTRA_HOME, so no test reads or writes the user's: cached chains, the run
history, the saved Treasury curve (tests that need a home of their own still set one)."""

import os
import tempfile

_home = tempfile.TemporaryDirectory(prefix="optra-tests-")
os.environ["OPTRA_HOME"] = _home.name
