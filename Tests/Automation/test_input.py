"""Input injection through automation (Docs/Architecture.md §4.3, §5.7 step 1, §13.5 "input", §13.6; Roadmap M7
acceptance; Docs/Decisions/0012-m7-decisions.md decision 4): tick-stamped events, taps, and what the game's step view
saw (play.state's "input").
"""

from __future__ import annotations

import json
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

    def test_trigger_bound_to_button_action_fires_and_releases_with_hysteresis(self) -> None:
        # M34 (Docs/Decisions/0049-m34-characters-input.md decision 5; World Tree's brief 10): attack on the trigger.
        client, _ = self.open_editor_with_scene()
        client.call("project.setSettings", {"patch": {"Input": {"Actions": {
            "Attack": {"Type": "Button", "Bindings": ["Gamepad.RightTrigger"]}}}}})
        client.call("play.start", {"lockstep": True})
        # (value, down, pressed, released) after a tick with the trigger at value.
        steps = [(0.45, False, False, False), (0.6, True, True, False), (0.45, True, False, False),
                 (0.35, False, False, True)]
        for value, down, pressed, released in steps:
            with self.subTest(value=value):
                client.call("play.step", {"ticks": 1, "input": [
                    {"tick": 0, "type": "gamepadAxis", "gamepad": 0, "axis": "RightTrigger", "value": value}]})
                seen = client.call("play.state")["input"]
                self.assertEqual("Action.Attack" in seen["down"], down, seen)
                self.assertEqual("Action.Attack" in seen["pressed"], pressed, seen)
                self.assertEqual("Action.Attack" in seen["released"], released, seen)

    def test_set_settings_refuses_an_invalid_binding_and_writes_nothing(self) -> None:
        # M34 (decision 6): play.start refused it, one step later than it should.
        client, root = self.open_editor_with_scene()
        project_file = root / f"{root.name}.eproj"
        before = project_file.read_bytes()
        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("project.setSettings", {"patch": {"Input": {"Actions": {
                "Look": {"Type": "Button", "Bindings": ["Gamepad.LeftY"]}}}}})
        self.assert_engine_error(raised.exception, engine_client.VALIDATION_FAILED, "Validation")
        issues = raised.exception.issues
        self.assertEqual([issue["pointer"] for issue in issues], ["/patch/Input/Actions/Look/Bindings/0"])
        self.assertIn("Gamepad.LeftY", issues[0]["message"])
        self.assertEqual(project_file.read_bytes(), before)
        self.assertNotIn("Look", client.call("project.getSettings")["settings"]["Input"]["Actions"])

    def test_validate_reports_an_invalid_binding_the_project_holds(self) -> None:
        # M34 (decision 7): a project saved before M34 with a binding play.start refuses.
        client, root = self.open_editor_with_scene()
        client.call("session.shutdown")
        self.assertEqual(self.editors[-1].wait(), 0)
        project_file = root / f"{root.name}.eproj"
        document = json.loads(project_file.read_text(encoding="utf-8"))
        document["Input"]["Actions"]["Look"] = {"Type": "Button", "Bindings": ["Key.Space", "Gamepad.RightX"],
                                                "Positive": [], "Negative": [], "Gamepad": "", "Invert": False}
        project_file.write_bytes((json.dumps(document, indent="\t") + "\n").encode("utf-8"))
        reopened = self.connect(self.start_editor(project=root))
        diagnostics = reopened.call("project.validate", {})["diagnostics"]
        invalid = [diagnostic for diagnostic in diagnostics if diagnostic["code"] == "INPUT_INVALID_ACTION"]
        self.assertEqual(len(invalid), 1, diagnostics)
        self.assertEqual(invalid[0]["field"], "Input.Actions.Look.Bindings[1]")
        self.assertEqual(invalid[0]["severity"], "Error")
        self.assertEqual(invalid[0]["file"], project_file.name)
        with self.assertRaises(engine_client.EngineError):
            reopened.call("play.start", {"lockstep": True})


if __name__ == "__main__":
    unittest.main()
