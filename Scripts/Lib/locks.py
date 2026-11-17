"""Machine-wide locks that every checkout of the repository shares (Docs/Architecture.md §2.3; Docs/Decisions/
0028-backlog-program.md decision 16, T-LOCKS).

The checkouts of one repository (the main one and its git worktrees) build, run GPU tests and run gates on one machine.
Three locks keep them from overloading it:
  build  counted: one uncapped holder, or up to two capped holders. A capped build limits itself to about half the
         machine (Build.py: at most half the logical CPUs as jobs, and at most that many compilers for the whole build,
         MSBuild's -m times its per-project compiler count CL_MPCount), so two run side by side; an uncapped build has
         the machine alone. The slots go to the waiting requests in arrival order (Queue, below), so a stream of capped
         builds cannot starve an uncapped one; an uncapped request at the head of the queue also takes the claim
         (build-claim.lock), which holds off the new capped builds of checkouts whose locks.py has no queue.
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

Waiting: every acquisition polls until a bound (0 refuses at once) and reports the holders, and the requests queued
ahead of it, when it starts waiting, when they change and every REPORT_SECONDS; at the bound it raises LockUnavailable
naming them (exit code 3 for a refusal, 5 for a wait that ran out).

Queue (first come, first served): a request that has to wait takes a ticket, <lock directory>/queue/<lock>-<sequence>
.ticket, and holds an exclusive operating-system lock on it, as a holder does on its lock file, until its wait ends
(taken, refused or interrupted). The sequence is a counter in <lock directory>/queue.lock, taken under that file's
short exclusive lock (the queue's claim), so tickets are numbered in the order their requests began to wait. A ticket
whose lock is free is a dead waiter's (the operating system released it): every reader skips it, and one that takes the
claim removes it. Only the earliest live requests may take a lock:
  gate, gpu  a request takes the lock only when no earlier request of it is queued that can take it now;
  build      the earlier requests take their slots first, oldest first, and the first one that cannot start yet (an
             uncapped request beside a running build, a capped one with both slots taken) stops everything behind it,
             so the slots are given in arrival order and a stream of capped builds never starves an uncapped one.
A request that refuses at once (--no-wait) takes no ticket and comes after every queued one. The gate keeps its
precedence: while a gate runs or a CI.py queues for it (_gate_closed), a gpu or build request outside a run that holds a
machine lock waits, and the queued ones are passed over by the requests that run inside one (the gate's own CI.py and
its children, or the children of a run that began before the gate), which would otherwise wait for requests that wait
for the gate. A ticket records the request's ENGINE_HELD_LOCKS, so every reader can tell whether it runs inside a held
run (_runs_inside). An uncapped build takes the claim file of older checkouts (build-claim.lock) only at the head of its
queue. A process can keep one place in a lock's queue across several no-wait tries (queued(): the MCP bridge's
cancellable wait). A process running an older locks.py takes no ticket and does not read the queue: it still takes a
lock only when the lock is free (and, for build, respects build-claim.lock), so it can take a lock ahead of the queued
requests, but it never removes, reorders or waits for a ticket, and the queue never waits for it except while it holds
a lock or that claim, which the operating system releases when it ends: it can cut in, never deadlock the queue. A
waiter that stops polling while it lives (a suspended process) keeps its place, as a suspended holder keeps its lock.

Probes: whether a lock is held is learned by taking it and letting it go at once (is_held), and every waiter's poll
tries the locks it wants, so a free lock can look held for the moment another process probes it. Nothing acts on one
such sighting alone: at the bound a wait polls SETTLE_POLLS more times, SETTLE_SECONDS apart, before it refuses (a
probe ends within microseconds), and an uncapped build gives up its claim for a gate only when the gate stays held
across those polls (_gate_stays_closed), so another process's probe can neither refuse a free lock nor let a capped
build overtake a waiting uncapped one. A ticket is probed the same way, without creating it; a probe can make a dead
waiter's ticket look live for that moment (it is skipped at the next poll), never a live one look dead.
"""

from __future__ import annotations

