"""The machine-wide locks of Scripts/Lib/locks.py (Docs/Architecture.md §2.3; Docs/Decisions/0028-backlog-program.md
decision 16, T-LOCKS): the counted build lock, the exclusive GPU and gate locks, inheritance by child processes, release
when a holder dies, the holder in every waiting and refusal message, and the scripts that take them (Build.py, Test.py,
CI.py, Scaffold.py, Perf.py, Hardening.py, Export.py, Locks.py).

Every test runs on a lock directory of its own (lock_holder.isolated_lock_directory), with other processes holding
locks (lock_holder.LockHolder), so it never touches the machine's real locks. Waits that must end are bounded by a
generous deadline that only bounds a failure; waits that must run out use a short bound whose outcome does not depend on
time (the lock stays held throughout).
"""

from __future__ import annotations

import argparse
import contextlib
import functools
import io
import json
import os
import subprocess
import sys
import unittest
from collections.abc import Callable
from pathlib import Path
from unittest import mock

from harness import automation_config
from lock_holder import DEADLINE_SECONDS, REPOSITORY_ROOT, LockHolder, isolated_lock_directory

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import Build  # noqa: E402  (Scripts/ is on sys.path above)
import CI  # noqa: E402
import Export  # noqa: E402
import Hardening  # noqa: E402
import Test  # noqa: E402
from Lib import locks, workspace  # noqa: E402
from Lib.report import EXIT_INIT_FAILED, EXIT_SUCCESS, EXIT_TIMEOUT, Console, Status  # noqa: E402

GATE, GPU, BUILD = locks.GATE, locks.GPU, locks.BUILD
CAPPED, UNCAPPED, AUTO = locks.CAPPED, locks.UNCAPPED, locks.AUTO
# A wait that must run out: the lock stays held throughout, so the outcome does not depend on how long it is.
SHORT_WAIT_SECONDS = 0.5


def quiet(_message: str) -> None:
    """A reporter that drops the waiting messages."""


class LockTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.lock_directory = isolated_lock_directory(self)

    def holder(self, *requests: str, timeout: float = 0.0, environment: dict[str, str] | None = None,
               marker: str = "") -> LockHolder:
        holder = LockHolder(*requests, timeout=timeout, environment=environment, marker=marker)
        self.addCleanup(holder.stop)
        return holder

    def hold(self, hold: locks.Hold) -> locks.Hold:
        """Releases `hold` at the end of the test, whatever happens."""
        self.addCleanup(hold.release)
        return hold

    @staticmethod
    def stranger_environment() -> dict[str, str]:
        """This process's environment without its holds: a process that does not run under them."""
        return {key: value for key, value in os.environ.items() if key != locks.HELD_LOCKS_VARIABLE}

    def refused(self, call: Callable[[], object], lock: str, pids: set[int]) -> locks.LockUnavailable:
        """Runs `call`, which asks for a lock without waiting, and checks that it is refused because `lock` is held by
        exactly the processes `pids`."""
        with self.assertRaises(locks.LockUnavailable) as raised:
            hold = call()
            if isinstance(hold, locks.Hold):
                hold.release()
        error = raised.exception
        self.assertEqual(error.lock, lock)
        self.assertEqual({holder.pid for holder in error.holders}, pids)
        self.assertFalse(error.timed_out)
        self.assertEqual(error.exit_code, EXIT_INIT_FAILED)
        return error


