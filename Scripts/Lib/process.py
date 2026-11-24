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
from typing import Any, Callable, Mapping, Sequence, TextIO

from . import debugheap

# After the child exits, wait at most this long for the reader to drain the pipe. Grandchildren that inherited the
# pipe (MSBuild worker nodes, mspdbsrv.exe) can keep it open long after the build finished.
READER_GRACE_SECONDS = 5.0
TERMINATE_GRACE_SECONDS = 5.0
DEFAULT_TAIL_LINES = 200
# terminate_tree on Windows: the snapshots of the process table it takes at most, each after ending what the one before
# found, until one finds no new descendant (a tree that keeps starting processes while it is ended).
TERMINATE_SNAPSHOT_ROUNDS = 8
_TH32CS_SNAPPROCESS = 0x00000002
_PROCESS_TERMINATE = 0x0001
_SYNCHRONIZE = 0x00100000


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
    process_id: int | None = None  # the started process's id; None in a result made without starting one

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


def find_new_descendants(parents: Mapping[int, int], members: dict[int, int],
                         creation_time: Callable[[int], int | None], snapshot_time: int) -> list[int]:
    """Adds to `members` (process id: creation time, the tree's root among them) every process of `parents` (process id:
    recorded parent id, a snapshot taken at `snapshot_time`) that descends from a member, and returns their ids, each
    after its parent. A process descends from the member its recorded parent id names only when it was created no
    earlier than that member: Windows never updates a recorded parent id when the parent exits and gives ids out again,
    so a process older than the holder of its parent's id was started by an earlier holder and is some other program's.
    `creation_time` reads a process's creation time (None when it is gone); one created after the snapshot holds an id
    whose snapshot process is gone, so it is no member either. Times are FILETIME ticks."""
    added: list[int] = []
    frontier = list(members)
    while frontier:
        parent = frontier.pop(0)
        for child, recorded_parent in parents.items():
            if recorded_parent != parent or child == parent or child in members:
                continue
            created = creation_time(child)
            if created is None or created < members[parent] or created > snapshot_time:
                continue
            members[child] = created
            added.append(child)
            frontier.append(child)
    return added


def _terminate_windows_tree(root: int) -> None:
    """Terminates the process `root` (whose id its Popen object keeps from changing hands) and its descendants
    (find_new_descendants), each through a handle opened before it is ended, so no id changes hands meanwhile; snapshots
    are taken again until one finds no new descendant, which catches what the tree started while it was ended."""
    import ctypes  # noqa: PLC0415  (Windows only)
    from ctypes import wintypes  # noqa: PLC0415

    class ProcessEntry(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                    ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wintypes.DWORD),
                    ("cntThreads", wintypes.DWORD), ("th32ParentProcessID", wintypes.DWORD),
                    ("pcPriClassBase", ctypes.c_long), ("dwFlags", wintypes.DWORD),
                    ("szExeFile", wintypes.WCHAR * 260)]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    filetime_pointer = ctypes.POINTER(wintypes.FILETIME)
    kernel32.CreateToolhelp32Snapshot.argtypes = (wintypes.DWORD, wintypes.DWORD)
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.Process32FirstW.argtypes = (wintypes.HANDLE, ctypes.POINTER(ProcessEntry))
    kernel32.Process32FirstW.restype = wintypes.BOOL
    kernel32.Process32NextW.argtypes = (wintypes.HANDLE, ctypes.POINTER(ProcessEntry))
    kernel32.Process32NextW.restype = wintypes.BOOL
    kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.GetProcessTimes.argtypes = (wintypes.HANDLE, filetime_pointer, filetime_pointer, filetime_pointer,
                                         filetime_pointer)
    kernel32.GetProcessTimes.restype = wintypes.BOOL
    kernel32.GetSystemTimeAsFileTime.argtypes = (filetime_pointer,)
    kernel32.GetSystemTimeAsFileTime.restype = None
    kernel32.TerminateProcess.argtypes = (wintypes.HANDLE, wintypes.UINT)
    kernel32.TerminateProcess.restype = wintypes.BOOL
    kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    kernel32.WaitForSingleObject.restype = wintypes.DWORD
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel32.CloseHandle.restype = wintypes.BOOL
    invalid_handle = wintypes.HANDLE(-1).value

    def ticks(filetime: wintypes.FILETIME) -> int:
        return (filetime.dwHighDateTime << 32) | filetime.dwLowDateTime

    def now() -> int:
        filetime = wintypes.FILETIME()
        kernel32.GetSystemTimeAsFileTime(ctypes.byref(filetime))
        return ticks(filetime)

    def snapshot() -> dict[int, int]:
        handle = kernel32.CreateToolhelp32Snapshot(_TH32CS_SNAPPROCESS, 0)
        if handle is None or handle == invalid_handle:
            return {}
        parents: dict[int, int] = {}
        try:
            entry = ProcessEntry()
            entry.dwSize = ctypes.sizeof(ProcessEntry)
            listed = kernel32.Process32FirstW(handle, ctypes.byref(entry))
            while listed:
                parents[int(entry.th32ProcessID)] = int(entry.th32ParentProcessID)
                listed = kernel32.Process32NextW(handle, ctypes.byref(entry))
        finally:
            kernel32.CloseHandle(handle)
        return parents

    handles: dict[int, int] = {}

    def open_process(process_id: int) -> int | None:
        """Opens `process_id` for good and returns its creation time; None when it cannot be opened."""
        handle = kernel32.OpenProcess(_PROCESS_TERMINATE | _PROCESS_QUERY_LIMITED_INFORMATION | _SYNCHRONIZE, False,
                                      process_id)
        if not handle:
            return None
        times = [wintypes.FILETIME() for _ in range(4)]
        if not kernel32.GetProcessTimes(handle, *(ctypes.byref(filetime) for filetime in times)):
            kernel32.CloseHandle(handle)
            return None
        handles[process_id] = handle
        return ticks(times[0])

    try:
        root_created = open_process(root)
        if root_created is None:
            return  # gone already; the caller's kill() and wait() settle it
        members = {root: root_created}
        ended: set[int] = set()
        for _ in range(TERMINATE_SNAPSHOT_ROUNDS):
            taken = now()
            find_new_descendants(snapshot(), members, open_process, taken)
            fresh = [member for member in members if member not in ended]
            if not fresh:
                break
            for member in fresh:  # the root first, then each process after its parent
                kernel32.TerminateProcess(handles[member], 1)
                ended.add(member)
        deadline = time.monotonic() + TERMINATE_GRACE_SECONDS
        for handle in handles.values():
            kernel32.WaitForSingleObject(handle, int(max(0.0, deadline - time.monotonic()) * 1000))
    finally:
        for handle in handles.values():
            kernel32.CloseHandle(handle)


