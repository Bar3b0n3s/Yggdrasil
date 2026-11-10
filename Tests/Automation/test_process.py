"""Scripts/Lib/process.py's terminate_tree (Docs/Architecture.md §2.3), which the timeout and Ctrl+C of every script's
runs use: it ends the process and its descendants and nothing else. On Windows the descendants come from the recorded
parent ids of process snapshots, a process counting only when it was created no earlier than the holder of its
parent's id (find_new_descendants): Windows never updates a recorded parent id and gives ids out again, so `taskkill
/T`, which follows the ids alone, could end another checkout's older process whose parent's id a process of the tree
now holds (Docs/Backlog.md, R-GPUCRASH's evidence). A real reuse of an id takes thousands of process creations on
Windows 11, so the selection is checked on a recorded table, and a real tree checks the rest. Waits for a process to end
are bounded by a generous deadline that only bounds a failure. No editor.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
import unittest

from harness import REPOSITORY_ROOT

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

from Lib import process  # noqa: E402  (Scripts/ is on sys.path above)

DEADLINE_SECONDS = 60.0

# Prints its process id, starts the next level of the tree (argv[1] levels in all, this script as argv[2]) with the same
# standard output, and sleeps: a tree of argv[1] processes, each a child of the one before.
TREE_SCRIPT = (
    "import os, subprocess, sys, time\n"
    "print(os.getpid(), flush=True)\n"
    "if int(sys.argv[1]) > 1:\n"
    "    subprocess.Popen([sys.executable, '-c', sys.argv[2], str(int(sys.argv[1]) - 1), sys.argv[2]])\n"
    "time.sleep(600)\n"
)


class ProcessWatch:
    """Tells when a process that is not this one's child has ended: on Windows through a handle opened while it runs, so
    its id cannot change hands meanwhile; elsewhere by signalling it (a process that ended is reaped by init)."""

    def __init__(self, process_id: int) -> None:
        self.process_id = process_id
        self.handle = None
        if os.name == "nt":
            import ctypes  # noqa: PLC0415  (Windows only)
            from ctypes import wintypes  # noqa: PLC0415

            self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            self.kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
            self.kernel32.OpenProcess.restype = wintypes.HANDLE
            self.kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
            self.kernel32.WaitForSingleObject.restype = wintypes.DWORD
            self.kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
            synchronize = 0x00100000
            self.handle = self.kernel32.OpenProcess(synchronize, False, process_id)
            if not self.handle:
                raise OSError(ctypes.get_last_error(), f"cannot open process {process_id}")

    def ended_within(self, seconds: float) -> bool:
        if self.handle is not None:
            return self.kernel32.WaitForSingleObject(self.handle, int(seconds * 1000)) == 0
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                os.kill(self.process_id, 0)
            except ProcessLookupError:
                return True
            time.sleep(0.05)
        return False

    def close(self) -> None:
        if self.handle is not None:
            self.kernel32.CloseHandle(self.handle)
            self.handle = None


class TerminateTreeTests(unittest.TestCase):
    def test_terminate_tree_ends_the_process_and_its_descendants(self) -> None:
        # Started as run_captured and run_streamed start their processes (a process group of its own on POSIX).
        root = subprocess.Popen([sys.executable, "-c", TREE_SCRIPT, "3", TREE_SCRIPT], stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, **process._popen_options())
        self.addCleanup(process.terminate_tree, root)
        assert root.stdout is not None
        self.addCleanup(root.stdout.close)
        # The interpreters print their ids; below a virtual environment each level is the environment's launcher and the
        # interpreter it starts, so the root's own id may be the launcher's, and every printed one is a descendant.
        ids = [int(root.stdout.readline()) for _ in range(3)]
        watches = [ProcessWatch(process_id) for process_id in ids]
        for watch in watches:
            self.addCleanup(watch.close)

        process.terminate_tree(root)

        self.assertIsNotNone(root.returncode)
        for watch in watches:
            self.assertTrue(watch.ended_within(DEADLINE_SECONDS), f"process {watch.process_id} still runs")

    def test_a_process_older_than_the_holder_of_its_parents_id_is_no_descendant(self) -> None:
        # The root, 100, was created at tick 1000; 200 (created in the same tick) and 300 are its child and grandchild.
        # 400 names 100 as its parent but is older: an earlier holder of the id 100 started it, so it is another
        # program's, and so is its child 500. 600 is gone when it is opened, and 700 holds an id whose process in the
        # snapshot is gone (it was created after the snapshot).
        parents = {100: 4, 200: 100, 300: 200, 400: 100, 500: 400, 600: 200, 700: 300}
        created: dict[int, int | None] = {200: 1000, 300: 1200, 400: 900, 500: 950, 600: None, 700: 5000}
        members = {100: 1000}

        added = process.find_new_descendants(parents, members, created.get, 2000)

        self.assertEqual(added, [200, 300])
        self.assertEqual(members, {100: 1000, 200: 1000, 300: 1200})
        # The next snapshot finds what the tree started meanwhile, and nothing a second time.
        parents[800] = 300
        created[800] = 2500
        self.assertEqual(process.find_new_descendants(parents, members, created.get, 3000), [800])


if __name__ == "__main__":
    unittest.main()