class BuildLockTests(LockTestCase):
    def test_two_capped_builds_hold_the_lock_together_and_a_third_build_waits(self) -> None:
        first = self.holder("build:capped").ready()
        second = self.holder("build:capped").ready()
        self.assertIn("held build capped owned", first.output)
        self.assertIn("held build capped owned", second.output)
        for mode in (CAPPED, UNCAPPED, AUTO):
            with self.subTest(mode=mode):
                self.refused(functools.partial(locks.acquire_build, mode, 0, quiet), BUILD, {first.pid, second.pid})
        self.assertEqual(first.release(), EXIT_SUCCESS)
        hold = self.hold(locks.acquire_build(CAPPED, DEADLINE_SECONDS, quiet))
        self.assertEqual((hold.mode, hold.inherited), (CAPPED, False))

    def test_an_uncapped_build_holds_the_whole_lock(self) -> None:
        uncapped = self.holder("build:uncapped").ready()
        self.assertIn("held build uncapped owned", uncapped.output)
        for mode in (CAPPED, UNCAPPED, AUTO):
            with self.subTest(mode=mode):
                error = self.refused(functools.partial(locks.acquire_build, mode, 0, quiet), BUILD, {uncapped.pid})
                self.assertEqual(error.holders[0].mode, UNCAPPED)
        uncapped.release()
        hold = self.hold(locks.acquire_build(AUTO, DEADLINE_SECONDS, quiet))
        self.assertEqual(hold.mode, UNCAPPED, "an automatic build on a free lock is uncapped")

    def test_an_automatic_build_takes_the_free_slot_beside_one_capped_build(self) -> None:
        capped = self.holder("build:capped").ready()
        hold = self.hold(locks.acquire_build(AUTO, 0, quiet))
        self.assertEqual(hold.mode, CAPPED)
        third = self.holder("build:auto")
        self.assertEqual(third.wait_for("unavailable", "ready"), f"unavailable build {EXIT_INIT_FAILED}")
        hold.release()
        capped.release()

    def test_a_waiting_uncapped_build_holds_off_new_capped_builds(self) -> None:
        capped = self.holder("build:capped").ready()
        waiting = self.holder("build:uncapped", timeout=DEADLINE_SECONDS)
        waiting.wait_for("waiting")
        # One slot is free, but the uncapped build waiting for the running ones goes first.
        error = self.refused(lambda: locks.acquire_build(CAPPED, 0, quiet), BUILD, {capped.pid, waiting.pid})
        self.assertIn(locks.CLAIM, {holder.mode for holder in error.holders})
        capped.release()
        self.assertEqual(waiting.wait_for("held", "unavailable"), "held build uncapped owned")
        waiting.ready()
        self.refused(lambda: locks.acquire_build(CAPPED, 0, quiet), BUILD, {waiting.pid})

    def test_build_py_chooses_its_mode_and_job_count_from_jobs(self) -> None:
        self.assertEqual(Build.requested_build_mode(None, 24), AUTO)
        self.assertEqual(Build.requested_build_mode(12, 24), CAPPED)
        self.assertEqual(Build.requested_build_mode(13, 24), UNCAPPED)
        self.assertEqual(Build.requested_build_mode(1, 1), CAPPED)
        self.assertEqual(Build.build_jobs(UNCAPPED, None, 24), 24)
        self.assertEqual(Build.build_jobs(UNCAPPED, 16, 24), 16)
        self.assertEqual(Build.build_jobs(CAPPED, None, 24), 12)
        self.assertEqual(Build.build_jobs(CAPPED, 8, 24), 8)
        # Under an inherited capped hold, a build that asked for more stays within the cap.
        self.assertEqual(Build.build_jobs(CAPPED, 24, 24), 12)

    def test_a_capped_msbuild_build_bounds_its_compilers_across_the_whole_build(self) -> None:
        # The review's per-project finding: -m:N with CL_MPCount=N let N projects each run N compilers (Engine and
        # EditorCore side by side at /MP12: 24 cl.exe for a 12-job build). MSBuild runs up to -m x CL_MPCount of them,
        # so a capped build keeps that product within its jobs; an uncapped one has the machine.
        def arguments(jobs: int, capped: bool) -> list[str]:
            return Build.msbuild_parallel_arguments(argparse.Namespace(jobs=jobs, capped=capped))

        self.assertEqual(arguments(12, True), ["-m:2", "-p:CL_MPCount=6"])
        self.assertEqual(arguments(24, False), ["-m:24"])
        for jobs in range(1, 33):
            with self.subTest(jobs=jobs):
                projects, compilers = arguments(jobs, True)
                self.assertTrue(projects.startswith("-m:") and compilers.startswith("-p:CL_MPCount="))
                product = int(projects.split(":", 1)[1]) * int(compilers.split("=", 1)[1])
                self.assertLessEqual(product, jobs)
                self.assertGreaterEqual(product, jobs - 1, "a capped build uses its jobs")


