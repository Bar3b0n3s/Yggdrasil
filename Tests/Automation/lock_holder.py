"""A process that holds machine-wide locks (Scripts/Lib/locks.py; Docs/Architecture.md §2.3) for the tests of the locks
and of the scripts and the MCP bridge that take them (test_locks.py, Tools/MCP/tests/test_bridge.py).

Every test that uses it runs on a lock directory of its own (ENGINE_LOCK_DIRECTORY, isolated_lock_directory), so it
never touches, waits for or blocks the machine's real locks, which other checkouts' builds and GPU runs hold.

The holder is a child Python process (the base interpreter, so its pid is the process that holds the locks and killing
it ends that process, fake_editor.PYTHON's reason). It asks for each request in turn and prints one line per outcome:
  "waiting <message>"            each report of a wait (locks.Reporter)
  "held <lock> <mode> <owned|inherited>"
  "unavailable <lock> <exit code>" and it exits with that code
then "ready", and holds everything until its standard input gets a line (release) or it is killed. A request is a lock
name (gate, gpu), "build:<capped|uncapped|auto>", or "wait-gate" (locks.wait_for_gate).
"""

from __future__ import annotations

import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_ROOT = REPOSITORY_ROOT / "Scripts"
if str(SCRIPTS_ROOT) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_ROOT))

from Lib import locks  # noqa: E402  (Scripts/ is on sys.path above)

PYTHON = Path(getattr(sys, "_base_executable", sys.executable))
# A generous bound for another process's state, which only bounds a failure (ADR 0008 decision 15).
DEADLINE_SECONDS = 60.0

HOLDER_SCRIPT = r"""
import sys
sys.path.insert(0, sys.argv[1])
from Lib import locks

def report(message):
    print("waiting " + message, flush=True)

timeout = float(sys.argv[2])
holds = []
for request in sys.argv[3:]:
    if request.startswith("--"):
        continue
    try:
        if request == "gate":
            hold = locks.acquire_gate(timeout, report)
        elif request == "gpu":
            hold = locks.acquire_gpu(timeout, report)
        elif request == "wait-gate":
            locks.wait_for_gate(timeout, report)
            print("held wait-gate none owned", flush=True)
            continue
        else:
            hold = locks.acquire_build(request.split(":", 1)[1], timeout, report)
    except locks.LockUnavailable as error:
        print(f"unavailable {error.lock} {error.exit_code}", flush=True)
        sys.exit(error.exit_code)
    holds.append(hold)
    print(f"held {hold.lock} {hold.mode} {'inherited' if hold.inherited else 'owned'}", flush=True)
print("ready", flush=True)
sys.stdin.readline()
for hold in reversed(holds):
    hold.release()
"""


def isolated_lock_directory(test: unittest.TestCase) -> Path:
    """Gives `test` a lock directory of its own and an environment without inherited holds, both undone at its end."""
    directory = Path(tempfile.mkdtemp(prefix="EngineLocks-")).resolve()
    test.addCleanup(shutil.rmtree, directory, ignore_errors=True)
    environment = {key: value for key, value in os.environ.items() if key != locks.HELD_LOCKS_VARIABLE}
    environment[locks.LOCK_DIRECTORY_VARIABLE] = str(directory)
    patch = mock.patch.dict(os.environ, environment, clear=True)
    patch.start()
    test.addCleanup(patch.stop)
    return directory


class LockHolder:
    """A child process holding locks (see the module comment). Use it as a context manager, or call stop()."""

    def __init__(self, *requests: str, timeout: float = 0.0, environment: dict[str, str] | None = None,
                 marker: str = "") -> None:
        """Starts the holder with `requests`, waiting up to `timeout` seconds for each. Its environment is `environment`
        (default: this process's, without its inherited holds), which must name the test's lock directory.
        `marker` is an extra argument the holder ignores, which shows in its recorded command."""
        if environment is None:
            environment = {key: value for key, value in os.environ.items() if key != locks.HELD_LOCKS_VARIABLE}
        command = [str(PYTHON), "-c", HOLDER_SCRIPT, str(SCRIPTS_ROOT), str(timeout), *requests,
                   *([f"--{marker}"] if marker else [])]
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, env=environment, text=True, encoding="utf-8")
        self.lines: queue.Queue[str | None] = queue.Queue()
        self.output: list[str] = []
        self.reader = threading.Thread(target=self._read, daemon=True, name=f"lock-holder-{self.process.pid}")
        self.reader.start()

    @property
    def pid(self) -> int:
        return self.process.pid

    def _read(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            self.lines.put(line.rstrip("\n"))
        self.lines.put(None)

    def wait_for(self, *prefixes: str) -> str:
        """The next output line that starts with one of `prefixes`; raises AssertionError, with the output so far,
        when the holder ends or DEADLINE_SECONDS pass first."""
        deadline = time.monotonic() + DEADLINE_SECONDS
        while True:
            try:
                line = self.lines.get(timeout=max(0.0, deadline - time.monotonic()))
            except queue.Empty:
                line = None
            if line is None:
                raise AssertionError(f"the lock holder (pid {self.pid}) printed no line starting with "
                                     f"{' or '.join(prefixes)}:\n" + "\n".join(self.output))
            self.output.append(line)
            if line.startswith(prefixes):
                return line

    def ready(self) -> LockHolder:
        """Waits until the holder holds every request (raises AssertionError when one was unavailable)."""
        line = self.wait_for("ready", "unavailable")
        if line != "ready":
            raise AssertionError(f"the lock holder (pid {self.pid}) did not get its locks: {line}\n"
                                 + "\n".join(self.output))
        return self

    def release(self) -> int:
        """Lets the holder release its locks and exit; returns its exit code."""
        if self.process.poll() is None:
            assert self.process.stdin is not None
            try:
                self.process.stdin.write("\n")
                self.process.stdin.flush()
            except OSError:
                pass
        return self.process.wait(DEADLINE_SECONDS)

    def kill(self) -> None:
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(DEADLINE_SECONDS)

    def stop(self) -> None:
        """Kills the holder if it still runs, and closes its pipes."""
        self.kill()
        for stream in (self.process.stdin, self.process.stdout):
            if stream is not None:
                try:
                    stream.close()
                except OSError:
                    pass

    def __enter__(self) -> LockHolder:
        return self

    def __exit__(self, *exception: object) -> None:
        self.stop()


def wait_until_free(lock: str) -> None:
    """Waits, bounded by DEADLINE_SECONDS, until nothing holds `lock` (a holder's exit releases it asynchronously, as
    the bridge's watcher thread does); raises AssertionError naming the holders otherwise."""
    deadline = time.monotonic() + DEADLINE_SECONDS
    while True:
        holders = locks.holders(lock)
        if not holders:
            return
        if time.monotonic() >= deadline:
            raise AssertionError(f"the {lock} lock is still held by " + "; ".join(h.describe() for h in holders))
        time.sleep(locks.POLL_SECONDS)