def terminate_tree(process: subprocess.Popen) -> None:
    """Terminate a process and all of its descendants: on Windows those that find_new_descendants finds in snapshots of
    the processes' recorded parent ids (never `taskkill /T`, which follows the ids alone and so also ends another
    program's process older than the holder of its parent's id; Docs/Backlog.md, R-GPUCRASH's evidence), elsewhere the
    process group the process leads."""
    if process.poll() is not None:
        return
    if os.name == "nt":
        try:
            _terminate_windows_tree(process.pid)
        except OSError:
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
        process_id=process.pid,
    )


def run_streamed(
    command: Sequence[str],
    cwd: Path | None = None,
    env: Mapping[str, str] | None = None,
    timeout: float | None = None,
    echo: TextIO | None = None,
    tail_lines: int = DEFAULT_TAIL_LINES,
    collect: re.Pattern[str] | None = None,
    debug_heap: bool = False,
) -> ProcessResult:
    """Run a command, echoing each output line to `echo` (default stdout) and keeping the last `tail_lines` lines.

    Lines matching `collect` (anywhere in the output, not only the tail) are returned in `collected`. With `debug_heap`
    (Windows only) the command runs on the Windows debug heap (Lib/debugheap.py: started under this thread as its
    debugger, which detaches at the loader's initial breakpoint). Raises ToolNotFoundError if the command cannot be
    started, or not on the debug heap. Ctrl+C terminates the process tree and propagates.
    """
    stream = echo if echo is not None else sys.stdout
    started = time.monotonic()
    options = _popen_options()
    environment = dict(env) if env is not None else None
    if debug_heap:
        if not debugheap.is_available():
            raise ToolNotFoundError(f"cannot start '{command[0]}' on the debug heap: it is Windows only")
        options["creationflags"] = options.get("creationflags", 0) | debugheap.start_flags()
        environment = debugheap.child_environment(environment if environment is not None else os.environ)
    process = _start(
        command,
        cwd=cwd,
        env=environment,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        **options,
    )
    if debug_heap:
        try:
            debugheap.detach_at_initial_breakpoint(process)
        except OSError as error:
            terminate_tree(process)
            raise ToolNotFoundError(f"could not start '{command[0]}' on the debug heap: {error}") from None
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
        process_id=process.pid,
    )


def start_to_file(command: Sequence[str], output: Path, cwd: Path | None = None,
                  env: Mapping[str, str] | None = None) -> subprocess.Popen:
    """Start `command` with its standard output and standard error written to the file `output` (replaced), standard
    input the null device and, like every run here, in a process group of its own, so terminate_tree ends its tree. The
    caller waits for it, and ends it with terminate_tree. Raises ToolNotFoundError if it cannot be started, OSError if
    `output` cannot be written."""
    with output.open("wb") as stream:
        return _start(command, cwd=cwd, env=dict(env) if env is not None else None, stdin=subprocess.DEVNULL,
                      stdout=stream, stderr=subprocess.STDOUT, **_popen_options())


