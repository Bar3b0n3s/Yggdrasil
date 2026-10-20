"""Machine-wide locks that every checkout of the repository shares (Docs/Architecture.md §2.3; Docs/Decisions/
0028-backlog-program.md decision 16, T-LOCKS).

The checkouts of one repository (the main one and its git worktrees) build, run GPU tests and run gates on one machine.
Three locks keep them from overloading it:
  build  counted: one uncapped holder, or up to two capped holders. A capped build limits itself to about half the
         machine (Build.py: at most half the logical CPUs as jobs, and at most that many compilers for the whole build,
         MSBuild's -m times its per-project compiler count CL_MPCount), so two run side by side; an uncapped build has
         the machine alone. An uncapped request first takes
         the claim (build-claim.lock), which holds off new capped builds until the running ones end, so a stream of
         capped builds cannot starve it.
  gpu    exclusive: one GPU run at a time (Test.py's GPU-using suites, Scaffold.py, Perf.py, Hardening.py's GPU steps
         and the MCP bridge's rendering editors).
  gate   exclusive: CI.py holds it for its whole run, and takes the gpu and then the build lock after it, so it begins
         its stages once the runs that held them have ended. While it is held, every other build or gpu acquisition,
         every Test.py run and every editor launch of the MCP bridge waits, unless it runs inside a run that holds a
         machine lock: CI.py's own children, or the children of a run that began before the gate (the gate waits for
         that run, so its children finish it instead of waiting for the gate, which would deadlock).
Order: gate, then gpu, then build. Nothing that holds the build lock asks for the gpu or gate lock, and nothing that
holds the gpu lock asks for the gate lock, so no two holders wait for each other.

The lock files are <git common directory>/machine-locks/{gate,gpu,build-0,build-1,build-claim}.lock, so every worktree
of the repository uses the same ones (ENGINE_LOCK_DIRECTORY overrides the directory, for the tests' own locks; a
checkout without git keeps them in bin-int/machine-locks). Each file carries an exclusive operating-system lock:
msvcrt.locking of one byte far past the start of the file on Windows, fcntl.flock on POSIX. The operating system
releases it when the holder's process ends, however it ends, so a crashed holder never wedges the machine and nothing is
ever cleared by hand. The first line of the file is the holder's record (JSON: lock, mode, token, pid, worktree,
command, since), which a waiting or refused process prints and Scripts/Locks.py lists.

Inheritance: a holder exports "<lock>=<token>" pairs in ENGINE_HELD_LOCKS, which its child processes inherit. A child
that asks for a lock whose token its environment carries, while that lock's file records the token and is still held,
runs under its ancestor's hold: it neither waits for that hold (a deadlock) nor takes another build slot (a double
count). A process that already holds a lock gets a nested hold of it.

Waiting: every acquisition polls until a bound (0 refuses at once) and reports the holders when it starts waiting, when
they change and every REPORT_SECONDS; at the bound it raises LockUnavailable naming them (exit code 3 for a refusal, 5
for a wait that ran out).

Probes: whether a lock is held is learned by taking it and letting it go at once (is_held), and every waiter's poll
tries the locks it wants, so a free lock can look held for the moment another process probes it. Nothing acts on one
such sighting alone: at the bound a wait polls SETTLE_POLLS more times, SETTLE_SECONDS apart, before it refuses (a
probe ends within microseconds), and an uncapped build gives up its claim for a gate only when the gate stays held
across those polls (_gate_stays_closed), so another process's probe can neither refuse a free lock nor let a capped
build overtake a waiting uncapped one.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime
import errno
import functools
import json
import os
import secrets
import subprocess
import sys
import threading
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from . import paths
from .process import format_command
from .report import EXIT_INIT_FAILED, EXIT_TIMEOUT, Status, Step

GATE = "gate"
GPU = "gpu"
BUILD = "build"
LOCK_NAMES = (GATE, GPU, BUILD)

# Build modes: Build.py asks for one of them; a hold is capped or uncapped.
CAPPED = "capped"
UNCAPPED = "uncapped"
AUTO = "auto"
BUILD_MODES = (CAPPED, UNCAPPED, AUTO)
EXCLUSIVE = "exclusive"
CLAIM = "uncapped, waiting for the running builds"
BUILD_SLOTS = 2

LOCK_DIRECTORY_NAME = "machine-locks"
LOCK_DIRECTORY_VARIABLE = "ENGINE_LOCK_DIRECTORY"
HELD_LOCKS_VARIABLE = "ENGINE_HELD_LOCKS"

DEFAULT_WAIT_SECONDS = 3 * 3600.0
POLL_SECONDS = 0.25
# A lock found held may be another process's probe of a free lock (is_held, a waiter's attempt), which lasts
# microseconds: before a wait refuses, and before a waiting uncapped build gives up its claim for a gate, the lock is
# polled this many more times, this far apart.
SETTLE_POLLS = 4
SETTLE_SECONDS = 0.05
REPORT_SECONDS = 300.0
GIT_TIMEOUT_SECONDS = 60.0
# Windows byte-range locks are mandatory, so the lock takes one byte far past the record, which stays readable. The C
# runtime's _locking takes the file position as its offset, which stays below 2 GiB here.
WINDOWS_LOCK_OFFSET = 1 << 30
RECORD_LIMIT = 64 * 1024
# The errors of a non-blocking lock attempt on a lock another handle holds: _locking's EACCES (EDEADLOCK after its
# retries) on Windows, flock's EWOULDBLOCK (EAGAIN) on POSIX.
HELD_ERRORS = frozenset(code for code in (errno.EACCES, errno.EAGAIN, errno.EWOULDBLOCK,
                                          getattr(errno, "EDEADLOCK", errno.EDEADLK)))

Reporter = Callable[[str], None]


@dataclasses.dataclass(frozen=True)
class Holder:
    """The record a holder writes at the start of its lock file."""

    lock: str
    mode: str  # EXCLUSIVE for gate and gpu; CAPPED, UNCAPPED or CLAIM for build
    token: str
    pid: int
    worktree: str
    command: str
    since: str  # UTC, ISO 8601

    def describe(self) -> str:
        mode = f" ({self.mode})" if self.lock == BUILD else ""
        return f"pid {self.pid}{mode} in {self.worktree} since {self.since}: {self.command}"

    def to_json(self) -> dict[str, Any]:
        return {"lock": self.lock, "mode": self.mode, "pid": self.pid, "worktree": self.worktree,
                "command": self.command, "since": self.since}


class LockUnavailable(RuntimeError):
    """A lock stayed held by other processes: refused at once (timeout 0) or until the wait ran out."""

    def __init__(self, lock: str, holders: list[Holder], waited: float, timed_out: bool) -> None:
        self.lock = lock
        self.holders = holders
        self.waited = waited
        self.timed_out = timed_out
        held_by = "; ".join(holder.describe() for holder in holders) or "another process"
        outcome = f"gave up after waiting {waited:.0f} s" if timed_out else "not waiting"
        super().__init__(f"the {lock} lock is held by {held_by} ({outcome})")

    @property
    def exit_code(self) -> int:
        return EXIT_TIMEOUT if self.timed_out else EXIT_INIT_FAILED

    def to_json(self) -> dict[str, Any]:
        return {"error": "LockUnavailable", "lock": self.lock, "holders": [holder.to_json() for holder in self.holders],
                "waitedSeconds": round(self.waited, 1), "timedOut": self.timed_out}


# --------------------------------------------------------------------------------------------------------------------
# Lock files
# --------------------------------------------------------------------------------------------------------------------


def lock_directory() -> Path:
    """ENGINE_LOCK_DIRECTORY when set, else <git common directory>/machine-locks (shared by every worktree), else
    bin-int/machine-locks of this checkout when git cannot name the common directory."""
    override = os.environ.get(LOCK_DIRECTORY_VARIABLE, "")
    if override:
        return Path(override).resolve()
    return _repository_lock_directory()


@functools.cache
def _repository_lock_directory() -> Path:
    try:
        result = subprocess.run(["git", "rev-parse", "--git-common-dir"], cwd=paths.REPOSITORY_ROOT,
                                stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=GIT_TIMEOUT_SECONDS,
                                check=False)
    except (OSError, subprocess.TimeoutExpired):
        return paths.BIN_INT_ROOT / LOCK_DIRECTORY_NAME
    common = result.stdout.strip()
    if result.returncode != 0 or not common:
        return paths.BIN_INT_ROOT / LOCK_DIRECTORY_NAME
    return (paths.REPOSITORY_ROOT / common).resolve() / LOCK_DIRECTORY_NAME


def lock_paths(lock: str) -> list[Path]:
    """The files that are held while `lock` is: one for gate and gpu, the slots for build."""
    directory = lock_directory()
    if lock == BUILD:
        return [directory / f"build-{slot}.lock" for slot in range(BUILD_SLOTS)]
    return [directory / f"{lock}.lock"]


def claim_path() -> Path:
    return lock_directory() / "build-claim.lock"


class LockFile:
    """One lock file: a non-blocking exclusive OS lock, and the holder record at its start."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.descriptor: int | None = None

    def try_lock(self) -> bool:
        """Takes the lock; False when another handle holds it. Raises OSError when the file cannot be opened."""
        self.path.parent.mkdir(parents=True, exist_ok=True)
        descriptor = os.open(self.path, os.O_RDWR | os.O_CREAT | getattr(os, "O_BINARY", 0), 0o666)
        try:
            locked = _lock_descriptor(descriptor)
        except BaseException:
            os.close(descriptor)
            raise
        if not locked:
            os.close(descriptor)
            return False
        self.descriptor = descriptor
        return True

    def write_record(self, holder: Holder) -> None:
        assert self.descriptor is not None
        data = (json.dumps(dataclasses.asdict(holder), sort_keys=True) + "\n").encode("utf-8")
        os.lseek(self.descriptor, 0, os.SEEK_SET)
        os.ftruncate(self.descriptor, 0)
        os.write(self.descriptor, data)

    def unlock(self, clear: bool = True) -> None:
        """Releases the lock; `clear` empties the record first (a probe that found the lock free leaves it)."""
        descriptor, self.descriptor = self.descriptor, None
        if descriptor is None:
            return
        try:
            if clear:
                os.lseek(descriptor, 0, os.SEEK_SET)
                os.ftruncate(descriptor, 0)
            _unlock_descriptor(descriptor)
        finally:
            os.close(descriptor)


