"""The progress of a tool call (Docs/Architecture.md §13.8 "Progress"; Docs/Decisions/0036-m25-tetris-pixel-art.md,
friction F14), through the official SDK's client on its in-memory transport: a request with a progress token gets
notifications/progress while the call waits for its turn behind another call and while it runs, naming the holder of a
machine-wide lock while editor_launch waits for it, with the seconds waited counted when each is sent, and then
answering with LockUnavailable; a request without one gets none; a notification that cannot be sent never fails the
call; the launch's default wait ends before a client that hears nothing gives up; and a launch whose call the client
cancels stops waiting at once, spawns nothing and leaves the bridge free for the next call (ADR 0036's milestone
review). No editor is spawned: the waits are for a lock of a test's own lock directory (lock_holder), and the long calls
and the launches that proceed are stand-ins."""

from __future__ import annotations

import contextlib
import os
import re
import shutil
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from typing import Any, Callable, Iterator
from unittest import mock

import anyio
import mcp_types as types
from mcp import Client
from mcp.server.session import ServerSession

MCP_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = MCP_ROOT.parents[1]
sys.path.insert(0, str(MCP_ROOT))
sys.path.insert(0, str(REPOSITORY_ROOT / "Tools" / "Automation"))
sys.path.insert(0, str(REPOSITORY_ROOT / "Tests" / "Automation"))

import engine_client  # noqa: E402
import lock_holder  # noqa: E402
from engine_mcp import launcher, server, transcript  # noqa: E402
from engine_mcp.launcher import Launcher  # noqa: E402
from Lib import locks  # noqa: E402  (engine_mcp puts Scripts/ on sys.path)

# How long Claude Code waits for a tool call that sends it no response and no progress before it aborts the call (its
# default MCP tool idle timeout), which ended S3's launch in friction F14.
CLIENT_IDLE_TIMEOUT_SECONDS = 1800.0
# The heartbeat's interval in these tests, and its bound on one notification where a test needs it lowered.
HEARTBEAT_SECONDS = 0.05
NOTIFY_BOUND_SECONDS = 0.2
# A launch's wait that must run out: the lock stays held throughout, so the outcome does not depend on its length.
SHORT_WAIT_SECONDS = 0.5
# Bound a session that never ends and a wait for a notification that never comes; no passing outcome depends on them.
SESSION_TIMEOUT_SECONDS = 120.0
WAIT_SECONDS = lock_holder.DEADLINE_SECONDS
PROGRESS = "notifications/progress"
# The protocol eras the SDK's client speaks: a handshake-era connection and a 2026-07-28 one.
MODES = ("legacy", "auto")
# The heartbeat's own interval, which the tests lower.
DEFAULT_PROGRESS_INTERVAL_SECONDS = server.PROGRESS_INTERVAL_SECONDS
# A launch's wait that a cancellation must end: twice the bound of the next call's wait for its turn, so a cancelled
# wait that went on (holding the bridge) fails that call's bound; only a failure lasts this long.
CANCELLED_WAIT_SECONDS = 2 * WAIT_SECONDS
# The seconds a lock wait's report counts ("Waiting for the gpu lock (N s of at most M s): ...", launcher.LockWait).
WAITED = re.compile(r"Waiting for the gpu lock \((\d+) s of at most \d+ s\)")


class ProgressRecorder:
    """The progress callback of one call: records each (progress, message) and sets `heard` once a message contains
    `expected`."""

    def __init__(self, expected: str) -> None:
        self.expected = expected
        self.notifications: list[tuple[float, str]] = []
        self.heard = threading.Event()

    async def record(self, progress: float, total: float | None, message: str | None) -> None:
        self.notifications.append((progress, message or ""))
        if self.expected in (message or ""):
            self.heard.set()


