"""Play sessions through automation (Docs/Architecture.md §5.6, §13.5 "play", §13.6, §4.2; Roadmap M7 acceptance):
lockstep, the 50 ms per-frame budget of play.step, the state hash, the disconnect rule and the headless throttle.

Skipped skeletons of the M7 contract (Docs/Decisions/0012-m7-decisions.md): stream A implements and registers the play
methods and removes the skips. test_headless_play_without_lockstep_is_throttled measures ticks against wall-clock time,
a recorded wall-clock exception (ADR 0012 decision 3): it asserts only an upper bound with a generous margin, so a slow
machine passes the same way.
"""

from __future__ import annotations

import time
import unittest

from harness import AutomationTestCase, engine_client

SEED = 42
# A play.step this long cannot fit one 50 ms frame (PlayStepFrameBudget) on any machine: it would need less than 250 ns
# per tick. So the budget must split it across frames, and a budget that is ignored (every tick in one frame) or never
# used (one tick per frame) fails the frame count. Within MaxPlayStepTicks and play.step's 600 s timeout in Debug.
BUDGET_TICKS = 200_000
# The project FixedHz of the throttle test: far below the editor's own 60 Hz loop, so a session that ran at the editor's
# rate instead of the project's (ADR 0012 decision 3) exceeds the upper bound.
THROTTLED_FIXED_HZ = 10