class GpuLockTests(LockTestCase):
    def test_the_gpu_lock_admits_one_holder_across_processes(self) -> None:
        first = self.holder(GPU).ready()
        self.refused(lambda: locks.acquire_gpu(0, quiet), GPU, {first.pid})
        second = self.holder(GPU)
        self.assertEqual(second.wait_for("unavailable", "ready"), f"unavailable gpu {EXIT_INIT_FAILED}")
        self.assertEqual(second.process.wait(DEADLINE_SECONDS), EXIT_INIT_FAILED)
        first.release()
        hold = self.hold(locks.acquire_gpu(DEADLINE_SECONDS, quiet))
        self.assertEqual((hold.mode, hold.inherited), (locks.EXCLUSIVE, False))
        self.assertEqual([holder.pid for holder in locks.holders(GPU)], [os.getpid()])
        hold.release()
        self.assertEqual(locks.holders(GPU), [])

    def test_a_process_that_holds_the_gpu_lock_gets_a_nested_hold(self) -> None:
        outer = self.hold(locks.acquire_gpu(0, quiet))
        inner = locks.acquire_gpu(0, quiet)
        inner.release()
        self.assertEqual([holder.pid for holder in locks.holders(GPU)], [os.getpid()],
                         "a nested hold's release keeps the lock")
        outer.release()
        self.assertEqual(locks.holders(GPU), [])


class GateLockTests(LockTestCase):
    def test_the_gate_is_exclusive_and_builds_gpu_runs_and_tests_wait_for_it(self) -> None:
        gate = self.holder(GATE).ready()
        self.refused(lambda: locks.acquire_gate(0, quiet), GATE, {gate.pid})
        self.refused(lambda: locks.wait_for_gate(0, quiet), GATE, {gate.pid})
        # The GPU and build locks are free, but nothing new starts while a gate runs.
        self.refused(lambda: locks.acquire_gpu(0, quiet), GATE, {gate.pid})
        self.refused(lambda: locks.acquire_build(CAPPED, 0, quiet), GATE, {gate.pid})
        waiter = self.holder(GPU, "build:auto", timeout=DEADLINE_SECONDS)
        self.assertIn(f"pid {gate.pid}", waiter.wait_for("waiting"))
        gate.release()
        waiter.ready()
        self.assertIn("held gpu exclusive owned", waiter.output)
        self.assertIn("held build uncapped owned", waiter.output)

    def test_a_gate_waits_for_the_running_gpu_run_and_build(self) -> None:
        running = self.holder(GPU, "build:capped").ready()
        gate = self.hold(locks.acquire_gate(0, quiet))
        self.refused(lambda: locks.acquire_gpu(0, quiet), GPU, {running.pid})
        self.refused(lambda: locks.acquire_build(UNCAPPED, 0, quiet), BUILD, {running.pid})
        running.release()
        self.hold(locks.acquire_gpu(DEADLINE_SECONDS, quiet))
        self.assertEqual(self.hold(locks.acquire_build(UNCAPPED, DEADLINE_SECONDS, quiet)).mode, UNCAPPED)
        gate.release()


