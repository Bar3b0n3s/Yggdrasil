"""The automation watchdog (Docs/Architecture.md §13.2 "Watchdog", Roadmap M4), driven by the debug.stall test hook.

The watchdog's threshold is wall-clock time (5 s without a pump), so this test waits past it: the suite's one wall-clock
exception (Docs/Decisions/0008-m4-decisions.md decision 15), bounded by the stall it provokes.
"""

from __future__ import annotations

import threading
import time
import unittest

from harness import AutomationTestCase, engine_client

WATCHDOG_THRESHOLD_SECONDS = 5.0
STALL_MILLISECONDS = 10000


class WatchdogTests(AutomationTestCase):
    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_busy_watchdog_reports_phase(self) -> None:
        editor = self.start_editor()
        staller = self.connect(editor, "staller")
        observer = self.connect(editor, "observer")
        outcome: dict[str, dict[str, object]] = {}
        stalling = threading.Event()

        def stall() -> None:
            stalling.set()
            outcome["result"] = staller.call("debug.stall", {"ms": STALL_MILLISECONDS}, timeout=60.0)

        thread = threading.Thread(target=stall)
        thread.start()
        stalling.wait(10.0)
        # A request sent after the threshold is answered by the I/O thread at once; one sent before it would wait. This
        # wall-clock wait is the suite's one exception (Docs/Decisions/0008-m4-decisions.md decision 15).
        time.sleep(WATCHDOG_THRESHOLD_SECONDS + 1.0)
        with self.assertRaises(engine_client.EngineError) as raised:
            observer.call("session.info", timeout=2.0)
        thread.join()
        self.assertEqual(raised.exception.code, engine_client.BUSY)
        self.assertEqual(raised.exception.data["phase"], f"Debug:stall {STALL_MILLISECONDS} ms")
        self.assertGreaterEqual(raised.exception.data["stalledMilliseconds"], 5000)
        self.assertNotIn("_meta", raised.exception.data)
        self.assertEqual(outcome["result"]["stalledMs"], STALL_MILLISECONDS)
        observer.call("session.info")


if __name__ == "__main__":
    unittest.main()