class PlayTests(AutomationTestCase):
    """Lockstep sessions of a headless editor."""

    def create_ball(self, client: engine_client.EngineClient) -> None:
        """One entity in the open edit scene, so a session has something to copy."""
        client.call("entity.create", {"name": "Ball", "components": {"Transform": {"Translation": [0, 2, 0]}}})

    def start_lockstep(self, client: engine_client.EngineClient, seed: int = SEED) -> dict:
        """play.start in lockstep with `seed`; the edit scene is left as it is."""
        return client.call("play.start", {"lockstep": True, "seed": seed})

    @unittest.skip("contract stub: un-skipped by M7 stream A")
    def test_lockstep_600_ticks_render_last(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.create_ball(client)
        started = self.start_lockstep(client)
        self.assertEqual(started["state"], "Play")
        self.assertEqual(started["mode"], "Play")
        self.assertTrue(started["lockstep"])
        self.assertEqual(started["tick"], 0)

        stepped = client.call("play.step", {"ticks": 600, "render": "last"})
        self.assertEqual(stepped["tick"], 600)
        self.assertEqual(stepped["ticks"], 600)
        # Only the last tick extracted the game view.
        self.assertEqual(stepped["rendered"], 1)
        state = client.call("play.state")
        self.assertEqual(state["tick"], 600)
        self.assertEqual(state["stateHash"], stepped["stateHash"])
        # The run state uses the vocabulary of _meta.playState.
        self.assertEqual(state["state"], client.call("session.info")["playState"])
        # Lockstep: nothing advances without play.step, however many frames pass between calls.
        for _ in range(5):
            self.assertEqual(client.call("play.state")["tick"], 600)

    @unittest.skip("contract stub: un-skipped by M7 stream A")
    def test_play_step_respects_frame_budget_and_reports_state_hash(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.create_ball(client)
        self.start_lockstep(client)
        # render "every" runs one tick per frame.
        every = client.call("play.step", {"ticks": 10, "render": "every"})
        self.assertEqual(every["frames"], 10)
        self.assertEqual(every["rendered"], 10)
        # render "none" runs as many ticks per frame as fit the budget: more than one frame for a step that cannot fit
        # one frame, and far fewer frames than ticks.
        budgeted = client.call("play.step", {"ticks": BUDGET_TICKS, "render": "none"})
        self.assertEqual(budgeted["tick"], 10 + BUDGET_TICKS)
        self.assertEqual(budgeted["ticks"], BUDGET_TICKS)
        self.assertEqual(budgeted["rendered"], 0)
        self.assertGreaterEqual(budgeted["frames"], 2)
        self.assertLess(budgeted["frames"], BUDGET_TICKS)
        self.assertRegex(budgeted["stateHash"], r"^[0-9a-f]{16}$")

        # Other clients wait for the owner: lockstep belongs to one client, play.stop included.
        other = self.connect(self.editors[-1], "other-client")
        for method, params in (("play.step", {"ticks": 1}), ("play.stop", {}), ("play.pause", {})):
            with self.subTest(method=method):
                with self.assertRaises(engine_client.EngineError) as raised:
                    other.call(method, params)
                self.assert_engine_error(raised.exception, engine_client.INVALID_STATE, "InvalidState")

        # A session of the same scene and seed stepped in one call ends at the same hash, however the ticks were split
        # across frames and whatever was rendered.
        client.call("play.stop")
        self.start_lockstep(client)
        replayed = client.call("play.step", {"ticks": 10 + BUDGET_TICKS, "render": "none"})
        self.assertEqual(replayed["tick"], budgeted["tick"])
        self.assertEqual(replayed["stateHash"], budgeted["stateHash"])

    @unittest.skip("contract stub: un-skipped by M7 stream A")
    def test_disconnect_releases_lockstep_and_pauses_play(self) -> None:
        editor = self.start_editor()
        owner = self.connect(editor, "owner")
        self.create_project(owner)
        owner.call("scene.new", {"path": "Assets/Scenes/Main.scene"})
        self.create_ball(owner)
        self.start_lockstep(owner)
        observer = self.connect(editor, "observer")
        self.assertEqual(observer.call("play.state")["lockstepOwner"], "owner")

        owner.close()
        deadline = time.monotonic() + 30.0
        state = observer.call("play.state")
        while state["lockstep"] and time.monotonic() < deadline:
            state = observer.call("play.state")
        self.assertFalse(state["lockstep"])
        self.assertEqual(state["state"], "Paused")
        self.assertEqual(state["mode"], "Play")
        # The session survives and the other client can step it now.
        self.assertEqual(observer.call("play.step", {"ticks": 2})["tick"], state["tick"] + 2)

    @unittest.skip("contract stub: un-skipped by M7 stream A")
    def test_headless_play_without_lockstep_is_throttled(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.create_ball(client)
        # The editor's loop runs at the project's FixedHz while playing (FrameLoop::SetLoopConfig).
        client.call("project.setSettings", {"patch": {"Simulation": {"FixedHz": THROTTLED_FIXED_HZ}}})
        client.call("play.start")
        start = time.monotonic()
        first = client.call("play.state")["tick"]
        time.sleep(1.0)
        last = client.call("play.state")["tick"]
        elapsed = time.monotonic() - start
        # Throttled to FixedHz frames per wall-clock second, one tick per frame (ManualClock): never much faster than
        # real time at the project's rate. An unthrottled loop would run thousands of ticks per second, and a loop at
        # the editor's 60 Hz six times the project's rate.
        self.assertGreater(last, first)
        self.assertLessEqual(last - first, int(2 * THROTTLED_FIXED_HZ * elapsed) + THROTTLED_FIXED_HZ)

    @unittest.skip("contract stub: un-skipped by M7 stream A")
    def test_play_start_and_stop_leave_the_edit_scene_unchanged(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.create_ball(client)
        client.call("scene.save")
        before = client.call("scene.get", {"target": "edit"})
        client.call("play.start", {"lockstep": True})
        client.call("entity.update", {"entity": "/Ball", "target": "play",
                                      "components": {"Transform": {"Translation": [5, 5, 5]}}})
        client.call("play.step", {"ticks": 30})
        client.call("play.stop")
        self.assertEqual(client.call("scene.get", {"target": "edit"}), before)
        self.assertEqual(client.call("play.state")["state"], "Edit")

    @unittest.skip("contract stub: un-skipped by M7 stream A")
    def test_play_methods_refuse_members_of_later_milestones(self) -> None:
        client, _ = self.open_editor_with_scene()
        for params, pointer in (({"parameters": {}}, "/parameters"), ({"pauseOnError": False}, "/pauseOnError")):
            with self.subTest(params=params):
                with self.assertRaises(engine_client.EngineError) as raised:
                    client.call("play.start", params)
                self.assert_engine_error(raised.exception, engine_client.UNSUPPORTED, "Unsupported")
                self.assertEqual(raised.exception.issues[0]["pointer"], pointer)
        client.call("play.start", {"lockstep": True})
        client.call("play.pause")
        self.assertEqual(client.call("play.state")["state"], "Paused")
        client.call("play.setTimeScale", {"scale": 0.5})
        self.assertEqual(client.call("play.state")["timeScale"], 0.5)
        client.call("play.resume")
        self.assertEqual(client.call("play.state")["state"], "Play")
        client.call("play.stop")


if __name__ == "__main__":
    unittest.main()