class InheritanceTests(LockTestCase):
    def test_a_child_runs_under_its_parents_gate_and_gpu_locks(self) -> None:
        self.hold(locks.acquire_gate(0, quiet))
        self.hold(locks.acquire_gpu(0, quiet))
        self.assertEqual(set(locks.inherited_tokens()), {GATE, GPU}, "the holds are exported to child processes")
        child = self.holder(GATE, GPU, "wait-gate", environment=dict(os.environ)).ready()
        self.assertEqual(child.output, ["held gate exclusive inherited", "held gpu exclusive inherited",
                                        "held wait-gate none owned", "ready"])
        stranger = self.holder(GPU, environment=self.stranger_environment())
        self.assertEqual(stranger.wait_for("unavailable", "held"), f"unavailable gate {EXIT_INIT_FAILED}")

    def test_a_child_takes_no_build_slot_of_its_own(self) -> None:
        self.assertEqual(self.hold(locks.acquire_build(CAPPED, 0, quiet)).mode, CAPPED)
        child = self.holder("build:auto", environment=dict(os.environ)).ready()
        self.assertEqual(child.output, ["held build capped inherited", "ready"])
        # Not double counted: the second slot is still free for one other build, and only for one.
        second = self.holder("build:capped", environment=self.stranger_environment()).ready()
        third = self.holder("build:capped", environment=self.stranger_environment())
        self.assertEqual(third.wait_for("unavailable", "held"), f"unavailable build {EXIT_INIT_FAILED}")
        self.assertEqual({holder.pid for holder in locks.holders(BUILD)}, {os.getpid(), second.pid})

    def test_a_token_that_names_no_held_lock_is_not_inherited(self) -> None:
        holder = self.holder(GPU).ready()
        environment = dict(os.environ)
        environment[locks.HELD_LOCKS_VARIABLE] = f"{GPU}={'0' * 32}"
        child = self.holder(GPU, environment=environment)
        self.assertEqual(child.wait_for("unavailable", "held"), f"unavailable gpu {EXIT_INIT_FAILED}")
        holder.release()
        # The token of a hold that has ended is stale too, once another process holds the lock.
        ended = locks.acquire_gpu(0, quiet)
        environment = dict(os.environ)
        ended.release()
        self.holder(GPU).ready()
        child = self.holder(GPU, environment=environment)
        self.assertEqual(child.wait_for("unavailable", "held"), f"unavailable gpu {EXIT_INIT_FAILED}")

    def test_the_children_of_a_run_that_began_before_a_gate_finish_it(self) -> None:
        # Hardening.py holds the GPU lock and runs Build.py and Test.py. A gate that begins meanwhile waits for that
        # run, so its children must not wait for the gate, or each would wait for the other.
        self.hold(locks.acquire_gpu(0, quiet))
        gate = self.holder(GATE).ready()
        child = self.holder("wait-gate", "build:capped", environment=dict(os.environ)).ready()
        self.assertEqual(child.output, ["held wait-gate none owned", "held build capped owned", "ready"])
        stranger = self.holder("build:capped", environment=self.stranger_environment())
        self.assertEqual(stranger.wait_for("unavailable", "held"), f"unavailable gate {EXIT_INIT_FAILED}")
        child.release()
        gate.release()


class ReleaseTests(LockTestCase):
    def test_a_killed_holder_releases_its_locks(self) -> None:
        holder = self.holder(GATE, GPU, "build:uncapped").ready()
        self.refused(lambda: locks.acquire_gate(0, quiet), GATE, {holder.pid})
        holder.kill()
        self.hold(locks.acquire_gate(DEADLINE_SECONDS, quiet))
        self.hold(locks.acquire_gpu(DEADLINE_SECONDS, quiet))
        self.assertEqual(self.hold(locks.acquire_build(UNCAPPED, DEADLINE_SECONDS, quiet)).mode, UNCAPPED)


