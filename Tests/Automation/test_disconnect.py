"""Client disconnects (Docs/Architecture.md §13.2 "Disconnect", Roadmap M4): pending operations are cancelled, through
the debug.pend test hook."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client


class DisconnectTests(AutomationTestCase):
    @staticmethod
    def poll_log(observer: engine_client.EngineClient, cursor: str, text: str) -> list[dict[str, object]]:
        """The log entries after `cursor` containing `text`, once there are any (at most 200 reads, one per request,
        each answered after at least one editor frame)."""
        entries: list[dict[str, object]] = []
        for _ in range(200):
            entries = observer.call("log.read", {"cursor": cursor, "contains": text})["entries"]
            if entries:
                break
        return entries

    @unittest.skip("contract stub: un-skipped by M4 stream D")
    def test_disconnect_cancels_pending_operations(self) -> None:
        editor = self.start_editor()
        observer = self.connect(editor, "observer")
        log_cursor = observer.call("log.read", {"cursor": "end"})["nextCursor"]
        event_cursor = observer.call("events.read", {"cursor": "end"})["nextCursor"]

        assert editor.session is not None
        doomed = engine_client.EngineClient.connect_session(editor.session, client_name="doomed")
        doomed.connection.sendall(engine_client.encode_frame(
            b'{"jsonrpc":"2.0","id":1,"method":"debug.pend","params":{"frames":0}}'))
        # Requests of different clients have no order, so wait until the operation is pending before disconnecting.
        started = self.poll_log(observer, log_cursor, "Started debug.pend")
        self.assertEqual(len(started), 1)
        doomed.connection.close()

        cancelled = self.poll_log(observer, log_cursor, "Cancelled debug.pend")
        self.assertEqual(len(cancelled), 1)
        self.assertIn("doomed", cancelled[0]["message"])
        events = observer.call("events.read",
                               {"cursor": event_cursor, "types": ["AutomationClientDisconnected"]})["events"]
        self.assertEqual([event["name"] for event in events], ["doomed"])
        self.assertEqual([client["name"] for client in observer.call("session.info")["clients"]], ["observer"])


if __name__ == "__main__":
    unittest.main()