class WaitRecorder(ProgressRecorder):
    """A ProgressRecorder for a launch's lock wait: it also sets `grown` once two messages that name the holder count
    different seconds waited, the later more."""

    def __init__(self, expected: str) -> None:
        super().__init__(expected)
        self.grown = threading.Event()
        self.waited: list[int] = []

    async def record(self, progress: float, total: float | None, message: str | None) -> None:
        await super().record(progress, total, message)
        found = WAITED.search(message or "")
        if found is not None and self.expected in (message or ""):
            self.waited.append(int(found.group(1)))
            if self.waited[-1] > self.waited[0]:
                self.grown.set()


class HeldActivity(launcher.Activity):
    """The launcher's activity, whose every report of a wait returns only once `until` is set (bounded), so the client
    hears the wait as long as a test needs, however long a try of the lock takes."""

    def __init__(self, until: threading.Event) -> None:
        super().__init__()
        self.until = until

    def set(self, wait: launcher.LockWait | None) -> None:
        super().set(wait)
        if wait:
            self.until.wait(WAIT_SECONDS)


class ProgressTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="EngineProgress-")).resolve()
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        # No transcript redirection, and no method coverage: no editor answers these calls.
        excluded = (transcript.TRANSCRIPT_ENVIRONMENT_VARIABLE, engine_client.COVERAGE_ENVIRONMENT_VARIABLE)
        environment = {key: value for key, value in os.environ.items() if key not in excluded}
        patch = mock.patch.dict(os.environ, environment, clear=True)
        patch.start()
        self.addCleanup(patch.stop)
        # Launches wait for the machine-wide locks of a directory of their own, never the machine's.
        lock_holder.isolated_lock_directory(self)
        self.project = self.directory / "Game"
        self.project.mkdir()
        (self.project / "Game.eproj").write_text('{"Format": "Project", "Version": 1}\n', encoding="utf-8")
        self.launcher = Launcher(REPOSITORY_ROOT, self.directory / "UserData", lock_wait_seconds=SHORT_WAIT_SECONDS)
        self.bridge = server.Bridge(REPOSITORY_ROOT, [], self.launcher)
        heartbeat = mock.patch.object(server, "PROGRESS_INTERVAL_SECONDS", HEARTBEAT_SECONDS)
        heartbeat.start()
        self.addCleanup(heartbeat.stop)

    def call(self, mode: str, name: str, arguments: dict[str, Any], recorder: ProgressRecorder | None = None,
             message_handler: Callable[[Any], Any] | None = None) -> types.CallToolResult:
        """Calls tool `name` from an SDK client of protocol era `mode`, with a progress token when `recorder` is given
        (its callback); `message_handler` receives the client's other messages, the progress notifications of a
        handshake-era connection among them. Each call has a server of its own on the test's bridge."""
        mcp_server, _ = server._build_server(self.bridge)

        async def session() -> types.CallToolResult:
            with anyio.fail_after(SESSION_TIMEOUT_SECONDS):
                async with Client(mcp_server, mode=mode, message_handler=message_handler) as client:
                    return await client.call_tool(name, arguments,
                                                  progress_callback=recorder.record if recorder is not None else None)

        return anyio.run(session)

    def stand_in_status(self, proceed: threading.Event) -> Callable[[dict[str, Any]], tuple[dict[str, Any], str, bool]]:
        """editor_status's stand-in for a long call: it returns once `proceed` is set, failing the call when it never
        is."""

        def status(arguments: dict[str, Any]) -> tuple[dict[str, Any], str, bool]:
            if not proceed.wait(WAIT_SECONDS):
                raise AssertionError("the condition the stand-in call waited for never came")
            return {"connected": False, "editors": []}, "status read", False

        return status

    @staticmethod
    def text_of(result: types.CallToolResult) -> str:
        return "\n".join(block.text for block in result.content if isinstance(block, types.TextContent))

    def test_the_default_lock_wait_ends_before_a_silent_client_gives_up(self) -> None:
        # A client that asks for no progress still hears LockUnavailable naming the holder (F14's second part).
        self.assertLess(launcher.LOCK_WAIT_SECONDS, CLIENT_IDLE_TIMEOUT_SECONDS)
        self.assertEqual(Launcher(REPOSITORY_ROOT).lock_wait_seconds, launcher.LOCK_WAIT_SECONDS)
        # A client that asks for progress hears from a waiting call many times within its idle timeout.
        self.assertLessEqual(DEFAULT_PROGRESS_INTERVAL_SECONDS * 10, CLIENT_IDLE_TIMEOUT_SECONDS)

    def test_a_launch_waiting_for_the_gpu_lock_reports_its_holder_then_answers_lock_unavailable(self) -> None:
        with lock_holder.LockHolder(locks.GPU, marker="progress-holder") as holder:
            holder.ready()
            for mode in MODES:
                with self.subTest(mode=mode):
                    # The wait's reports return once the client has heard the seconds waited grow (bounded), so the
                    # progress comes before the wait runs out whatever the timing.
                    recorder = WaitRecorder(f"pid {holder.pid}")
                    self.launcher.activity = HeldActivity(recorder.grown)
                    with mock.patch.object(engine_client, "find_editor_executable", return_value=Path("Editor")), \
                            mock.patch.object(engine_client, "launch_editor") as launch_editor:
                        result = self.call(mode, "editor_launch", {"project": str(self.project), "renderer": "vulkan"},
                                           recorder)
                    launch_editor.assert_not_called()
                    self.assertTrue(recorder.heard.is_set(), recorder.notifications)
                    heard = [message for _, message in recorder.notifications if f"pid {holder.pid}" in message]
                    # The tool, then the wait's report, which names the lock, its holder (pid, worktree, command), the
                    # seconds waited and the bound.
                    self.assertTrue(heard[0].startswith("editor_launch"), heard[0])
                    self.assertRegex(heard[0], WAITED)
                    self.assertIn(locks.GPU, heard[0])
                    self.assertIn("--progress-holder", heard[0], "the holder's command")
                    # The seconds waited are counted when each message is sent, not when the wait last reported (the
                    # milestone review): a later message counts more.
                    self.assertTrue(recorder.grown.is_set(), recorder.waited)
                    self.assertEqual(recorder.waited, sorted(recorder.waited), "the seconds waited never go back")
                    values = [progress for progress, _ in recorder.notifications]
                    self.assertEqual(values, sorted(set(values)), "the progress increases")

                    self.assertTrue(result.is_error)
                    structured = result.structured_content
                    self.assertEqual(structured["error"], "LockUnavailable")
                    self.assertEqual(structured["lock"], locks.GPU)
                    self.assertEqual([entry["pid"] for entry in structured["holders"]], [holder.pid])
                    self.assertTrue(structured["timedOut"])
                    self.assertIn('renderer "none"', structured["hint"])
                    text = self.text_of(result)
                    self.assertIn(f"pid {holder.pid}", text)
                    self.assertIn(f"hint: {structured['hint']}", text)
                    self.assertEqual(self.launcher.activity.read(), "", "the wait's report ends with the wait")

    def test_a_lock_wait_counts_its_seconds_when_it_is_read(self) -> None:
        # The milestone review: the report counted the seconds of the lock library's latest report, made at most every
        # five minutes, so a heartbeat could say "0 s" after minutes of waiting.
        activity = launcher.Activity()
        self.assertEqual(activity.read(), "")
        activity.set(launcher.LockWait("gpu lock", locks.GPU, "pid 7 in D:/Other since 2026-11-13T00:00:00Z: gpu run",
                                       started=100.0, bound=1500.0))
        self.assertEqual(activity.read(130.0), "Waiting for the gpu lock (30 s of at most 1500 s): the gpu lock is "
                                               "held by pid 7 in D:/Other since 2026-11-13T00:00:00Z: gpu run")
        self.assertIn("(400 s of at most 1500 s)", activity.read(500.0))
        self.assertEqual(server.progress_message("editor_launch", 400.0, activity.read(500.0)),
                         f"editor_launch running for 400 s: {activity.read(500.0)}")
        activity.set(None)
        self.assertEqual(activity.read(500.0), "")

    @contextlib.contextmanager
    def stand_in_spawn(self, launch_editor: mock.Mock) -> Iterator[None]:
        """A launch that gets past the machine-wide locks proceeds without an editor: `launch_editor` stands in for the
        spawn, and the connection, the project.open and the session.info for the editor's answers."""
        connection = mock.Mock(supervised=True, session_info=None)
        connection.describe.return_value = {"connected": True, "pid": 4242, "project": str(self.project)}
        with mock.patch.object(engine_client, "find_editor_executable", return_value=Path("Editor")), \
                mock.patch.object(engine_client, "launch_editor", launch_editor), \
                mock.patch.object(self.launcher, "connect", return_value=connection), \
                mock.patch.object(self.launcher, "open_project"), \
                mock.patch.object(self.launcher, "read_session_info"), \
                mock.patch.object(self.launcher, "release_when_exits"):
            yield

    def test_a_cancelled_launch_stops_waiting_for_the_lock_and_spawns_nothing(self) -> None:
        # The milestone review: the call runs on a worker thread, which ignored the cancellation, so a launch cancelled
        # while it waited for the GPU lock kept waiting, holding the bridge (the hint's renderer "none" launch queued
        # behind it), then spawned a rendering editor nobody asked for once the lock came free.
        self.launcher.lock_wait_seconds = CANCELLED_WAIT_SECONDS
        with lock_holder.LockHolder(locks.GPU, marker="cancel-holder") as holder:
            holder.ready()
            for mode in MODES:
                with self.subTest(mode=mode):
                    self.bridge.connection = None
                    recorder = ProgressRecorder(f"pid {holder.pid}")
                    mcp_server, _ = server._build_server(self.bridge)
                    results: dict[str, types.CallToolResult] = {}
                    cancelled: list[bool] = []

                    async def session() -> None:
                        launch_scope = anyio.CancelScope()
                        with anyio.fail_after(SESSION_TIMEOUT_SECONDS):
                            async with Client(mcp_server, mode=mode) as client:
                                async def launch_rendering() -> None:
                                    with launch_scope:
                                        results["vulkan"] = await client.call_tool(
                                            "editor_launch", {"project": str(self.project), "renderer": "vulkan"},
                                            progress_callback=recorder.record)

                                async with anyio.create_task_group() as group:
                                    group.start_soon(launch_rendering)
                                    # Cancelled while it waits, once its progress names the holder.
                                    await anyio.to_thread.run_sync(recorder.heard.wait, WAIT_SECONDS)
                                    launch_scope.cancel()
                                    cancelled.append(launch_scope.cancel_called)
                                # The hint's launch runs at once instead of waiting for the cancelled one's turn to end.
                                with anyio.move_on_after(WAIT_SECONDS):
                                    results["none"] = await client.call_tool(
                                        "editor_launch", {"project": str(self.project), "renderer": "none"})

                    launch_editor = mock.Mock(return_value=mock.Mock())
                    with self.stand_in_spawn(launch_editor):
                        anyio.run(session)
                    self.assertTrue(recorder.heard.is_set(), recorder.notifications)
                    self.assertEqual(cancelled, [True])
                    if "vulkan" in results:
                        # A protocol era may still deliver the cancelled call's answer: never a launched editor.
                        self.assertTrue(results["vulkan"].is_error)
                        self.assertEqual(results["vulkan"].structured_content["error"], "Cancelled")
                    self.assertIn("none", results, "the next call waited for the cancelled launch's wait")
                    self.assertFalse(results["none"].is_error, results["none"].content)
                    self.assertTrue(self.text_of(results["none"]).startswith("Launched editor (pid 4242)"))
                    # Only the renderer "none" launch spawned, and the cancelled one took no lock.
                    self.assertEqual(launch_editor.call_count, 1)
                    self.assertIn("none", launch_editor.call_args.args[1])
                    self.assertEqual([entry.pid for entry in locks.holders(locks.GPU)], [holder.pid])
                    self.assertEqual(self.launcher.activity.read(), "")

    def test_a_launch_cancelled_as_it_takes_the_gpu_lock_releases_it_and_spawns_nothing(self) -> None:
        # The lock comes free just as the call is cancelled: the launch takes it, then lets it go again.
        cancelled = threading.Event()
        real_acquire_gpu = locks.acquire_gpu

        def acquire_then_cancel(timeout: float, report: locks.Reporter, command: str | None = None) -> locks.Hold:
            hold = real_acquire_gpu(timeout, report, command)
            cancelled.set()
            return hold

        launch_editor = mock.Mock(return_value=mock.Mock())
        with self.stand_in_spawn(launch_editor), mock.patch.object(locks, "acquire_gpu", acquire_then_cancel), \
                self.assertRaises(launcher.LaunchCancelled):
            self.launcher.launch(launcher.LaunchRequest(project=self.project, renderer="vulkan"), cancelled)
        launch_editor.assert_not_called()
        self.assertEqual(locks.holders(locks.GPU), [], "the lock it took is released")
        with lock_holder.LockHolder(locks.GPU) as other:
            other.ready()

        # A launch cancelled before it tries the lock takes nothing.
        with self.stand_in_spawn(launch_editor), self.assertRaises(launcher.LaunchCancelled):
            self.launcher.launch(launcher.LaunchRequest(project=self.project, renderer="none"), cancelled)
        launch_editor.assert_not_called()

    def test_a_long_call_reports_how_long_it_has_run(self) -> None:
        for mode in MODES:
            with self.subTest(mode=mode):
                recorder = ProgressRecorder("editor_status")
                with mock.patch.object(self.bridge, "editor_status", self.stand_in_status(recorder.heard)):
                    result = self.call(mode, "editor_status", {}, recorder)
                self.assertFalse(result.is_error, result.content)
                self.assertEqual(self.text_of(result), "status read")
                self.assertTrue(recorder.notifications)
                # The tool and how long it has run, with nothing it waits for.
                for _, message in recorder.notifications:
                    self.assertTrue(message.startswith("editor_status"), message)
                    self.assertRegex(message, r"\b\d+ s$")
                    self.assertNotIn("lock", message)
                values = [progress for progress, _ in recorder.notifications]
                self.assertEqual(values, sorted(set(values)), "the progress increases")

    def test_a_call_queued_behind_a_running_call_reports_that_it_waits(self) -> None:
        # Calls run one at a time: a call that arrives while another runs (a long export or test run) reports, while it
        # waits for its turn, that it is queued behind that call, so its client does not give up on it either.
        for mode in MODES:
            with self.subTest(mode=mode):
                entered = threading.Event()
                recorder = ProgressRecorder("editor_status")
                status = self.stand_in_status(recorder.heard)

                def first_status(arguments: dict[str, Any]) -> tuple[dict[str, Any], str, bool]:
                    # Runs until the queued call has reported that it waits behind it (bounded).
                    entered.set()
                    return status(arguments)

                mcp_server, _ = server._build_server(self.bridge)
                results: dict[str, types.CallToolResult] = {}

                async def session() -> None:
                    with anyio.fail_after(SESSION_TIMEOUT_SECONDS):
                        async with Client(mcp_server, mode=mode) as client:
                            async def call_first() -> None:
                                results["first"] = await client.call_tool("editor_status", {})

                            async with anyio.create_task_group() as group:
                                group.start_soon(call_first)
                                await anyio.to_thread.run_sync(entered.wait, WAIT_SECONDS)
                                results["queued"] = await client.call_tool("engine_methods", {},
                                                                           progress_callback=recorder.record)

                with mock.patch.object(self.bridge, "editor_status", first_status):
                    anyio.run(session)
                self.assertTrue(entered.is_set())
                self.assertFalse(results["first"].is_error, results["first"].content)
                self.assertEqual(self.text_of(results["first"]), "status read")
                self.assertFalse(results["queued"].is_error, results["queued"].content)
                self.assertTrue(recorder.heard.is_set(), recorder.notifications)
                # The queued tool, that it is queued and for how long, and the tool whose call runs meanwhile.
                queued = [message for _, message in recorder.notifications if "editor_status" in message]
                self.assertTrue(queued[0].startswith("engine_methods"), queued[0])
                self.assertIn("queued", queued[0])
                self.assertRegex(queued[0], r"\b\d+ s\b")
                values = [progress for progress, _ in recorder.notifications]
                self.assertEqual(values, sorted(set(values)), "the progress increases")

    def test_a_call_without_a_progress_token_gets_no_progress_and_still_answers(self) -> None:
        # The heartbeat runs (it asks the SDK to report twice), but a request without a token is sent nothing. A
        # handshake-era client's message handler sees every notification, as the call with a token shows.
        methods: list[str] = []

        async def record(message: Any) -> None:
            if not isinstance(message, Exception) and isinstance(getattr(message, "method", None), str):
                methods.append(message.method)

        reports: list[str] = []
        reported_twice = threading.Event()
        real_report_progress = ServerSession.report_progress

        async def counted_report(session: ServerSession, progress: float, total: float | None = None,
                                 message: str | None = None) -> None:
            reports.append(message or "")
            if len(reports) >= 2:
                reported_twice.set()
            await real_report_progress(session, progress, total, message)

        with mock.patch.object(ServerSession, "report_progress", counted_report), \
                mock.patch.object(self.bridge, "editor_status", self.stand_in_status(reported_twice)):
            result = self.call("legacy", "editor_status", {}, message_handler=record)
        self.assertFalse(result.is_error, result.content)
        self.assertGreaterEqual(len(reports), 2)
        self.assertNotIn(PROGRESS, methods)

        recorder = ProgressRecorder("editor_status")
        with mock.patch.object(self.bridge, "editor_status", self.stand_in_status(recorder.heard)):
            result = self.call("legacy", "editor_status", {}, recorder, message_handler=record)
        self.assertFalse(result.is_error, result.content)
        self.assertIn(PROGRESS, methods)

    def test_a_progress_notification_that_cannot_be_sent_never_fails_the_call(self) -> None:
        tried = threading.Event()

        async def broken_report(session: ServerSession, progress: float, total: float | None = None,
                                message: str | None = None) -> None:
            tried.set()
            raise RuntimeError("the transport broke")

        recorder = ProgressRecorder("editor_status")
        with mock.patch.object(ServerSession, "report_progress", broken_report), \
                mock.patch.object(self.bridge, "editor_status", self.stand_in_status(tried)), \
                self.assertLogs("engine_mcp", level="WARNING") as logged:
            result = self.call("legacy", "editor_status", {}, recorder)
        self.assertFalse(result.is_error, result.content)
        self.assertEqual(self.text_of(result), "status read")
        self.assertIn("the transport broke", "\n".join(logged.output))
        self.assertEqual(recorder.notifications, [])

    def test_a_progress_notification_that_hangs_is_given_up_and_the_next_one_is_sent(self) -> None:
        real_report_progress = ServerSession.report_progress
        hung = threading.Event()

        async def first_hangs(session: ServerSession, progress: float, total: float | None = None,
                              message: str | None = None) -> None:
            if not hung.is_set():
                hung.set()
                await anyio.sleep_forever()
            await real_report_progress(session, progress, total, message)

        recorder = ProgressRecorder("editor_status")
        with mock.patch.object(server, "NOTIFY_TIMEOUT_SECONDS", NOTIFY_BOUND_SECONDS), \
                mock.patch.object(ServerSession, "report_progress", first_hangs), \
                mock.patch.object(self.bridge, "editor_status", self.stand_in_status(recorder.heard)):
            result = self.call("legacy", "editor_status", {}, recorder)
        self.assertFalse(result.is_error, result.content)
        self.assertTrue(hung.is_set())
        self.assertGreaterEqual(recorder.notifications[0][0], 2, "the first notification was given up")


if __name__ == "__main__":
    unittest.main()