import argparse
import contextlib
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
from collections.abc import Callable, Iterator
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
# The queue (first come, first served): the tickets' directory and the file whose lock is the queue's claim and whose
# record is the last sequence given out.
QUEUE_DIRECTORY_NAME = "queue"
QUEUE_CLAIM_NAME = "queue.lock"
TICKET_SUFFIX = ".ticket"
SEQUENCE_DIGITS = 12
# The claim is held for a few file operations, so a request tries it this many times, this far apart (about 2 s), and
# otherwise tries again at its next poll; removing dead tickets tries it once.
QUEUE_CLAIM_TRIES = 400
QUEUE_CLAIM_PAUSE_SECONDS = 0.005

DEFAULT_WAIT_SECONDS = 3 * 3600.0
POLL_SECONDS = 0.25
# A lock found held may be another process's probe of a free lock (is_held, a waiter's attempt), which lasts
# microseconds: before a wait refuses, and before a waiting uncapped build gives up its claim for a gate, the lock is
# polled this many more times, this far apart.
SETTLE_POLLS = 4
SETTLE_SECONDS = 0.05
REPORT_SECONDS = 300.0
GIT_TIMEOUT_SECONDS = 60.0
# The records' "since" (UTC).
TIME_FORMAT = "%Y-%m-%dT%H:%M:%SZ"
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


@dataclasses.dataclass(frozen=True)
class Waiter:
    """The record a queued request writes at the start of its ticket: the lock and mode it asks for (EXCLUSIVE for
    gate and gpu; CAPPED, UNCAPPED or AUTO for build), its place (sequence), who asks (pid, worktree, command), since
    when it waits, and the holds it runs under (its ENGINE_HELD_LOCKS tokens, by lock)."""

    lock: str
    mode: str
    sequence: int
    pid: int
    worktree: str
    command: str
    since: str  # UTC, ISO 8601
    tokens: dict[str, str] = dataclasses.field(default_factory=dict)

    def describe(self) -> str:
        mode = f" ({self.mode})" if self.lock == BUILD else ""
        return f"pid {self.pid}{mode} in {self.worktree} waiting since {self.since}: {self.command}"

    def waited_seconds(self, now: datetime.datetime | None = None) -> float:
        """How long it has waited at `now` (UTC; default: now), from its record's since; 0 when that is unreadable."""
        try:
            began = datetime.datetime.strptime(self.since, TIME_FORMAT).replace(tzinfo=datetime.timezone.utc)
        except ValueError:
            return 0.0
        moment = now or datetime.datetime.now(datetime.timezone.utc)
        return max(0.0, (moment - began).total_seconds())

    def to_json(self) -> dict[str, Any]:
        return {"lock": self.lock, "mode": self.mode, "sequence": self.sequence, "pid": self.pid,
                "worktree": self.worktree, "command": self.command, "since": self.since}


def describe_blockers(holders: list[Holder], waiting: list[Waiter]) -> str:
    """Who keeps a request from a lock, worded to follow "the <lock> lock is held by": its holders ("another process"
    when none has written its record, "nobody" when only queued requests are in the way), then the earlier queued
    requests that go first."""
    held_by = "; ".join(holder.describe() for holder in holders) or ("nobody" if waiting else "another process")
    if not waiting:
        return held_by
    count = len(waiting)
    earlier = "1 earlier request goes" if count == 1 else f"{count} earlier requests go"
    return f"{held_by}, and {earlier} first ({waiting[0].describe()}{', ...' if count > 1 else ''})"


class LockUnavailable(RuntimeError):
    """A lock stayed held by other processes, or earlier requests queued for it stayed ahead: refused at once (timeout
    0) or until the wait ran out."""

    def __init__(self, lock: str, holders: list[Holder], waited: float, timed_out: bool,
                 waiting: list[Waiter] | None = None) -> None:
        self.lock = lock
        self.holders = holders
        self.waiting = list(waiting or [])
        self.waited = waited
        self.timed_out = timed_out
        outcome = f"gave up after waiting {waited:.0f} s" if timed_out else "not waiting"
        super().__init__(f"the {lock} lock is held by {describe_blockers(holders, self.waiting)} ({outcome})")

    @property
    def exit_code(self) -> int:
        return EXIT_TIMEOUT if self.timed_out else EXIT_INIT_FAILED

    def to_json(self) -> dict[str, Any]:
        return {"error": "LockUnavailable", "lock": self.lock, "holders": [holder.to_json() for holder in self.holders],
                "waiting": [waiter.to_json() for waiter in self.waiting], "waitedSeconds": round(self.waited, 1),
                "timedOut": self.timed_out}


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