class ReportTests(LockTestCase):
    def test_the_waiting_message_and_the_refusal_name_the_holder(self) -> None:
        holder = self.holder(GPU, marker="holder-marker").ready()
        messages: list[str] = []
        with self.assertRaises(locks.LockUnavailable) as raised:
            locks.acquire_gpu(SHORT_WAIT_SECONDS, messages.append)
        self.assertTrue(raised.exception.timed_out)
        self.assertEqual(raised.exception.exit_code, EXIT_TIMEOUT)
        self.assertEqual(len(messages), 1, messages)
        for text in (messages[0], str(raised.exception)):
            with self.subTest(text=text):
                self.assertIn(f"pid {holder.pid}", text)
                self.assertIn(REPOSITORY_ROOT.as_posix(), text)
                self.assertIn("--holder-marker", text)
        refusal = self.refused(lambda: locks.acquire_gpu(0, messages.append), GPU, {holder.pid})
        self.assertIn(f"pid {holder.pid}", str(refusal))
        self.assertEqual(len(messages), 1, "a refusal without a wait reports nothing besides its error")
        step = locks.unavailable_step(refusal)
        self.assertEqual((step.status, step.exit_code), (Status.FAILED, EXIT_INIT_FAILED))
        self.assertEqual(step.data["lock"]["holders"][0]["pid"], holder.pid)

    def test_a_lock_held_before_its_record_is_written_names_no_holder(self) -> None:
        # A holder that has just taken a lock, or a waiter's momentary attempt, holds it without a record: it is not
        # listed (another waiter's probe would otherwise show up as a holder), and a refusal still says it is held.
        probe = locks.LockFile(locks.lock_paths(GPU)[0])
        self.assertTrue(probe.try_lock())
        self.addCleanup(probe.unlock)
        self.assertTrue(locks.is_held(probe.path))
        self.assertEqual(locks.holders(GPU), [])
        refusal = self.refused(lambda: locks.acquire_gpu(0, quiet), GPU, set())
        self.assertIn("the gpu lock is held", str(refusal))

    def test_locks_py_lists_every_holder(self) -> None:
        gpu = self.holder(GPU).ready()
        build = self.holder("build:capped").ready()
        completed = subprocess.run([sys.executable, str(REPOSITORY_ROOT / "Scripts" / "Locks.py"), "--json"],
                                   capture_output=True, text=True, encoding="utf-8", timeout=DEADLINE_SECONDS,
                                   check=False)
        self.assertEqual(completed.returncode, EXIT_SUCCESS, completed.stderr)
        document = json.loads(completed.stdout)
        self.assertEqual(Path(document["directory"]), self.lock_directory)
        self.assertEqual(document["locks"][GATE], [])
        self.assertEqual([holder["pid"] for holder in document["locks"][GPU]], [gpu.pid])
        self.assertEqual([(holder["pid"], holder["mode"]) for holder in document["locks"][BUILD]],
                         [(build.pid, CAPPED)])


class ProbeTests(LockTestCase):
    """is_held and every waiter's poll take a free lock for a moment, so another process can find it held: one such
    sighting never refuses a request or costs a waiting uncapped build its claim (the review's lock-probe finding)."""

    def test_a_free_lock_another_process_probes_at_that_moment_is_still_granted(self) -> None:
        real = locks._lock_descriptor
        calls: list[int] = []

        def first_attempt_collides(descriptor: int) -> bool:
            # The first lock attempt of the request meets another process's probe; every later one finds the files
            # as they are.
            calls.append(descriptor)
            return False if len(calls) == 1 else real(descriptor)

        for name, request in (("gpu", lambda: locks.acquire_gpu(0, quiet)),
                              ("gate", lambda: locks.acquire_gate(0, quiet)),
                              ("build", lambda: locks.acquire_build(CAPPED, 0, quiet)),
                              ("wait for the gate", lambda: locks.wait_for_gate(0, quiet))):
            with self.subTest(name), mock.patch.object(locks, "_lock_descriptor", first_attempt_collides):
                calls.clear()
                hold = request()
                if isinstance(hold, locks.Hold):
                    hold.release()
                self.assertGreater(len(calls), 1)

    def test_a_waiting_uncapped_build_keeps_its_claim_when_the_gate_only_looked_held(self) -> None:
        capped = self.holder("build:capped").ready()
        attempt = locks._BuildAttempt(UNCAPPED, None)
        self.addCleanup(attempt.abandon)
        self.assertIsInstance(attempt.poll(), locks._Blocked)
        self.assertIsNotNone(attempt.claim, "a blocked uncapped request takes the claim")
        sightings = iter([False])  # one probe of the free gate by another process, then the gate as it is

        def gate_open() -> bool:
            return next(sightings, True)

        with mock.patch.object(locks, "_gate_open", gate_open):
            self.assertEqual(attempt.poll().lock, GATE)
        self.assertIsNotNone(attempt.claim, "the claim stays")
        self.refused(lambda: locks.acquire_build(CAPPED, 0, quiet), BUILD, {capped.pid, os.getpid()})
        # A gate that runs takes the claim back: the gate's own build request needs it.
        gate = self.holder(GATE).ready()
        self.assertEqual(attempt.poll().lock, GATE)
        self.assertIsNone(attempt.claim)
        gate.release()
        capped.release()


