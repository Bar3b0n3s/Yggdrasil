#!/usr/bin/env python3
"""List the holders of the machine-wide locks (Docs/Architecture.md §2.3; Docs/Decisions/0028-backlog-program.md
decision 16; Scripts/Lib/locks.py).

The gate, gpu and build locks are shared by every checkout of the repository: their files are in the git common
directory (<common>/machine-locks/). For each lock this prints whether it is free or which processes hold it: pid,
worktree, command and since when; for build, each holder's mode (uncapped, capped, or the claim of an uncapped build
waiting for the running ones). It changes nothing: the operating system releases a lock when its holder's process ends,
so a lock is never cleared by hand; a holder that should not run any more is stopped like any process (its pid is
listed).

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

from Lib import locks, paths
from Lib.report import EXIT_SUCCESS, Console, configure_stdio, emit_json


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    parser = argparse.ArgumentParser(
        description="List the holders of the machine-wide gate, gpu and build locks (Architecture §2.3).",
        epilog="Exit codes: 0 listed, 2 usage error.",
    )
    parser.add_argument("--json", action="store_true", help="print a machine-readable result on stdout")
    arguments = parser.parse_args(sys.argv[1:] if argv is None else argv)
    paths.enter_repository_root()
    console = Console(arguments.json)

    directory = locks.lock_directory()
    console.heading(f"Machine-wide locks in {directory.as_posix()}")
    found = {lock: locks.holders(lock) for lock in locks.LOCK_NAMES}
    for lock, holders in found.items():
        if not holders:
            console.print(f"{lock}: free")
        for holder in holders:
            console.print(f"{lock}: held by {holder.describe()}")
    if arguments.json:
        emit_json({"success": True, "directory": directory.as_posix(), "worktree": paths.REPOSITORY_ROOT.as_posix(),
                   "locks": {lock: [holder.to_json() for holder in holders] for lock, holders in found.items()}})
    return EXIT_SUCCESS


if __name__ == "__main__":
    sys.exit(main())