def queue_directory() -> Path:
    """The directory of the queued requests' tickets."""
    return lock_directory() / QUEUE_DIRECTORY_NAME


def queue_claim_path() -> Path:
    return lock_directory() / QUEUE_CLAIM_NAME


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

    def write_record(self, record: Holder | Waiter) -> None:
        self.write_text(json.dumps(dataclasses.asdict(record), sort_keys=True) + "\n")

    def write_text(self, text: str) -> None:
        """Replaces the file's content (the record before the locked byte) with `text`; the lock must be held."""
        assert self.descriptor is not None
        os.lseek(self.descriptor, 0, os.SEEK_SET)
        os.ftruncate(self.descriptor, 0)
        os.write(self.descriptor, text.encode("utf-8"))

    def read_text(self) -> str:
        """The file's content (at most RECORD_LIMIT bytes) read through the held lock's handle."""
        assert self.descriptor is not None
        os.lseek(self.descriptor, 0, os.SEEK_SET)
        return os.read(self.descriptor, RECORD_LIMIT).decode("utf-8", errors="replace")

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
# The queue: tickets
# --------------------------------------------------------------------------------------------------------------------


def _ticket_name(lock: str, sequence: int) -> str:
    return f"{lock}-{sequence:0{SEQUENCE_DIGITS}d}{TICKET_SUFFIX}"


def _ticket_sequence(name: str, lock: str | None) -> int | None:
    """The sequence of the ticket file `name` (of `lock`, or of any lock when None); None for another file."""
    if not name.endswith(TICKET_SUFFIX):
        return None
    stem = name[:-len(TICKET_SUFFIX)]
    owner, _, digits = stem.rpartition("-")
    if (lock is not None and owner != lock) or owner not in LOCK_NAMES or not digits.isdigit():
        return None
    return int(digits)


def _probe_ticket(path: Path) -> bool | None:
    """Whether the ticket at `path` is held (its waiter lives): True held, False free (its waiter is dead, or is just
    creating or leaving it), None when it is gone. Unlike is_held it never creates the file."""
    try:
        descriptor = os.open(path, os.O_RDWR | getattr(os, "O_BINARY", 0))
    except FileNotFoundError:
        return None
    except PermissionError:
        return None  # Windows: a file whose deletion is pending
    try:
        if not _lock_descriptor(descriptor):
            return True
        _unlock_descriptor(descriptor)
        return False
    finally:
        os.close(descriptor)


