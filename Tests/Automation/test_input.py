"""Input injection through automation (Docs/Architecture.md §4.3, §5.7 step 1, §13.5 "input", §13.6; Roadmap M7
acceptance; Docs/Decisions/0012-m7-decisions.md decision 4): tick-stamped events, taps, and what the game's step view
saw (play.state's "input").
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

    def test_input_inject_queues_a_frame_time_signal_for_input_was_signalled(self) -> None:
        # M28 (Docs/Decisions/0043-m28-game-services.md decision 6): automation injects the signals a lockstep session
        # takes; Input.WasSignalled reads the tick's latch, and a script's live post is ignored in lockstep.
        client, _ = self.open_editor_with_scene()
        client.call("play.start", {"lockstep": True})
        injected = client.call("input.inject", {"events": [{"type": "signal", "name": "FrameTimeExceeded"}]})
        self.assertEqual((injected["tick"], injected["queued"]), (0, 1))
        client.call("play.step", {"ticks": 1})
        latched = client.call("script.eval", {"context": "play",
                                              "code": "return Input.WasSignalled('FrameTimeExceeded')"})
        self.assertEqual(latched["value"], True)
        posted = client.call("script.eval", {"context": "play", "code": "return Input.PostSignal('Live')"})
        self.assertEqual(posted["value"], False)
        with self.assertRaises(engine_client.EngineError) as caught:
            client.call("input.inject", {"events": [{"type": "signal"}]})
        self.assert_engine_error(caught.exception, -32602)
        client.call("play.stop")

    def test_input_inject_needs_a_play_session(self) -> None:
        client, _ = self.open_editor_with_scene()
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("input.inject", {"events": []})
        self.assert_engine_error(raised.exception, engine_client.INVALID_STATE, "InvalidState")


if __name__ == "__main__":
    unittest.main()