class ScriptTests(LockTestCase):
    """Each script that takes a lock refuses at once with --no-wait while another process holds it, naming the holder,
    before it does anything else."""

    def run_script(self, script: str, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, str(REPOSITORY_ROOT / "Scripts" / script), *arguments, "--no-wait"],
                              capture_output=True, text=True, encoding="utf-8", timeout=DEADLINE_SECONDS,
                              cwd=REPOSITORY_ROOT, check=False)

    def assert_refused_by(self, completed: subprocess.CompletedProcess[str], holder: LockHolder) -> None:
        self.assertEqual(completed.returncode, EXIT_INIT_FAILED, completed.stdout + completed.stderr)
        self.assertIn(f"pid {holder.pid}", completed.stdout + completed.stderr)

    def export_arguments(self, *extra: str) -> tuple[str, ...]:
        """Export.py's arguments for the suite's editor (it checks that the editor is built before it takes a lock)."""
        config = automation_config()
        return ("--project", str(self.lock_directory / "Game"), "--config", config, "--out",
                str(self.lock_directory / "Package"), "--editor-config", config, *extra)

    def test_gpu_runs_refuse_while_another_holds_the_gpu_lock(self) -> None:
        holder = self.holder(GPU).ready()
        for script, arguments in (("Test.py", ("--suite", "gpu", "--config", "Debug")),
                                  ("Test.py", ("--suite", "golden", "--update-golden")),
                                  ("Scaffold.py", ("--check",)),
                                  ("Perf.py", ()),
                                  ("Hardening.py", ("--steps", "soak")),
                                  ("Export.py", self.export_arguments()),
                                  ("Export.py", self.export_arguments("--renderer", "none", "--smoke-test"))):
            with self.subTest(script=script, arguments=arguments):
                self.assert_refused_by(self.run_script(script, *arguments), holder)

    def test_build_and_test_runs_refuse_while_a_gate_runs(self) -> None:
        gate = self.holder(GATE).ready()
        workspace = self.make_workspace()
        for script, arguments in (("Build.py", ("--workspace-dir", str(workspace))),
                                  ("Test.py", ("--suite", "unit", "--config", "Debug")),
                                  ("CI.py", ("--stages", "lint")),
                                  ("Export.py", self.export_arguments("--renderer", "none"))):
            with self.subTest(script=script):
                self.assert_refused_by(self.run_script(script, *arguments), gate)

    def test_only_the_gpu_suites_and_steps_take_the_gpu_lock(self) -> None:
        for suite, expected in (("unit", False), ("gpu", True), ("golden", True), ("feature", True),
                                ("automation", True), ("export", True), ("determinism", True), ("games", True)):
            with self.subTest(suite=suite):
                self.assertEqual(Test.uses_gpu(argparse.Namespace(suites=[suite])), expected)
        for step, expected in (("perf", True), ("soak", True), ("asan-generate", False), ("asan-build", False),
                               ("asan-unit", False), ("asan-feature", True), ("debug-heap", True), ("fuzz", False)):
            with self.subTest(step=step):
                self.assertEqual(Hardening.uses_gpu([step]), expected)
        self.assertEqual(set(Hardening.STEPS), {"perf", "soak", "asan-generate", "asan-build", "asan-unit",
                                                "asan-feature", "debug-heap", "fuzz"}, "a new step needs a row above")

    def test_runs_without_the_gpu_go_ahead_while_another_holds_the_gpu_lock(self) -> None:
        # A unit run and the ASan build never wait behind a GPU run in another checkout; the GPU runs do.
        holder = self.holder(GPU).ready()
        console = Console(True)

        def options(**values: object) -> argparse.Namespace:
            return argparse.Namespace(no_wait=True, lock_timeout=0.0, **values)

        self.assertIsNone(Test.take_machine_locks(options(suites=["unit"]), console))
        self.assertIsNone(Hardening.take_machine_locks(options(steps=["asan-generate", "asan-build", "asan-unit",
                                                                      "fuzz"]), console))
        self.refused(lambda: Test.take_machine_locks(options(suites=["unit", "gpu"]), console), GPU, {holder.pid})
        self.refused(lambda: Hardening.take_machine_locks(options(steps=["asan-build", "asan-feature"]), console),
                     GPU, {holder.pid})

    def test_an_export_runs_the_gpu_when_it_renders_or_smoke_tests(self) -> None:
        for renderer, smoke_test, expected in (("vulkan", False, True), ("none", True, True), ("none", False, False)):
            with self.subTest(renderer=renderer, smoke_test=smoke_test):
                arguments = argparse.Namespace(renderer=renderer, smoke_test=smoke_test)
                self.assertEqual(Export.uses_gpu(arguments), expected)

    def test_build_py_refuses_while_an_uncapped_build_runs(self) -> None:
        uncapped = self.holder("build:uncapped").ready()
        completed = self.run_script("Build.py", "--workspace-dir", str(self.make_workspace()), "--jobs", "1", "--json")
        self.assert_refused_by(completed, uncapped)
        document = json.loads(completed.stdout)
        self.assertEqual(document["steps"][0]["lock"]["lock"], BUILD)
        self.assertIsNone(document["buildLock"])

    def make_workspace(self) -> Path:
        """A generated vs2026 workspace with one project: enough for Build.py to read it before it takes the lock."""
        root = self.lock_directory / "Workspace"
        (root / "Runtime").mkdir(parents=True)
        (root / workspace.workspace_file_name("vs2026")).write_text(
            '<Solution>\n  <Configurations>\n    <BuildType Name="Debug" />\n    <Platform Name="x64" />\n'
            '  </Configurations>\n  <Project Path="Runtime\\Runtime.vcxproj" />\n</Solution>\n', encoding="utf-8")
        (root / "Runtime" / "Runtime.vcxproj").write_text(
            '<Project DefaultTargets="Build">\n  <PropertyGroup>\n    <ConfigurationType>Application'
            '</ConfigurationType>\n  </PropertyGroup>\n</Project>\n', encoding="utf-8")
        return root