def read_waiter(path: Path, lock: str) -> Waiter | None:
    """The waiter record at the start of the ticket `path`; None when there is none or it is incomplete."""
    try:
        with path.open("rb") as stream:
            data = stream.read(RECORD_LIMIT)
    except OSError:
        return None
    try:
        document = json.loads(data.split(b"\n", 1)[0].decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(document, dict) or document.get("lock") != lock:
        return None
    integers = ("sequence", "pid")
    if not all(isinstance(document.get(name), int) and not isinstance(document.get(name), bool) for name in integers):
        return None
    texts = ("mode", "worktree", "command", "since")
    if not all(isinstance(document.get(name), str) for name in texts):
        return None
    if document["mode"] not in (BUILD_MODES if lock == BUILD else (EXCLUSIVE,)):
        return None
    tokens = document.get("tokens", {})
    if not isinstance(tokens, dict) or not all(isinstance(key, str) and isinstance(value, str)
                                               for key, value in tokens.items()):
        return None
    return Waiter(lock, document["mode"], document["sequence"], document["pid"], document["worktree"],
                  document["command"], document["since"], dict(tokens))


def _take_queue_claim(tries: int = QUEUE_CLAIM_TRIES) -> LockFile | None:
    """The queue's claim (queue.lock), tried `tries` times QUEUE_CLAIM_PAUSE_SECONDS apart; None when another process
    kept it throughout."""
    claim = LockFile(queue_claim_path())
    for attempt in range(tries):
        if claim.try_lock():
            return claim
        if attempt + 1 < tries:
            time.sleep(QUEUE_CLAIM_PAUSE_SECONDS)
    return None


def _remove_dead_tickets(tickets: list[Path]) -> None:
    """Removes the tickets of `tickets` that are still free under the queue's claim: a ticket is created and locked
    under the claim, so one found free there belongs to a dead waiter (or to one that is leaving it). Tries the claim
    once; the next reader removes them otherwise."""
    claim = _take_queue_claim(tries=1)
    if claim is None:
        return
    try:
        for path in tickets:
            if _probe_ticket(path) is False:
                with contextlib.suppress(OSError):
                    path.unlink()
    finally:
        claim.unlock(clear=False)


def waiters(lock: str, tidy: bool = True) -> list[Waiter]:
    """The live requests queued for `lock`, earliest first. Skips the tickets of dead waiters (and, when `tidy`,
    removes them) and the ones whose record cannot be read: a ticket being created, whose request comes after every
    listed one, or one being left, whose request has ended."""
    try:
        names = os.listdir(queue_directory())
    except OSError:
        return []
    found: list[Waiter] = []
    dead: list[Path] = []
    for name in names:
        sequence = _ticket_sequence(name, lock)
        if sequence is None:
            continue
        path = queue_directory() / name
        held = _probe_ticket(path)
        if held is False:
            dead.append(path)
        elif held:
            waiter = read_waiter(path, lock)
            if waiter is not None and waiter.sequence == sequence:
                found.append(waiter)
    if dead and tidy:
        _remove_dead_tickets(dead)
    return sorted(found, key=lambda waiter: waiter.sequence)


class _Ticket:
    """This process's place in a lock's queue: a ticket file it holds locked until it leaves."""

    def __init__(self, file: LockFile, waiter: Waiter) -> None:
        self.file: LockFile | None = file
        self.waiter = waiter

    @property
    def sequence(self) -> int:
        return self.waiter.sequence

    def leave(self) -> None:
        """Gives the place up: the ticket's lock and then the file (a reader's probe that keeps it open a moment leaves
        a free ticket, which the next reader removes)."""
        file, self.file = self.file, None
        if file is None:
            return
        file.unlock()
        with contextlib.suppress(OSError):
            file.path.unlink()


def _join_queue(lock: str, mode: str, command: str | None) -> _Ticket | None:
    """A new ticket for a request of `lock` in `mode`, after every ticket given out so far: its sequence is one more
    than the claim's counter and every ticket present, and it is created, locked and recorded under the claim. None when
    the claim, or the new ticket's lock (which a reader's probe can hold for a moment), stayed busy: the request tries
    again at its next poll."""
    claim = _take_queue_claim()
    if claim is None:
        return None
    try:
        counter = claim.read_text().split("\n", 1)[0].strip()
        highest = int(counter) if counter.isdigit() else 0
        try:
            names = os.listdir(queue_directory())
        except OSError:
            names = []
        for name in names:
            highest = max(highest, _ticket_sequence(name, None) or 0)
        sequence = highest + 1
        file = LockFile(queue_directory() / _ticket_name(lock, sequence))
        for attempt in range(QUEUE_CLAIM_TRIES):
            if file.try_lock():
                break
            if attempt + 1 < QUEUE_CLAIM_TRIES:
                time.sleep(QUEUE_CLAIM_PAUSE_SECONDS)
        else:
            return None
        shown = command or format_command([Path(sys.executable).name, *sys.argv])
        since = datetime.datetime.now(datetime.timezone.utc).strftime(TIME_FORMAT)
        waiter = Waiter(lock, mode, sequence, os.getpid(), paths.REPOSITORY_ROOT.as_posix(), shown, since,
                        inherited_tokens())
        ticket = _Ticket(file, waiter)
        try:
            file.write_record(waiter)
            claim.write_text(f"{sequence}\n")
        except BaseException:
            ticket.leave()
            raise
        return ticket
    finally:
        claim.unlock(clear=False)


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
class _Place:
    """A place this process keeps in a lock's queue across requests (queued()): its ticket (None until one could be
    taken) and how many blocks keep it."""

    lock: str
    mode: str
    command: str | None
    ticket: _Ticket | None = None
    count: int = 1


@dataclasses.dataclass
class _Registry:
    """The locks this process holds and the places it keeps in their queues, by name; `guard` serializes their changes
    (the bridge releases a hold on the thread that watches its editor)."""

    guard: threading.RLock = dataclasses.field(default_factory=threading.RLock)
    owned: dict[str, _Ownership] = dataclasses.field(default_factory=dict)
    places: dict[str, _Place] = dataclasses.field(default_factory=dict)


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


def _gate_closed() -> bool:
    """Whether a gate runs, or a CI.py queues for the gate: either way new gpu and build requests outside a held run
    wait, so the gate goes first."""
    return is_held(lock_paths(GATE)[0]) or bool(waiters(GATE))


def _gate_open() -> bool:
    """Whether a gate-respecting acquisition may proceed: no gate runs or queues (_gate_closed), or this process or an
    ancestor holds any machine lock. A gate waits for the runs that hold a lock when it begins (CI.py takes the gpu and
    build locks after the gate), so a process inside such a run finishes it instead of waiting for that gate, which
    would deadlock."""
    registry = _registry()
    with registry.guard:
        if registry.owned:
            return True
    if not _gate_closed():
        return True
    return any(inherited_holder(lock) is not None for lock in LOCK_NAMES)


def _running_holds() -> list[Holder]:
    """The records of every held lock file (the gate, the gpu lock and the build slots): the runs in progress."""
    found: list[Holder] = []
    for lock in LOCK_NAMES:
        for path in lock_paths(lock):
            holder = current_holder(path, lock)
            if holder is not None:
                found.append(holder)
    return found


def _runs_inside(waiter: Waiter, running: list[Holder]) -> bool:
    """Whether the queued `waiter` runs inside a run that holds a machine lock (`running`): its own process holds one,
    or an ancestor's hold whose token it carries, so a gate does not hold it back (_gate_open, from its side)."""
    return any(holder.pid == waiter.pid or waiter.tokens.get(holder.lock) == holder.token for holder in running)


def _earlier_requests(lock: str, sequence: int | None) -> list[Waiter]:
    """The requests queued for `lock` before the ticket `sequence` (all of them for a request without one, which comes
    after every queued request) that can take it now, earliest first: for gpu and build while a gate runs or queues,
    only the ones that run inside a held run (_runs_inside); the others wait for the gate, so they go after the
    requests the gate lets through."""
    queued = [waiter for waiter in waiters(lock) if sequence is None or waiter.sequence < sequence]
    if not queued or lock == GATE or not _gate_closed():
        return queued
    running = _running_holds()
    return [waiter for waiter in queued if _runs_inside(waiter, running)]


@contextlib.contextmanager
def queued(lock: str, command: str | None = None, mode: str = EXCLUSIVE) -> Iterator[None]:
    """Keeps one place in `lock`'s queue for this process during the block (taken at its start, given up at its end),
    from which every request of `lock` made in it takes its turn, its no-wait tries too: a caller that waits by trying
    the lock without waiting (the MCP bridge's cancellable wait for the GPU lock) keeps its place across the tries,
    instead of coming after every queued request at each of them. `mode` is the build request's mode (CAPPED, UNCAPPED
    or AUTO) for the build lock, EXCLUSIVE otherwise. Blocks nest; the outermost one gives the place up."""
    if lock not in LOCK_NAMES:
        raise ValueError(f"unknown lock '{lock}' (expected {', '.join(LOCK_NAMES)})")
    if mode not in (BUILD_MODES if lock == BUILD else (EXCLUSIVE,)):
        raise ValueError(f"the {lock} lock is not requested in mode '{mode}'")
    registry = _registry()
    with registry.guard:
        place = registry.places.get(lock)
        if place is None:
            place = registry.places[lock] = _Place(lock, mode, command)
        else:
            place.count += 1
    try:
        _place_ticket(place)
        yield
    finally:
        with registry.guard:
            place.count -= 1
            ticket = None
            if place.count == 0:
                if registry.places.get(lock) is place:
                    del registry.places[lock]
                ticket, place.ticket = place.ticket, None
        if ticket is not None:
            ticket.leave()


def _place_ticket(place: _Place) -> _Ticket | None:
    """The ticket of `place`, taken now when it has none yet (None when the queue's claim stayed busy)."""
    registry = _registry()
    with registry.guard:
        if place.ticket is not None:
            return place.ticket
    ticket = _join_queue(place.lock, place.mode, place.command)
    with registry.guard:
        if place.ticket is None and place.count > 0:
            place.ticket = ticket
            return ticket
        current = place.ticket
    if ticket is not None:
        ticket.leave()
    return current


def _new_holder(lock: str, mode: str, token: str, command: str | None) -> Holder:
    shown = command or format_command([Path(sys.executable).name, *sys.argv])
    since = datetime.datetime.now(datetime.timezone.utc).strftime(TIME_FORMAT)
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
    """Why one poll could not take the lock: the lock that is in the way, its holders and the earlier queued requests
    that go first."""

    lock: str
    holders: list[Holder]
    waiting: list[Waiter] = dataclasses.field(default_factory=list)


class _Request:
    """One request for a lock: its polls (poll(), which subclasses define), and once it waits its place in the lock's
    queue: the place this process keeps (queued()) or a ticket of its own."""

    def __init__(self, lock: str, mode: str, command: str | None) -> None:
        self.lock = lock
        self.mode = mode
        self.command = command
        self.ticket: _Ticket | None = None

    def poll(self) -> Hold | _Blocked:
        raise NotImplementedError

    def sequence(self) -> int | None:
        """The request's place in the queue: its own ticket's, or the place this process keeps; None without one (it
        comes after every queued request)."""
        if self.ticket is not None:
            return self.ticket.sequence
        registry = _registry()
        with registry.guard:
            place = registry.places.get(self.lock)
            ticket = place.ticket if place is not None else None
        return ticket.sequence if ticket is not None else None

    def join(self, waits: bool) -> bool:
        """Takes the request's place in the queue, if it has none: the place this process keeps for the lock (taken now
        if it could not be at the block's start), else, for a request that `waits`, a ticket of its own. False when the
        queue's claim stayed busy (the next poll tries again)."""
        registry = _registry()
        with registry.guard:
            place = registry.places.get(self.lock)
        if place is not None:
            return _place_ticket(place) is not None
        if waits and self.ticket is None:
            self.ticket = _join_queue(self.lock, self.mode, self.command)
            return self.ticket is not None
        return True

    def close(self) -> None:
        """Ends the request: its own ticket goes (a place this process keeps stays until its block ends)."""
        ticket, self.ticket = self.ticket, None
        if ticket is not None:
            ticket.leave()


def _wait(description: str, request: _Request, timeout: float, report: Reporter) -> Hold:
    """Polls `request` until it returns a hold or `timeout` seconds have passed, reporting the holders and the earlier
    requests when the wait starts, when they change and every REPORT_SECONDS. A request that waits joins the lock's
    queue at its first refused poll, before it reports. At the bound it polls SETTLE_POLLS more times, SETTLE_SECONDS
    apart (a lock that looked held may have been another process's probe), then raises LockUnavailable. However it
    ends, the request is closed: its ticket and what its polls kept (a build claim) go back."""
    started = time.monotonic()
    deadline = started + max(0.0, timeout)
    shown: tuple[object, ...] | None = None
    next_report = started
    try:
        while True:
            outcome = request.poll()
            if isinstance(outcome, Hold):
                if shown is not None:
                    report(f"Took the {description} after waiting {time.monotonic() - started:.0f} s")
                return outcome
            now = time.monotonic()
            # The place this process keeps in the lock's queue is taken here even for a refusal, when the block could
            # not take it at its start; a request that refuses at once takes no ticket of its own.
            joined = request.join(waits=timeout > 0)
            if now >= deadline:
                for _ in range(SETTLE_POLLS):
                    time.sleep(SETTLE_SECONDS)
                    outcome = request.poll()
                    if isinstance(outcome, Hold):
                        return outcome
                raise LockUnavailable(outcome.lock, outcome.holders, time.monotonic() - started,
                                      timed_out=timeout > 0, waiting=outcome.waiting)
            if joined:
                current = (outcome.lock, tuple(holder.describe() for holder in outcome.holders),
                           tuple(waiter.sequence for waiter in outcome.waiting))
                if current != shown or now >= next_report:
                    report(f"Waiting for the {description} ({now - started:.0f} s of at most {timeout:.0f} s): the "
                           f"{outcome.lock} lock is held by {describe_blockers(outcome.holders, outcome.waiting)}")
                    shown = current
                    next_report = now + REPORT_SECONDS
            time.sleep(min(POLL_SECONDS, max(0.0, deadline - now)))
    finally:
        request.close()


def _gate_stays_closed() -> bool:
    """Whether the gate, found closed (_gate_open), stays closed across SETTLE_POLLS probes SETTLE_SECONDS apart: a
    single sighting may be another process's probe of a free gate."""
    for _ in range(SETTLE_POLLS):
        time.sleep(SETTLE_SECONDS)
        if _gate_open():
            return False
    return True


def _gate_blocked() -> _Blocked:
    return _Blocked(GATE, holders(GATE), waiters(GATE))


class _GateWait(_Request):
    """The polls of a wait for the end of the running gate: it takes no lock, so it never queues."""

    def __init__(self) -> None:
        super().__init__(GATE, EXCLUSIVE, None)

    def poll(self) -> Hold | _Blocked:
        return Hold(GATE, EXCLUSIVE, inherited=True) if _gate_open() else _gate_blocked()

    def join(self, waits: bool) -> bool:
        return True


def wait_for_gate(timeout: float, report: Reporter) -> None:
    """Returns once no gate runs or queues (or this process or an ancestor runs it); raises LockUnavailable
    otherwise."""
    _wait("end of the running gate", _GateWait(), timeout, report)


def acquire_gate(timeout: float, report: Reporter, command: str | None = None) -> Hold:
    """The exclusive gate lock (CI.py, for its whole run), in the order the gate's requests began to wait."""
    return _wait(f"{GATE} lock", _ExclusiveRequest(GATE, command, respect_gate=False), timeout, report)


def acquire_gpu(timeout: float, report: Reporter, command: str | None = None) -> Hold:
    """The exclusive GPU lock, in the order its requests began to wait; it waits while a gate runs or queues, unless
    this process or an ancestor runs it."""
    return _wait(f"{GPU} lock", _ExclusiveRequest(GPU, command, respect_gate=True), timeout, report)


class _ExclusiveRequest(_Request):
    """The polls of one gate or gpu request: it takes the lock when no earlier queued request that can take it now is
    waiting (_earlier_requests)."""

    def __init__(self, lock: str, command: str | None, respect_gate: bool) -> None:
        super().__init__(lock, EXCLUSIVE, command)
        self.respect_gate = respect_gate

    def poll(self) -> Hold | _Blocked:
        nested = _nested(self.lock, EXCLUSIVE)
        if nested is not None:
            return nested
        if self.respect_gate and not _gate_open():
            return _gate_blocked()
        earlier = _earlier_requests(self.lock, self.sequence())
        if earlier:
            return _Blocked(self.lock, holders(self.lock), earlier)
        file = LockFile(lock_paths(self.lock)[0])
        if not file.try_lock():
            return _Blocked(self.lock, holders(self.lock))
        if self.respect_gate and not _gate_open():
            file.unlock(clear=False)
            return _gate_blocked()
        return _own(self.lock, EXCLUSIVE, secrets.token_hex(16), [file], self.command)


def acquire_build(requested: str, timeout: float, report: Reporter, command: str | None = None) -> Hold:
    """The build lock in `requested` mode: UNCAPPED takes both slots (after the claim, which holds off older checkouts'
    new capped builds), CAPPED one slot, AUTO both slots when no build holds the lock and one slot beside a single
    capped build. The slots are given in the order the requests began to wait (_BuildAttempt). The hold's mode is what
    the build may use. It waits while a gate runs or queues, unless this process or an ancestor runs it."""
    if requested not in BUILD_MODES:
        raise ValueError(f"unknown build mode '{requested}' (expected {', '.join(BUILD_MODES)})")
    return _wait("build lock", _BuildAttempt(requested, command), timeout, report)


@dataclasses.dataclass(frozen=True)
class _Slots:
    """The build slots a request can still take: how many are free, and whether every other one is (or goes to) a
    capped build, beside which an automatic build may take one."""

    free: int
    beside_capped: bool

    def wanted(self, mode: str) -> int:
        """The slots a request in `mode` takes from these: both (UNCAPPED, or AUTO on a free lock), one (CAPPED, or AUTO
        beside capped builds), or 0 when it cannot start yet."""
        if mode == UNCAPPED:
            return BUILD_SLOTS if self.free == BUILD_SLOTS else 0
        if mode == CAPPED:
            return 1 if self.free > 0 else 0
        if self.free == BUILD_SLOTS:
            return BUILD_SLOTS
        return 1 if self.free > 0 and self.beside_capped else 0

    def after(self, taken: int) -> _Slots:
        """What is left once a request took `taken` slots (one slot is a capped build's)."""
        return _Slots(self.free - taken, self.beside_capped)


def _build_slots() -> _Slots:
    """The free build slots, and whether every held one is a capped build's (a slot held without a record, taken a
    moment ago or probed, is not known to be)."""
    free = 0
    beside_capped = True
    for path in lock_paths(BUILD):
        if not is_held(path):
            free += 1
            continue
        record = read_record(path, BUILD)
        beside_capped = beside_capped and record is not None and record.mode == CAPPED
    return _Slots(free, beside_capped)


class _BuildAttempt(_Request):
    """The polls of one build lock request. The earlier queued requests take their slots first, oldest first, and the
    first one that cannot start yet stops every later one, so the slots are given in arrival order and a waiting
    uncapped request is never overtaken. An uncapped request at the head of the queue also keeps build-claim.lock
    between polls, which holds off the capped builds of checkouts whose locks.py does not read the queue."""

    def __init__(self, requested: str, command: str | None) -> None:
        super().__init__(BUILD, requested, command)
        self.requested = requested
        self.claim: LockFile | None = None
        self.token = secrets.token_hex(16)

    def abandon(self) -> None:
        if self.claim is not None:
            self.claim.unlock()
            self.claim = None

    def close(self) -> None:
        self.abandon()
        super().close()

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
        earlier = _earlier_requests(BUILD, self.sequence())
        slots = _build_slots()
        for waiter in earlier:
            taken = slots.wanted(waiter.mode)
            if taken == 0:
                # An earlier request that cannot start yet goes first, and the claim is the head of the queue's.
                self.abandon()
                return _Blocked(BUILD, self.others(), earlier)
            slots = slots.after(taken)
        if self.requested == UNCAPPED:
            if earlier:
                self.abandon()
                return _Blocked(BUILD, self.others(), earlier)
            return self.poll_uncapped()
        wanted = slots.wanted(self.requested)
        if wanted == 0 or is_held(claim_path()):
            return _Blocked(BUILD, holders(BUILD), earlier)
        slot_paths = lock_paths(BUILD)
        files = [LockFile(path) for path in slot_paths]
        if wanted == 1:
            # One slot, a capped build's: beside a capped build, or beside the earlier request that takes the other. A
            # capped build prefers the last slot.
            for file in reversed(files):
                if file.try_lock():
                    return self.own(CAPPED, [file])
            return _Blocked(BUILD, holders(BUILD), earlier)
        # An automatic request on a free lock, with nobody queued before it, tries both slots.
        taken_files = [file for file in reversed(files) if file.try_lock()]
        if len(taken_files) == BUILD_SLOTS:
            return self.own(UNCAPPED, taken_files)
        if taken_files:
            other = next(path for path, file in zip(slot_paths, files) if file is not taken_files[0])
            beside = read_record(other, BUILD)
            if beside is not None and beside.mode == CAPPED:
                return self.own(CAPPED, taken_files)
            for file in taken_files:
                file.unlock(clear=False)
        return _Blocked(BUILD, holders(BUILD))

    def others(self) -> list[Holder]:
        """The build lock's holders and claimant other than this request."""
        return [holder for holder in holders(BUILD) if holder.token != self.token]

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
        return _Blocked(BUILD, self.others())

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
