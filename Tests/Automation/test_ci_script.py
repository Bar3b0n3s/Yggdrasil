"""Scripts/CI.py's M15 stages and modes (Docs/Architecture.md §15.8, §15.9; Docs/Decisions/0024-m15-contract.md
decisions 9, 10, 11, 18 and 19): --milestone selects every available stage plus hardening and implies
--require-clang-cl, refusing every selection and mode option; a plain run lists hardening as not run; the export and
determinism stages require a device unless --gpu-optional; the Dist build is followed by the map check under its
Roadmap name; the portability stage checks the Runtime's link settings; the hardening stage reports Hardening.py's steps
one by one. Nothing here builds or runs a stage: the runner's script calls are recorded.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from harness import REPOSITORY_ROOT

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import CI  # noqa: E402  (Scripts/ is on sys.path above; CI imports its Lib package from there)
from Lib import scripts  # noqa: E402
from Lib.report import Console, Status, Step  # noqa: E402


class RecordingRunner(CI.Runner):
    """A runner whose child scripts are recorded and pass, instead of running."""

    def __init__(self, arguments: argparse.Namespace) -> None:
        super().__init__(arguments, Console(json_mode=True))
        self.calls: list[tuple[str, str, list[str]]] = []

    def script(self, name: str, script: str, script_arguments: list[str], timeout: float,
               expected_exit: int = 0, expected_output: object = None) -> Step:
        self.calls.append((name, script, list(script_arguments)))
        return self.done(Step(name, Status.PASSED, "recorded"))


def parse(*argv: str) -> argparse.Namespace:
    return CI.parse_arguments(list(argv))


class MilestoneTests(unittest.TestCase):
    def test_milestone_selects_every_available_stage_and_hardening_with_clang_cl_required(self) -> None:
        arguments = parse("--milestone", "--json")
        expected = [stage.name for stage in CI.STAGES if stage.available]
        self.assertEqual(arguments.selected, expected)
        self.assertIn("hardening", arguments.selected)
        self.assertEqual(arguments.explicit_stages, {"hardening"})
        self.assertTrue(arguments.require_clang_cl)
        self.assertEqual(arguments.configs, ["Debug", "Release", "Dist"])

    def test_milestone_refuses_every_selection_and_mode_option(self) -> None:
        refused = [["--stages", "unit"], ["--skip-stages", "lint"], ["--configs", "Release"], ["--toolset", "msc"],
                   ["--no-clang-cl"], ["--gpu-optional"], ["--contract"]]
        for options in refused:
            with self.subTest(options=options), contextlib.redirect_stderr(io.StringIO()) as error:
                with self.assertRaises(SystemExit) as raised:
                    parse("--milestone", *options)
                self.assertEqual(raised.exception.code, 2)
                self.assertIn(options[0], error.getvalue())

    def test_a_plain_run_lists_hardening_as_not_run(self) -> None:
        arguments = parse()
        self.assertIn("hardening", arguments.selected)
        self.assertFalse(arguments.require_clang_cl)
        runner = RecordingRunner(arguments)
        steps = CI.run_stage(runner, CI.STAGE_BY_NAME["hardening"], arguments.configs)
        self.assertEqual([(step.status, step.detail) for step in steps], [(Status.NOT_RUN, CI.MILESTONE_ONLY_NOTE)])
        self.assertEqual(runner.calls, [])

    def test_naming_hardening_runs_it(self) -> None:
        arguments = parse("--stages", "hardening")
        self.assertEqual(arguments.selected, ["hardening"])
        runner = RecordingRunner(arguments)
        with mock.patch.object(CI.Runner, "hardening", return_value=[Step("soak Release", Status.PASSED)]) as hardening:
            steps = CI.run_stage(runner, CI.STAGE_BY_NAME["hardening"], arguments.configs)
        hardening.assert_called_once_with()
        self.assertEqual([step.name for step in steps], ["soak Release"])


class StageTests(unittest.TestCase):
    def test_export_and_determinism_require_a_device_unless_gpu_optional(self) -> None:
        for options, device in (((), ["--require-gpu"]), (("--gpu-optional",), [])):
            with self.subTest(options=options):
                runner = RecordingRunner(parse("--stages", "export,determinism", *options))
                CI.run_stage(runner, CI.STAGE_BY_NAME["export"], runner.arguments.configs)
                CI.run_stage(runner, CI.STAGE_BY_NAME["determinism"], runner.arguments.configs)
                self.assertEqual(runner.calls, [
                    ("export Release, Dist", "Test.py",
                     ["--suite", "export", "--config", "Release,Dist", "--junit", *device]),
                    ("determinism Debug, Release, Dist", "Test.py",
                     ["--suite", "determinism", "--config", "Debug,Release,Dist", "--junit", *device]),
                ])

    def test_contract_mode_allows_skips_in_the_export_stage(self) -> None:
        runner = RecordingRunner(parse("--stages", "export", "--contract", "--configs", "Release"))
        CI.run_stage(runner, CI.STAGE_BY_NAME["export"], runner.arguments.configs)
        self.assertEqual(runner.calls, [("export Release", "Test.py",
                                         ["--suite", "export", "--config", "Release", "--junit", "--require-gpu",
                                          "--allow-skips"])])

    def test_the_dist_build_is_followed_by_the_map_check_under_its_roadmap_name(self) -> None:
        runner = RecordingRunner(parse("--stages", "build", "--configs", "Dist"))
        CI.run_stage(runner, CI.STAGE_BY_NAME["build"], runner.arguments.configs)
        self.assertEqual(runner.calls, [
            ("build Dist", "Build.py", ["--config", "Dist"]),
            ("Dist: map file contains no automation, Analysis, ImGui or AssetPipeline symbols", "CheckRuntimeMap.py",
             ["--config", "Dist"]),
        ])

    def test_the_link_settings_step_reports_every_problem(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            generated = {target: (Path(directory) / f"{target[0]}-{target[1]}", [])
                         for target in CI.PORTABILITY_TARGETS}
            step = CI.Runner.check_link_settings(generated)
        self.assertEqual(step.status, Status.FAILED)
        for system, action in CI.PORTABILITY_TARGETS:
            self.assertIn(f"{system}-{action}: cannot read", step.detail)

    def test_the_hardening_stage_reports_each_hardening_step(self) -> None:
        document = {"success": False, "steps": [
            {"name": "perf capture (Release)", "status": "warning", "detail": "playCopy over budget",
             "durationSeconds": 180.0},
            {"name": "soak Release", "status": "failed", "detail": "1 of 35 subject(s) failed",
             "durationSeconds": 900.0, "exitCode": 1},
            {"name": "asan generate", "status": "not-run", "detail": "soak Release failed", "durationSeconds": 0.0},
        ]}
        process = mock.Mock(returncode=1)
        # A workflow annotation may follow the JSON document on standard output.
        output = "progress\n" + json.dumps(document, indent=2) + "\n::error title=soak Release::1 of 35 failed\n"
        process.communicate.return_value = (output.encode("utf-8"), None)
        runner = RecordingRunner(parse("--stages", "hardening"))
        with mock.patch.object(CI.subprocess, "Popen", return_value=process) as popen:
            steps = runner.hardening()
        command = popen.call_args.args[0]
        self.assertEqual(Path(command[1]).name, "Hardening.py")
        self.assertEqual(command[2:], ["--steps", ",".join(CI.HARDENING_STEPS), "--json"])
        self.assertEqual([(step.name, step.status) for step in steps], [
            ("perf capture (Release)", Status.WARNING), ("soak Release", Status.FAILED),
            ("asan generate", Status.NOT_RUN)])
        self.assertEqual(steps[1].exit_code, 1)


class StaticCheckTests(unittest.TestCase):
    def test_the_static_checks_include_the_detmath_reference_check(self) -> None:
        # Architecture §1.3 principle 2 and §15.1 T0: the committed DetMath oracle (Tests/Source/Engine/Core/
        # DetMathReferenceData.h) is staleness-checked by both gates, after the fixture generators.
        calls: list[tuple[str, str, list[str]]] = []

        def record(name: str, script: str, arguments: list[str], *rest: object, **options: object) -> Step:
            calls.append((name, script, list(arguments)))
            return Step(name, Status.PASSED, "recorded")

        with mock.patch.object(scripts, "run_script", record):
            steps = scripts.run_static_checks(Console(json_mode=True), lambda step: step)
        self.assertEqual(len(steps), len(calls))
        names = [name for name, _, _ in calls]
        self.assertIn(("detmath reference", "GenerateDetMathReference.py", ["--check"]), calls)
        fixtures = [index for index, name in enumerate(names) if name.startswith("fixtures ")]
        self.assertTrue(fixtures)
        self.assertEqual(names.index("detmath reference"), fixtures[-1] + 1)
        self.assertEqual(names[names.index("detmath reference") + 1], "resources")


if __name__ == "__main__":
    unittest.main()
