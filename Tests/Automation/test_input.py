"""Input injection through automation (Docs/Architecture.md §4.3, §5.7 step 1, §13.5 "input", §13.6; Roadmap M7
acceptance): tick-stamped events, taps, and what the game's step view saw (play.state's "input").

Skipped skeletons of the M7 contract (Docs/Decisions/0012-m7-decisions.md decision 4): stream A implements and registers
input.inject and removes the skips.
"""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client


class InputTests(AutomationTestCase):
    """input.inject and play.step's input in a lockstep session."""

    def start_lockstep_with_jump(self, client: engine_client.EngineClient) -> None:
        """The action Jump (Key.Space) in the project's settings, then a lockstep session of the open scene."""
        client.call("project.setSettings", {"patch": {"Input": {"Actions": {
            "Jump": {"Type": "Button", "Bindings": ["Key.Space"]}}}}})
        client.call("play.start", {"lockstep": True})

    @unittest.skip("contract stub: un-skipped by M7 stream A")
    def test_tap_event_seen_once_pressed_and_released(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.start_lockstep_with_jump(client)
        client.call("play.step", {"ticks": 1, "input": [{"tick": 0, "type": "action", "name": "Jump", "state": "tap"}]})
        first = client.call("play.state")["input"]
        self.assertEqual(first["pressed"], ["Action.Jump"])
        self.assertEqual(first["down"], ["Action.Jump"])
        self.assertEqual(first["released"], [])

        client.call("play.step", {"ticks": 1})
        second = client.call("play.state")["input"]
        self.assertEqual(second["pressed"], [])
        self.assertEqual(second["released"], ["Action.Jump"])
        self.assertEqual(second["down"], [])

        client.call("play.step", {"ticks": 1})
        third = client.call("play.state")["input"]
        self.assertEqual((third["pressed"], third["released"], third["down"]), ([], [], []))

        # The same through input.inject, with a key: stamped relative to the next tick.
        injected = client.call("input.inject", {"events": [{"type": "KEY", "key": "Space", "state": "Tap"}]})
        self.assertEqual(injected["tick"], 3)
        client.call("play.step", {"ticks": 1})
        self.assertIn("Key.Space", client.call("play.state")["input"]["pressed"])

    @unittest.skip("contract stub: un-skipped by M7 stream A")
    def test_input_inject_rejects_invalid_events_before_queuing_any(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.start_lockstep_with_jump(client)
        invalid = [
            ([{"type": "action", "name": "Jmup"}], "/events/0/name"),
            ([{"type": "key", "key": "Space"}, {"type": "key", "key": "Spcae"}], "/events/1/key"),
            ([{"type": "action", "name": "Jump", "state": "down", "value": 1}], "/events/0/value"),
            ([{"type": "gamepadAxis", "axis": "LeftX", "value": 2}], "/events/0/value"),
        ]
        for events, pointer in invalid:
            with self.subTest(events=events):
                with self.assertRaises(engine_client.EngineError) as raised:
                    client.call("input.inject", {"events": events})
                self.assert_engine_error(raised.exception, engine_client.INVALID_PARAMS)
                self.assertEqual(raised.exception.issues[0]["pointer"], pointer)
        client.call("play.step", {"ticks": 1})
        self.assertEqual(client.call("play.state")["input"]["down"], [])

    @unittest.skip("contract stub: un-skipped by M7 stream A")
    def test_input_inject_needs_a_play_session(self) -> None:
        client, _ = self.open_editor_with_scene()
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("input.inject", {"events": []})
        self.assert_engine_error(raised.exception, engine_client.INVALID_STATE, "InvalidState")


if __name__ == "__main__":
    unittest.main()