@dataclasses.dataclass(frozen=True)
class ProcessMemory:
    """The memory of a running process (Docs/Decisions/0024-m15-contract.md decision 15): the soak stage's rule and the
    perf capture read it from outside the process, so no engine interface changes."""

    private_bytes: int  # Windows: PrivateUsage; Linux: RssAnon of /proc/<pid>/status; macOS: the resident size
    resident_bytes: int  # Windows: WorkingSetSize; Linux: VmRSS; macOS: ps -o rss (KiB * 1024)
    peak_resident_bytes: int  # Windows: PeakWorkingSetSize; Linux: VmHWM; macOS: equal to resident_bytes


def query_process_memory(pid: int) -> ProcessMemory:
    """The memory of the running process `pid`: on Windows K32GetProcessMemoryInfo with PROCESS_MEMORY_COUNTERS_EX
    through ctypes (PROCESS_QUERY_LIMITED_INFORMATION), on Linux /proc/<pid>/status, on macOS `ps -o rss= -p <pid>`.
    Raises ProcessLookupError when the process has exited, OSError when the query fails."""
    if pid <= 0:
        raise ProcessLookupError(f"no process {pid}")
    if os.name == "nt":
        return _query_windows_process_memory(pid)
    if sys.platform.startswith("linux"):
        return _query_linux_process_memory(pid)
    return _query_ps_process_memory(pid)


_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
_STILL_ACTIVE = 259
_ERROR_INVALID_PARAMETER = 87  # OpenProcess's error for a process id that no process has


def _query_windows_process_memory(pid: int) -> ProcessMemory:
    import ctypes  # noqa: PLC0415  (Windows only)
    from ctypes import wintypes  # noqa: PLC0415

    class ProcessMemoryCountersEx(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("PageFaultCount", wintypes.DWORD),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
            ("PrivateUsage", ctypes.c_size_t),
        ]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel32.CloseHandle.restype = wintypes.BOOL
    kernel32.GetExitCodeProcess.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
    kernel32.GetExitCodeProcess.restype = wintypes.BOOL
    kernel32.K32GetProcessMemoryInfo.argtypes = (wintypes.HANDLE, ctypes.POINTER(ProcessMemoryCountersEx),
                                                 wintypes.DWORD)
    kernel32.K32GetProcessMemoryInfo.restype = wintypes.BOOL

    handle = kernel32.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        error = ctypes.get_last_error()
        if error == _ERROR_INVALID_PARAMETER:
            raise ProcessLookupError(f"no process {pid}")
        raise OSError(error, f"cannot open process {pid}: {ctypes.FormatError(error).strip()}")
    try:
        exit_code = wintypes.DWORD(0)
        if not kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code)):
            error = ctypes.get_last_error()
            raise OSError(error, f"cannot query process {pid}: {ctypes.FormatError(error).strip()}")
        if exit_code.value != _STILL_ACTIVE:
            raise ProcessLookupError(f"process {pid} has exited (exit code {describe_exit_code(exit_code.value)})")
        counters = ProcessMemoryCountersEx()
        counters.cb = ctypes.sizeof(ProcessMemoryCountersEx)
        if not kernel32.K32GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            error = ctypes.get_last_error()
            raise OSError(error, f"cannot read the memory of process {pid}: {ctypes.FormatError(error).strip()}")
        return ProcessMemory(int(counters.PrivateUsage), int(counters.WorkingSetSize), int(counters.PeakWorkingSetSize))
    finally:
        kernel32.CloseHandle(handle)


def _query_linux_process_memory(pid: int) -> ProcessMemory:
    status = Path("/proc") / str(pid) / "status"
    try:
        text = status.read_text(encoding="utf-8")
    except FileNotFoundError:
        raise ProcessLookupError(f"no process {pid}") from None
    if re.search(r"^State:\s+Z", text, re.MULTILINE):
        raise ProcessLookupError(f"process {pid} has exited (a zombie)")
    fields: dict[str, int] = {}
    for line in text.splitlines():
        name, _, value = line.partition(":")
        parts = value.split()
        if len(parts) == 2 and parts[1] == "kB" and parts[0].isdigit():
            fields[name.strip()] = int(parts[0]) * 1024
    try:
        return ProcessMemory(fields["RssAnon"], fields["VmRSS"], fields["VmHWM"])
    except KeyError as missing:
        raise OSError(f"{status} has no {missing.args[0]} line") from None


def _query_ps_process_memory(pid: int) -> ProcessMemory:
    try:
        result = run_captured(["ps", "-o", "rss=", "-p", str(pid)], timeout=30)
    except ToolNotFoundError as error:
        raise OSError(str(error)) from None
    text = result.output.strip()
    if not result.succeeded or not text:
        raise ProcessLookupError(f"no process {pid}")
    if not text.isdigit():
        raise OSError(f"ps printed '{text}' for the resident size of process {pid}")
    resident = int(text) * 1024
    return ProcessMemory(resident, resident, resident)
