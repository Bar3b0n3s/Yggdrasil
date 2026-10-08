"""Play sessions through automation (Docs/Architecture.md §5.6, §13.5 "play", §13.6, §4.2; Roadmap M7 acceptance;
Docs/Decisions/0012-m7-decisions.md decisions 3 and 8): lockstep, the 50 ms per-frame budget of play.step, the state
hash, the disconnect rule and the headless throttle.

test_headless_play_without_lockstep_is_throttled measures ticks against wall-clock time, a recorded wall-clock exception
(ADR 0012 decision 3): it asserts only an upper bound with a generous margin, so a slow machine passes the same way.
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
# How long the calls that step BUDGET_TICKS wait: play.step's own timeout (its TimeoutSeconds), since a Debug editor may
# need longer than the client's default for them.
PLAY_STEP_TIMEOUT_SECONDS = 600.0
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
        budgeted = client.call("play.step", {"ticks": BUDGET_TICKS, "render": "none"},
                               timeout=PLAY_STEP_TIMEOUT_SECONDS)
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
        replayed = client.call("play.step", {"ticks": 10 + BUDGET_TICKS, "render": "none"},
                               timeout=PLAY_STEP_TIMEOUT_SECONDS)
        self.assertEqual(replayed["tick"], budgeted["tick"])
        self.assertEqual(replayed["stateHash"], budgeted["stateHash"])

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

    def test_play_start_and_stop_leave_the_edit_scene_unchanged(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.create_ball(client)
        client.call("scene.save")
        before = client.call("scene.get", {"target": "edit"})
        client.call("play.start", {"lockstep": True})
        # A play-scene edit is transient (section 13.4): no undo step, and the edit scene's revision stays.
        updated = client.call("entity.update", {"entity": "/Ball", "target": "play",
                                                "components": {"Transform": {"Translation": [5, 5, 5]}}})
        self.assertEqual(updated["undoIndex"], 0)
        self.assertEqual(updated["_meta"]["revision"], before["_meta"]["revision"])
        self.assertFalse(updated["_meta"]["dirty"])
        played = client.call("entity.get", {"entity": "/Ball", "target": "play"})
        self.assertEqual(played["entity"]["components"]["Transform"]["Translation"], [5, 5, 5])
        client.call("play.step", {"ticks": 30})
        client.call("play.stop")
        after = client.call("scene.get", {"target": "edit"})
        self.assertEqual(after["scene"], before["scene"])
        self.assertEqual(after["_meta"]["revision"], before["_meta"]["revision"])
        self.assertEqual(client.call("play.state")["state"], "Edit")

    def test_play_methods_refuse_members_of_later_milestones(self) -> None:
        client, _ = self.open_editor_with_scene()
        for params, pointer in (({"parameters": {}}, "/parameters"), ({"pauseOnError": False}, "/pauseOnError")):
            with self.subTest(params=params):
                with self.assertRaises(engine_client.EngineError) as raised:
                    client.call("play.start", params)
                self.assert_engine_error(raised.exception, engine_client.UNSUPPORTED, "Unsupported")
                self.assertEqual(raised.exception.issues[0]["pointer"], pointer)
        self.assertEqual(client.call("play.state")["state"], "Edit")

    def test_pause_set_time_scale_and_resume_report_the_state(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("play.start", {"lockstep": True})
        # The owner's pause leaves lockstep; the paused session then resumes like any other.
        paused = client.call("play.pause")
        self.assertEqual(paused["state"], "Paused")
        self.assertFalse(paused["lockstep"])
        self.assertEqual(client.call("play.state")["state"], "Paused")
        self.assertEqual(client.call("play.setTimeScale", {"scale": 0.5})["timeScale"], 0.5)
        self.assertEqual(client.call("play.state")["timeScale"], 0.5)
        self.assertEqual(client.call("play.resume")["state"], "Play")
        self.assertEqual(client.call("play.state")["state"], "Play")
        client.call("play.stop")

    def test_dry_runs_and_failed_batches_leave_the_play_session_unchanged(self) -> None:
        # Section 13.4: a dry run leaves no trace, and a failed edit.batch takes every op back. A play-scene creation
        # draws a runtime id from the session's seeded generator, whose draw count is part of the state hash, so taking
        # the creation back must wind the generator back too.
        client, _ = self.open_editor_with_scene()
        self.create_ball(client)
        self.start_lockstep(client)
        before = client.call("play.state")

        dry = client.call("entity.create", {"name": "Spawned", "target": "play", "dryRun": True})
        self.assertTrue(dry["dryRun"])
        duplicated = client.call("entity.duplicate", {"entities": ["/Ball"], "target": "play", "dryRun": True})
        self.assertTrue(duplicated["dryRun"])
        after_dry_runs = client.call("play.state")
        self.assertEqual(after_dry_runs["stateHash"], before["stateHash"])
        self.assertEqual(after_dry_runs["entityCount"], before["entityCount"])

        with self.assertRaises(engine_client.EngineError) as raised:
            client.call("edit.batch", {"label": "Broken", "ops": [
                {"method": "entity.create", "params": {"name": "Spawned", "target": "play"}},
                {"method": "entity.create", "params": {"name": "Broken", "target": "play",
                                                       "components": {"RigidBody": {"Mass": -1}}}},
            ]})
        self.assertEqual(raised.exception.data["failedOp"], 1)
        after_batch = client.call("play.state")
        self.assertEqual(after_batch["stateHash"], before["stateHash"])
        self.assertEqual(after_batch["entityCount"], before["entityCount"])

        # The next real creation gets the id a fresh session of the same seed gives its first creation.
        created = client.call("entity.create", {"name": "Spawned", "target": "play"})
        self.assertEqual(created["undoIndex"], 0)
        client.call("play.stop")
        self.start_lockstep(client)
        fresh = client.call("entity.create", {"name": "Spawned", "target": "play"})
        self.assertEqual(created["entity"]["id"], fresh["entity"]["id"])
        client.call("play.stop")


if __name__ == "__main__":
    unittest.main()