if sys.platform == "win32":
    import msvcrt

    def _lock_descriptor(descriptor: int) -> bool:
        os.lseek(descriptor, WINDOWS_LOCK_OFFSET, os.SEEK_SET)
        try:
            msvcrt.locking(descriptor, msvcrt.LK_NBLCK, 1)
        except OSError as error:
            if error.errno in HELD_ERRORS:
                return False
            raise
        return True

    def _unlock_descriptor(descriptor: int) -> None:
        os.lseek(descriptor, WINDOWS_LOCK_OFFSET, os.SEEK_SET)
        msvcrt.locking(descriptor, msvcrt.LK_UNLCK, 1)
else:
    import fcntl

    def _lock_descriptor(descriptor: int) -> bool:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            if error.errno in HELD_ERRORS:
                return False
            raise
        return True

    def _unlock_descriptor(descriptor: int) -> None:
        fcntl.flock(descriptor, fcntl.LOCK_UN)


def read_record(path: Path, lock: str) -> Holder | None:
    """The holder record at the start of `path`; None when there is none or it is incomplete."""
    try:
        with path.open("rb") as stream:
            data = stream.read(RECORD_LIMIT)
    except OSError:
        return None
    try:
        document = json.loads(data.split(b"\n", 1)[0].decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(document, dict):
        return None
    fields = {field.name: document.get(field.name) for field in dataclasses.fields(Holder)}
    if not isinstance(fields["pid"], int) or isinstance(fields["pid"], bool):
        return None
    if not all(isinstance(value, str) for name, value in fields.items() if name != "pid"):
        return None
    if fields["lock"] != lock:
        return None
    return Holder(**fields)


def is_held(path: Path) -> bool:
    """Whether some handle (another process's, or another one of this process) holds the lock of `path`; a free lock is
    taken and released at once, its record left as it is."""
    probe = LockFile(path)
    if not probe.try_lock():
        return True
    probe.unlock(clear=False)
    return False


def current_holder(path: Path, lock: str) -> Holder | None:
    """The holder of `path`, or None when it is free or its holder has not written its record: a process that took it
    a moment ago, or one that only probes it (is_held, or a waiter's attempt), which lets it go at once."""
    if not is_held(path):
        return None
    return read_record(path, lock)


def holders(lock: str) -> list[Holder]:
    """Every current holder of `lock` that has written its record (for build: the claim first, then the slots; one
    entry per hold)."""
    found: list[Holder] = []
    candidates = ([claim_path()] if lock == BUILD else []) + lock_paths(lock)
    for path in candidates:
        holder = current_holder(path, lock)
        if holder is not None and all(holder.token != other.token for other in found):
            found.append(holder)
    return found


# --------------------------------------------------------------------------------------------------------------------
# Holds, nesting and inheritance
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass
class _Ownership:
    """A lock this process holds: its files, mode and token, and how many holds share it."""

    lock: str
    mode: str
    token: str
    files: list[LockFile]
    count: int = 1


@dataclasses.dataclass
class _Registry:
    """The locks this process holds, by name; `guard` serializes their changes (the bridge releases a hold on the
    thread that watches its editor)."""

    guard: threading.RLock = dataclasses.field(default_factory=threading.RLock)
    owned: dict[str, _Ownership] = dataclasses.field(default_factory=dict)


@functools.cache
def _registry() -> _Registry:
    """This process's one registry."""
    return _Registry()


class Hold:
    """A held lock, released by release() or at the end of a with block. A nested hold (this process already held the
    lock) or an inherited one (an ancestor holds it) releases nothing of the lock itself."""

    def __init__(self, lock: str, mode: str, inherited: bool = False, ownership: _Ownership | None = None) -> None:
        self.lock = lock
        self.mode = mode
        self.inherited = inherited
        self._ownership = ownership
        self._released = False

    @property
    def capped(self) -> bool:
        return self.mode == CAPPED

    def release(self) -> None:
        registry = _registry()
        with registry.guard:
            if self._released:
                return
            self._released = True
            ownership = self._ownership
            if ownership is None:
                return
            ownership.count -= 1
            if ownership.count > 0:
                return
            if registry.owned.get(ownership.lock) is ownership:
                del registry.owned[ownership.lock]
            _withdraw_token(ownership.lock, ownership.token)
            for file in reversed(ownership.files):
                file.unlock()

    def __enter__(self) -> Hold:
        return self

    def __exit__(self, *exception: object) -> None:
        self.release()


def inherited_tokens() -> dict[str, str]:
    """The lock tokens ENGINE_HELD_LOCKS carries: "<lock>=<token>" pairs separated by commas."""
    tokens: dict[str, str] = {}
    for item in os.environ.get(HELD_LOCKS_VARIABLE, "").split(","):
        name, _, token = item.strip().partition("=")
        if name in LOCK_NAMES and token:
            tokens[name] = token
    return tokens


def _export_token(lock: str, token: str) -> None:
    tokens = inherited_tokens()
    tokens[lock] = token
    os.environ[HELD_LOCKS_VARIABLE] = ",".join(f"{name}={value}" for name, value in sorted(tokens.items()))


def _withdraw_token(lock: str, token: str) -> None:
    tokens = inherited_tokens()
    if tokens.get(lock) != token:
        return
    del tokens[lock]
    if tokens:
        os.environ[HELD_LOCKS_VARIABLE] = ",".join(f"{name}={value}" for name, value in sorted(tokens.items()))
    else:
        os.environ.pop(HELD_LOCKS_VARIABLE, None)


def inherited_holder(lock: str) -> Holder | None:
    """The ancestor's hold of `lock` this process runs under: the record of a held file of `lock` with the token
    ENGINE_HELD_LOCKS carries for it; None when there is none (no token, a stale one, or a lock no longer held)."""
    token = inherited_tokens().get(lock)
    if not token:
        return None
    for path in lock_paths(lock):
        record = read_record(path, lock)
        if record is not None and record.token == token and is_held(path):
            return record
    return None


def _nested(lock: str, requested: str) -> Hold | None:
    """A hold under this process's own hold of `lock` or an ancestor's, or None when neither exists. Under a capped
    build hold the nested hold is capped; under an uncapped one it is what was asked for (uncapped for AUTO)."""
    registry = _registry()
    with registry.guard:
        ownership = registry.owned.get(lock)
        if ownership is not None:
            ownership.count += 1
            return Hold(lock, _nested_mode(ownership.mode, requested), ownership=ownership)
    holder = inherited_holder(lock)
    if holder is None:
        return None
    return Hold(lock, _nested_mode(holder.mode, requested), inherited=True)


def _nested_mode(held: str, requested: str) -> str:
    if held == CAPPED:
        return CAPPED
    if held == UNCAPPED:
        return UNCAPPED if requested == AUTO else requested
    return held


def _gate_open() -> bool:
    """Whether a gate-respecting acquisition may proceed: the gate is free, or this process or an ancestor holds any
    machine lock. A gate waits for the runs that hold a lock when it begins (CI.py takes the gpu and build locks after
    the gate), so a process inside such a run finishes it instead of waiting for that gate, which would deadlock."""
    registry = _registry()
    with registry.guard:
        if registry.owned:
            return True
    if not is_held(lock_paths(GATE)[0]):
        return True
    return any(inherited_holder(lock) is not None for lock in LOCK_NAMES)


def _new_holder(lock: str, mode: str, token: str, command: str | None) -> Holder:
    shown = command or format_command([Path(sys.executable).name, *sys.argv])
    since = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return Holder(lock, mode, token, os.getpid(), paths.REPOSITORY_ROOT.as_posix(), shown, since)


def _own(lock: str, mode: str, token: str, files: list[LockFile], command: str | None) -> Hold:
    """Records this process as the holder of `files` and makes the hold inheritable."""
    holder = _new_holder(lock, mode, token, command)
    for file in files:
        file.write_record(holder)
    ownership = _Ownership(lock, mode, token, files)
    registry = _registry()
    with registry.guard:
        registry.owned[lock] = ownership
        _export_token(lock, token)
    return Hold(lock, mode, ownership=ownership)


# --------------------------------------------------------------------------------------------------------------------
# Waiting
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass
class _Blocked:
    """Why one poll could not take the lock: the lock that is in the way and its holders."""

    lock: str
    holders: list[Holder]


def _wait(description: str, poll: Callable[[], Hold | _Blocked], timeout: float, report: Reporter,
          abandon: Callable[[], None] | None = None) -> Hold:
    """Polls until `poll` returns a hold or `timeout` seconds have passed, reporting the holders when the wait starts,
    when they change and every REPORT_SECONDS. At the bound it polls SETTLE_POLLS more times, SETTLE_SECONDS apart (a
    lock that looked held may have been another process's probe), then raises LockUnavailable; `abandon` then undoes
    what the polls kept (a build claim)."""
    started = time.monotonic()
    deadline = started + max(0.0, timeout)
    shown: tuple[str, ...] | None = None
    next_report = started
    try:
        while True:
            outcome = poll()
            if isinstance(outcome, Hold):
                if shown is not None:
                    report(f"Took the {description} after waiting {time.monotonic() - started:.0f} s")
                return outcome
            now = time.monotonic()
            if now >= deadline:
                for _ in range(SETTLE_POLLS):
                    time.sleep(SETTLE_SECONDS)
                    outcome = poll()
                    if isinstance(outcome, Hold):
                        return outcome
                raise LockUnavailable(outcome.lock, outcome.holders, time.monotonic() - started,
                                      timed_out=timeout > 0)
            current = tuple(holder.describe() for holder in outcome.holders)
            if current != shown or now >= next_report:
                held_by = "; ".join(current) or "another process"
                report(f"Waiting for the {description} ({now - started:.0f} s of at most {timeout:.0f} s): the "
                       f"{outcome.lock} lock is held by {held_by}")
                shown = current
                next_report = now + REPORT_SECONDS
            time.sleep(min(POLL_SECONDS, max(0.0, deadline - now)))
    except BaseException:
        if abandon is not None:
            abandon()
        raise


def _gate_stays_closed() -> bool:
    """Whether the gate, found closed (_gate_open), stays closed across SETTLE_POLLS probes SETTLE_SECONDS apart: a
    single sighting may be another process's probe of a free gate."""
    for _ in range(SETTLE_POLLS):
        time.sleep(SETTLE_SECONDS)
        if _gate_open():
            return False
    return True


def _gate_blocked() -> _Blocked:
    return _Blocked(GATE, holders(GATE))


def wait_for_gate(timeout: float, report: Reporter) -> None:
    """Returns once no gate is running (or this process or an ancestor runs it); raises LockUnavailable otherwise."""

    def poll() -> Hold | _Blocked:
        return Hold(GATE, EXCLUSIVE, inherited=True) if _gate_open() else _gate_blocked()

    _wait("end of the running gate", poll, timeout, report)


def acquire_gate(timeout: float, report: Reporter, command: str | None = None) -> Hold:
    """The exclusive gate lock (CI.py, for its whole run)."""
    return _acquire_exclusive(GATE, timeout, report, command, respect_gate=False)


def acquire_gpu(timeout: float, report: Reporter, command: str | None = None) -> Hold:
    """The exclusive GPU lock; it waits while a gate runs, unless this process or an ancestor runs it."""
    return _acquire_exclusive(GPU, timeout, report, command, respect_gate=True)


def _acquire_exclusive(lock: str, timeout: float, report: Reporter, command: str | None, respect_gate: bool) -> Hold:
    def poll() -> Hold | _Blocked:
        nested = _nested(lock, EXCLUSIVE)
        if nested is not None:
            return nested
        if respect_gate and not _gate_open():
            return _gate_blocked()
        file = LockFile(lock_paths(lock)[0])
        if not file.try_lock():
            return _Blocked(lock, holders(lock))
        if respect_gate and not _gate_open():
            file.unlock(clear=False)
            return _gate_blocked()
        return _own(lock, EXCLUSIVE, secrets.token_hex(16), [file], command)

    return _wait(f"{lock} lock", poll, timeout, report)


def acquire_build(requested: str, timeout: float, report: Reporter, command: str | None = None) -> Hold:
    """The build lock in `requested` mode: UNCAPPED takes both slots (after the claim, which holds off new capped
    builds), CAPPED one slot, AUTO both slots when no build holds or claims the lock and one slot beside a single capped
    build. The hold's mode is what the build may use. It waits while a gate runs, unless this process or an ancestor
    runs it."""
    if requested not in BUILD_MODES:
        raise ValueError(f"unknown build mode '{requested}' (expected {', '.join(BUILD_MODES)})")
    attempt = _BuildAttempt(requested, command)
    return _wait("build lock", attempt.poll, timeout, report, abandon=attempt.abandon)


class _BuildAttempt:
    """The polls of one build lock request; an uncapped one keeps the claim between polls."""

    def __init__(self, requested: str, command: str | None) -> None:
        self.requested = requested
        self.command = command
        self.claim: LockFile | None = None
        self.token = secrets.token_hex(16)

    def abandon(self) -> None:
        if self.claim is not None:
            self.claim.unlock()
            self.claim = None

    def poll(self) -> Hold | _Blocked:
        nested = _nested(BUILD, self.requested)
        if nested is not None:
            self.abandon()
            return nested
        if not _gate_open():
            # The claim goes back only for a gate that runs (its own build request needs the claim), never for another
            # process's probe of a free gate, which would let a capped build overtake this one.
            if self.claim is not None and _gate_stays_closed():
                self.abandon()
            return _gate_blocked()
        if self.requested == UNCAPPED:
            return self.poll_uncapped()
        if is_held(claim_path()):
            return _Blocked(BUILD, holders(BUILD))
        slot_paths = lock_paths(BUILD)
        slots = [LockFile(path) for path in slot_paths]
        # A capped build prefers the last slot; AUTO tries both.
        taken = [slot for slot in reversed(slots) if slot.try_lock()]
        if self.requested == CAPPED and len(taken) == BUILD_SLOTS:
            taken.pop().unlock(clear=False)
        if len(taken) == BUILD_SLOTS:
            return self.own(UNCAPPED, taken)
        if taken:
            other = next(path for path, slot in zip(slot_paths, slots) if all(slot is not mine for mine in taken))
            beside = read_record(other, BUILD)
            if self.requested == CAPPED or (beside is not None and beside.mode == CAPPED):
                return self.own(CAPPED, taken)
            for slot in taken:
                slot.unlock(clear=False)
        return _Blocked(BUILD, holders(BUILD))

    def poll_uncapped(self) -> Hold | _Blocked:
        if self.claim is None:
            claim = LockFile(claim_path())
            if not claim.try_lock():
                return _Blocked(BUILD, holders(BUILD))
            claim.write_record(_new_holder(BUILD, CLAIM, self.token, self.command))
            self.claim = claim
        slots = [LockFile(path) for path in lock_paths(BUILD)]
        taken = [slot for slot in slots if slot.try_lock()]
        if len(taken) == BUILD_SLOTS:
            outcome = self.own(UNCAPPED, taken)
            if isinstance(outcome, Hold):
                self.abandon()
            return outcome
        for slot in taken:
            slot.unlock(clear=False)
        return _Blocked(BUILD, [holder for holder in holders(BUILD) if holder.token != self.token])

    def own(self, mode: str, taken: list[LockFile]) -> Hold | _Blocked:
        """Records the hold of the slots taken, unless a gate began since the gate check: then the slots (and an
        uncapped request's claim, which the gate's own build request needs) go back."""
        if not _gate_open():
            for slot in taken:
                slot.unlock(clear=False)
            if self.claim is not None and _gate_stays_closed():
                self.abandon()
            return _gate_blocked()
        return _own(BUILD, mode, self.token, taken, self.command)


# --------------------------------------------------------------------------------------------------------------------
# The scripts' options and steps
# --------------------------------------------------------------------------------------------------------------------


def add_wait_arguments(parser: argparse.ArgumentParser) -> None:
    """--lock-timeout and --no-wait, the bound of every machine-lock wait of a script."""
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--lock-timeout", type=float, default=DEFAULT_WAIT_SECONDS, metavar="SECONDS",
                       help=f"wait at most this long for the machine-wide locks (gate, build, gpu; Architecture §2.3), "
                            f"reporting the holder, then fail with exit code 5 (default: {DEFAULT_WAIT_SECONDS:.0f})")
    group.add_argument("--no-wait", action="store_true",
                       help="fail at once (exit code 3), naming the holder, when a machine-wide lock is held")


def wait_seconds(arguments: argparse.Namespace) -> float:
    """The wait bound --lock-timeout and --no-wait select (0: refuse at once)."""
    if arguments.no_wait:
        return 0.0
    return max(0.0, float(arguments.lock_timeout))


def check_wait_arguments(parser: argparse.ArgumentParser, arguments: argparse.Namespace) -> None:
    if not arguments.no_wait and not 0.0 <= arguments.lock_timeout < float("inf"):
        parser.error("--lock-timeout must be a finite number of seconds, at least 0")


def unavailable_step(error: LockUnavailable) -> Step:
    """The failing step of a script whose machine lock stayed held."""
    status = Status.TIMEOUT if error.timed_out else Status.FAILED
    return Step(f"{error.lock} lock", status, str(error), error.waited, exit_code=error.exit_code,
                data={"lock": error.to_json()})
