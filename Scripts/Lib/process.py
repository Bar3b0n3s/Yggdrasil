"""Running tools: captured runs for short queries, streamed runs (tee + tail) for builds and tests.

Streamed runs echo the child's combined stdout/stderr line by line while keeping the last lines for summaries and
--json output. Every run has a timeout; on expiry (or Ctrl+C) the whole process tree is terminated, because build
tools (MSBuild, make, xcodebuild) spawn compilers that would otherwise keep running.
"""

from __future__ import annotations

import collections
import dataclasses
import locale
import os
import re
import shlex
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any, Mapping, Sequence, TextIO

# After the child exits, wait at most this long for the reader to drain the pipe. Grandchildren that inherited the
# pipe (MSBuild worker nodes, mspdbsrv.exe) can keep it open long after the build finished.
READER_GRACE_SECONDS = 5.0
TERMINATE_GRACE_SECONDS = 5.0
DEFAULT_TAIL_LINES = 200


class ToolNotFoundError(Exception):
    """The executable of a command does not exist or cannot be started."""


@dataclasses.dataclass
class ProcessResult:
    command: list[str]
    exit_code: int | None  # None when the process was terminated after a timeout
    timed_out: bool
    duration: float
    output: str  # the full output for captured runs, the last lines for streamed runs
    collected: list[str] = dataclasses.field(default_factory=list)  # streamed lines matching `collect`, in order

    @property
    def succeeded(self) -> bool:
        return not self.timed_out and self.exit_code == 0

    def describe_exit(self) -> str:
        if self.timed_out:
            return f"timed out after {self.duration:.0f} s"
        return f"exit code {describe_exit_code(self.exit_code)}"

    def tail(self, lines: int = 40) -> str:
        return "\n".join(self.output.rstrip().splitlines()[-lines:])


def describe_exit_code(code: int | None) -> str:
    """Exit code as text; Windows NTSTATUS crash codes (0xC0000005, ...) in hex, POSIX signals by name."""
    if code is None:
        return "none"
    if os.name == "nt" and (code < 0 or code >= 0x80000000):
        return f"0x{code & 0xFFFFFFFF:08X}"
    if code < 0:
        try:
            return f"{code} ({signal.Signals(-code).name})"
        except ValueError:
            return str(code)
    return str(code)


def format_command(command: Sequence[str]) -> str:
    if os.name == "nt":
        return subprocess.list2cmdline(list(command))
    return shlex.join(list(command))


def child_environment(extra: Mapping[str, str] | None = None) -> dict[str, str]:
    """os.environ plus `extra`, with unbuffered UTF-8 output for child Python scripts (their lines stream in order)."""
    environment = dict(os.environ)
    environment["PYTHONUNBUFFERED"] = "1"
    environment["PYTHONIOENCODING"] = "utf-8"
    if extra:
        environment.update(extra)
    return environment


def _decode(raw: bytes) -> str:
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        # Native Windows tools write in the active code page when their output is redirected.
        return raw.decode(locale.getpreferredencoding(False), errors="replace")


def _popen_options() -> dict:
    if os.name == "nt":
        # A separate process group, so Ctrl+C is handled here and the tree is terminated deliberately.
        return {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
    return {"start_new_session": True}


def terminate_tree(process: subprocess.Popen) -> None:
    """Terminate a process and all of its descendants."""
    if process.poll() is not None:
        return
    if os.name == "nt":
        try:
            subprocess.run(
                ["taskkill", "/F", "/T", "/PID", str(process.pid)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=TERMINATE_GRACE_SECONDS * 6,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired):
            pass  # process.kill() below still terminates the process itself
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(TERMINATE_GRACE_SECONDS)
                return
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
    if process.poll() is None:
        process.kill()
    process.wait()


def _start(command: Sequence[str], **options: Any) -> subprocess.Popen:
    try:
        return subprocess.Popen([str(part) for part in command], **options)
    except FileNotFoundError:
        raise ToolNotFoundError(f"'{command[0]}' was not found") from None
    except OSError as error:
        raise ToolNotFoundError(f"could not start '{command[0]}': {error}") from None


def run_captured(
    command: Sequence[str],
    cwd: Path | None = None,
    env: Mapping[str, str] | None = None,
    timeout: float = 120.0,
) -> ProcessResult:
    """Run a short command and capture its combined output. Raises ToolNotFoundError if it cannot be started."""
    started = time.monotonic()
    process = _start(
        command,
        cwd=cwd,
        env=dict(env) if env is not None else None,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        **_popen_options(),
    )
    try:
        raw, _ = process.communicate(timeout=timeout)
        timed_out = False
    except subprocess.TimeoutExpired:
        terminate_tree(process)
        raw, _ = process.communicate()
        timed_out = True
    except BaseException:
        terminate_tree(process)
        raise
    return ProcessResult(
        [str(part) for part in command],
        None if timed_out else process.returncode,
        timed_out,
        time.monotonic() - started,
        _decode(raw or b"").replace("\r\n", "\n"),
    )


def run_streamed(
    command: Sequence[str],
    cwd: Path | None = None,
    env: Mapping[str, str] | None = None,
    timeout: float | None = None,
    echo: TextIO | None = None,
    tail_lines: int = DEFAULT_TAIL_LINES,
    collect: re.Pattern[str] | None = None,
) -> ProcessResult:
    """Run a command, echoing each output line to `echo` (default stdout) and keeping the last `tail_lines` lines.

    Lines matching `collect` (anywhere in the output, not only the tail) are returned in `collected`. Raises
    ToolNotFoundError if the command cannot be started. Ctrl+C terminates the process tree and propagates.
    """
    stream = echo if echo is not None else sys.stdout
    started = time.monotonic()
    process = _start(
        command,
        cwd=cwd,
        env=dict(env) if env is not None else None,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        **_popen_options(),
    )
    tail: collections.deque[str] = collections.deque(maxlen=tail_lines)
    collected: list[str] = []

    def pump() -> None:
        assert process.stdout is not None
        # A buffered reader returns each line as soon as it is complete.
        for raw in iter(process.stdout.readline, b""):
            line = _decode(raw).rstrip("\r\n")
            tail.append(line)
            if collect is not None and collect.search(line):
                collected.append(line)
            try:
                stream.write(line + "\n")
                stream.flush()
            except (OSError, ValueError):
                pass  # the console went away; keep draining so the child never blocks on a full pipe

    reader = threading.Thread(target=pump, name="process-output", daemon=True)
    reader.start()
    timed_out = False
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        terminate_tree(process)
    except BaseException:
        terminate_tree(process)
        raise
    reader.join(READER_GRACE_SECONDS)
    return ProcessResult(
        [str(part) for part in command],
        None if timed_out else process.returncode,
        timed_out,
        time.monotonic() - started,
        "\n".join(tail),
        collected,
    )