class CiLockTests(LockTestCase):
    def test_ci_holds_every_lock_for_its_run_and_its_children_run_under_them(self) -> None:
        children: list[list[str]] = []

        def run_stages(arguments: argparse.Namespace, console: object, host: object) -> int:
            self.assertEqual({holder.pid for lock in (GATE, GPU, BUILD) for holder in locks.holders(lock)},
                             {os.getpid()})
            self.assertEqual(locks.holders(BUILD)[0].mode, UNCAPPED)
            child = LockHolder(GATE, GPU, "build:auto", "wait-gate", environment=dict(os.environ))
            try:
                child.ready()
                children.append(child.output)
            finally:
                child.release()
                child.stop()
            return EXIT_SUCCESS

        with mock.patch.object(CI, "run_stages", run_stages), contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(CI.main(["--stages", "lint", "--no-wait"]), EXIT_SUCCESS)
        self.assertEqual(children, [["held gate exclusive inherited", "held gpu exclusive inherited",
                                     "held build uncapped inherited", "held wait-gate none owned", "ready"]])
        for lock in (GATE, GPU, BUILD):
            self.assertEqual(locks.holders(lock), [], f"CI.py released the {lock} lock")
        self.assertNotIn(locks.HELD_LOCKS_VARIABLE, os.environ)

    def test_ci_releases_the_gate_when_a_later_lock_stays_held(self) -> None:
        running = self.holder(GPU).ready()
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(CI.main(["--stages", "lint", "--no-wait", "--json"]), EXIT_INIT_FAILED)
        document = json.loads(output.getvalue())
        self.assertEqual((document["lock"]["lock"], document["stages"]), (GPU, []))
        self.assertEqual(document["lock"]["holders"][0]["pid"], running.pid)
        self.assertEqual(locks.holders(GATE), [], "the gate it took first is released")
        self.assertNotIn(locks.HELD_LOCKS_VARIABLE, os.environ)


if __name__ == "__main__":
    unittest.main()
