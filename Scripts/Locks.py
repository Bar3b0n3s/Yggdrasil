#!/usr/bin/env python3
"""List the holders and the queues of the machine-wide locks (Docs/Architecture.md §2.3; Docs/Decisions/
0028-backlog-program.md decision 16; Scripts/Lib/locks.py).

The gate, gpu and build locks are shared by every checkout of the repository: their files are in the git common
directory (<common>/machine-locks/). For each lock this prints whether it is free or which processes hold it: pid,
worktree, command and since when; for build, each holder's mode (uncapped, capped, or the claim of an uncapped build
waiting for the running ones). Then it lists the lock's queue, the requests waiting for it in arrival order (first come,
first served; while a gate runs or queues, a request inside a held run goes first): each waiter's pid, worktree,
command, since when it waits and for how long; for build, the mode it asks for. It changes nothing: the operating
system releases a lock, and a waiter's place, when its process ends, so neither is ever cleared by hand; a holder
or waiter that should not run any more is stopped like any process (its pid is listed). A dead waiter's ticket is
never listed; the next waiter that lists the queue removes it.

It works from any directory (Docs/Architecture.md §2.3): once it has parsed its arguments, main() makes the repository
root of its own checkout the working directory.

Exit codes: 0 listed (whether the locks are held or not), 2 usage error.
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"Locks.py requires Python 3.10 or newer (this is Python {platform.python_version()})")

import argparse
import datetime

from Lib import locks, paths
from Lib.report import EXIT_SUCCESS, Console, configure_stdio, emit_json


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    parser = argparse.ArgumentParser(
        description="List the holders and the queues of the machine-wide gate, gpu and build locks (Architecture "
                    "§2.3).",
        epilog="Exit codes: 0 listed, 2 usage error.",
    )
    parser.add_argument("--json", action="store_true", help="print a machine-readable result on stdout")
    arguments = parser.parse_args(sys.argv[1:] if argv is None else argv)
    paths.enter_repository_root()
    console = Console(arguments.json)

    directory = locks.lock_directory()
    console.heading(f"Machine-wide locks in {directory.as_posix()}")
    found = {lock: locks.holders(lock) for lock in locks.LOCK_NAMES}
    queues = {lock: locks.waiters(lock, tidy=False) for lock in locks.LOCK_NAMES}
    now = datetime.datetime.now(datetime.timezone.utc)
    for lock, holders in found.items():
        if not holders:
            console.print(f"{lock}: free")
        for holder in holders:
            console.print(f"{lock}: held by {holder.describe()}")
        for position, waiter in enumerate(queues[lock], start=1):
            console.print(f"{lock}: waiting #{position} for {waiter.waited_seconds(now):.0f} s, {waiter.describe()}")
    if arguments.json:
        emit_json({"success": True, "directory": directory.as_posix(), "worktree": paths.REPOSITORY_ROOT.as_posix(),
                   "locks": {lock: [holder.to_json() for holder in holders] for lock, holders in found.items()},
                   "queues": {lock: [{**waiter.to_json(), "waitedSeconds": round(waiter.waited_seconds(now))}
                                     for waiter in waiting] for lock, waiting in queues.items()}})
    return EXIT_SUCCESS


if __name__ == "__main__":
    sys.exit(main())
