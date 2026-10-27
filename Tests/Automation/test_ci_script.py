"""Scripts/CI.py's M15 and M16 stages and modes (Docs/Architecture.md §15.8, §15.9; Docs/Decisions/0024-m15-contract.md
decisions 9, 10, 11, 18 and 19; Docs/Decisions/0026-m16-contract.md decision 8): --milestone selects every available
stage plus hardening and implies --require-clang-cl, refusing every selection and mode option; a plain run lists
hardening as not run; the export, determinism and games stages require a device unless --gpu-optional, and every stage
of §15.8 exists; the Dist build is followed by the map check under its Roadmap name; the portability stage checks the
Runtime's link settings; the hardening stage reports Hardening.py's steps one by one. Nothing here builds or runs a
stage: the runner's script calls are recorded. Every script works from any directory (Docs/Architecture.md §2.3): a
relative path on its command line names a path in the caller's directory, the gates, Test.py and every other entry
script then run in the repository root whatever the caller's directory, and premake's output is resolved against the
directory premake ran in.
"""

from __future__ import annotations

import argparse
import ast
import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ElementTree
from collections.abc import Iterator
from pathlib import Path
from unittest import mock

from harness import REPOSITORY_ROOT

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import CI  # noqa: E402  (Scripts/ is on sys.path above; CI imports its Lib package from there)
import PreCommit  # noqa: E402
import Test as test_script  # noqa: E402
from Lib import paths, premake, scripts, unittest_runner, workspace  # noqa: E402
from Lib.process import ProcessResult  # noqa: E402
from Lib.report import Console, Status, Step  # noqa: E402


# The calls that change an entry script's working directory: paths.enter_repository_root, and os.chdir in
# Scripts/Lib/unittest_runner.py, which cannot import the library modules.
ENTER_CALLS = frozenset({"enter_repository_root", "chdir"})
# A test module the runner runs from another directory: it passes only in the repository root RUNNER_PROBE_ROOT names.
RUNNER_PROBE_ROOT = "ENGINE_TEST_RUNNER_PROBE_ROOT"
RUNNER_PROBE = f"""import os
import unittest
from pathlib import Path


class WorkingDirectory(unittest.TestCase):
    def test_runs_in_the_repository_root(self) -> None:
        self.assertEqual(Path.cwd().resolve(), Path(os.environ["{RUNNER_PROBE_ROOT}"]).resolve())
"""


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


def called_names(node: ast.AST) -> list[str]:
    """The names of the functions and methods `node` calls, in walk order (`f(...)` and `x.f(...)` both give "f")."""
    return [call.func.attr if isinstance(call.func, ast.Attribute) else call.func.id for call in ast.walk(node)
            if isinstance(call, ast.Call) and isinstance(call.func, (ast.Attribute, ast.Name))]


@contextlib.contextmanager
def working_directory(directory: Path) -> Iterator[None]:
    """Runs the block with `directory` as this process's working directory, restoring the previous one after it."""
    previous = Path.cwd()
    os.chdir(directory)
    try:
        yield
    finally:
        os.chdir(previous)


@contextlib.contextmanager
def portability_root() -> Iterator[Path]:
    """A temporary bin-int/<name>/ as CI.PORTABILITY_ROOT: below the repository root, so premake prints the files it
    writes there relative to that root, as it does for the real bin-int/Portability/."""
    paths.BIN_INT_ROOT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="PortabilityWorkingDirectory-", dir=paths.BIN_INT_ROOT) as root, \
            mock.patch.object(CI, "PORTABILITY_ROOT", Path(root)):
        yield Path(root)


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

    def test_the_games_stage_runs_one_test_py_over_the_selected_configurations(self) -> None:
        # One run over the configurations --configs selects (the Dist half needs Dist, the editor halves Debug and
        # Release), with a device unless --gpu-optional; games is no longer reported as not available.
        self.assertEqual([stage.name for stage in CI.STAGES if not stage.available], [])
        for options, configs, device in (((), "Debug,Release,Dist", ["--require-gpu"]),
                                         (("--gpu-optional", "--configs", "Debug,Release"), "Debug,Release", [])):
            with self.subTest(options=options):
                runner = RecordingRunner(parse("--stages", "games", *options))
                steps = CI.run_stage(runner, CI.STAGE_BY_NAME["games"], runner.arguments.configs)
                self.assertEqual([step.status for step in steps], [Status.PASSED])
                self.assertEqual(runner.calls, [(f"games {configs.replace(',', ', ')}", "Test.py",
                                                 ["--suite", "games", "--config", configs, "--junit", *device])])

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


class WorkingDirectoryTests(unittest.TestCase):
    """M18's gate, started from the main checkout as python <worktree>/Scripts/CI.py, resolved premake's
    root-relative output against the main checkout, so the portability stage named the main checkout's files and
    crashed: a script works in its own checkout's root whatever the directory it was started from, and reads a
    relative path given on its command line in that directory, as it did before it entered the root."""

    def test_premake_output_resolves_below_the_directory_premake_ran_in(self) -> None:
        with tempfile.TemporaryDirectory() as directory, working_directory(Path(directory)):
            elsewhere = Path(directory).resolve()
            output = ("Building configurations...\nRunning action 'vs2026'...\n"
                      "Generated bin-int/Portability/windows-vs2026/Engine.vcxproj...\n"
                      f"Generated {elsewhere.as_posix()}/Makefile...\nDone (10ms).\n")
            reported = premake.generated_paths(output)
            self.assertEqual(reported, [
                (paths.REPOSITORY_ROOT / "bin-int" / "Portability" / "windows-vs2026" / "Engine.vcxproj").resolve(),
                (elsewhere / "Makefile").resolve()])

    def test_the_portability_stage_maps_premake_output_from_another_working_directory(self) -> None:
        with portability_root(), tempfile.TemporaryDirectory() as elsewhere, working_directory(Path(elsewhere)), \
                contextlib.redirect_stderr(io.StringIO()):
            runner = RecordingRunner(parse("--stages", "portability"))
            step, location, files = runner.generate_portability("windows", "vs2026")
            self.assertEqual(step.status, Status.PASSED, step.detail)
            self.assertIn(workspace.workspace_file_name("vs2026"), files)
            self.assertIn("Engine.vcxproj", files)
            self.assertEqual([file for file in files if not (location / file).is_file()], [])

    def test_a_file_premake_reports_outside_the_location_fails_the_step(self) -> None:
        with portability_root() as root, contextlib.redirect_stderr(io.StringIO()):
            location = paths.display_path(root / "windows-vs2026")
            output = f"Generated {location}/Engine.vcxproj...\nGenerated Engine/Engine.vcxproj...\n"
            runner = RecordingRunner(parse("--stages", "portability"))
            with mock.patch.object(CI.premake, "run", return_value=ProcessResult(["premake5"], 0, False, 0.0, output)):
                step, _, files = runner.generate_portability("windows", "vs2026")
        self.assertEqual(step.status, Status.FAILED)
        self.assertIn("Engine/Engine.vcxproj", step.detail)
        self.assertEqual(files, ["Engine.vcxproj"])

    def test_every_entry_script_enters_the_repository_root_right_after_parsing_its_arguments(self) -> None:
        # Parsed first, while the caller's directory is still the working directory, so paths.argument_path makes a
        # relative command-line path absolute against it; then the root, before anything else the script does.
        entry_scripts = {script: "enter_repository_root" for script in paths.SCRIPTS_ROOT.glob("*.py")}
        entry_scripts[paths.SCRIPTS_ROOT / "Lib" / "unittest_runner.py"] = "chdir"
        self.assertIn(paths.SCRIPTS_ROOT / "CI.py", entry_scripts)
        for script, enter in sorted(entry_scripts.items()):
            with self.subTest(script=script.name):
                tree = ast.parse(script.read_text(encoding="utf-8"))
                main = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "main"]
                self.assertEqual(len(main), 1)
                body = main[0].body
                parsing = [index for index, statement in enumerate(body)
                           if set(called_names(statement)) & {"parse_args", "parse_arguments"}]
                entering = [index for index, statement in enumerate(body)
                            for name in called_names(statement) if name in ENTER_CALLS]
                self.assertTrue(parsing)
                self.assertEqual(entering, [parsing[0] + 1])
                statement = body[entering[0]]
                self.assertIsInstance(statement, ast.Expr)
                self.assertEqual(called_names(statement), [enter])

    def test_no_command_line_path_is_left_relative_to_the_working_directory(self) -> None:
        # A type=Path option stays relative, so after the chdir it would name a path below the repository root.
        for script in sorted([*paths.SCRIPTS_ROOT.glob("*.py"), *(paths.SCRIPTS_ROOT / "Lib").glob("*.py")]):
            with self.subTest(script=script.name):
                tree = ast.parse(script.read_text(encoding="utf-8"))
                relative = [node.lineno for node in ast.walk(tree)
                            if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                            and node.func.attr == "add_argument"
                            for keyword in node.keywords
                            if keyword.arg == "type" and isinstance(keyword.value, ast.Name)
                            and keyword.value.id == "Path"]
                self.assertEqual(relative, [])

    def test_command_line_paths_are_made_absolute_against_the_callers_directory(self) -> None:
        absolute = REPOSITORY_ROOT / "bin"
        with tempfile.TemporaryDirectory() as directory, working_directory(Path(directory)):
            caller = Path.cwd()
            self.assertEqual(paths.argument_path("Report.xml"), caller / "Report.xml")
            self.assertEqual(paths.argument_path("../Game"), caller / ".." / "Game")
            self.assertEqual(paths.argument_path(str(absolute)), absolute)
            # A bare tool name stays a PATH lookup; a tool given by a path is a file in the caller's directory.
            self.assertEqual(paths.tool_argument("clang-format"), "clang-format")
            self.assertEqual(paths.tool_argument("Tools/clang-format"), str(caller / "Tools" / "clang-format"))
            self.assertEqual(paths.tool_argument(str(absolute)), str(absolute))

    def test_the_gates_and_test_py_read_relative_arguments_in_the_callers_directory_and_run_in_the_root(self) -> None:
        seen: dict[str, tuple[Path, object]] = {}

        def stages(arguments: argparse.Namespace, console: Console, host: paths.Host) -> int:
            seen["CI.py"] = (Path.cwd(), arguments.summary_junit)
            return 0

        def step(name: str, script: str, arguments: list[str], console: Console) -> Step:
            seen.setdefault("PreCommit.py", (Path.cwd(), None))
            return Step(name, Status.PASSED, "recorded")

        def merge(arguments: argparse.Namespace, console: Console) -> int:
            seen["Test.py"] = (Path.cwd(), [Path(report).resolve() for report in arguments.merge_coverage])
            return 0

        reports = ["Editor.json", "Reports/Release.json", "Reports/Dist.json"]
        with tempfile.TemporaryDirectory() as elsewhere, working_directory(Path(elsewhere)), \
                mock.patch.object(CI, "take_gate_locks", return_value=iter(())), \
                mock.patch.object(CI, "run_stages", stages), mock.patch.object(PreCommit, "run_step", step), \
                mock.patch.object(PreCommit, "run_static_checks", return_value=[]), \
                mock.patch.object(test_script, "run_merge", merge), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            caller = Path(elsewhere).resolve()
            self.assertEqual(CI.main(["--stages", "lint", "--summary-junit", "Relative.xml"]), 0)
            os.chdir(elsewhere)
            self.assertEqual(PreCommit.main([]), 0)
            os.chdir(elsewhere)
            self.assertEqual(test_script.main(["--merge-coverage", *reports]), 0)
        root = paths.REPOSITORY_ROOT
        self.assertEqual(seen, {
            "CI.py": (root, caller / "Relative.xml"),
            "PreCommit.py": (root, None),
            "Test.py": (root, [caller / report for report in reports]),
        })

    def test_an_entry_script_started_elsewhere_reads_a_relative_argument_in_the_callers_directory(self) -> None:
        clean_map = REPOSITORY_ROOT / "Tests" / "Data" / "MapFiles" / "msvc" / "Clean.map"
        self.assertFalse((REPOSITORY_ROOT / clean_map.name).exists())
        with tempfile.TemporaryDirectory() as elsewhere:
            shutil.copyfile(clean_map, Path(elsewhere) / clean_map.name)
            completed = subprocess.run([sys.executable, str(paths.SCRIPTS_ROOT / "CheckRuntimeMap.py"), "--map",
                                        clean_map.name, "--json"], cwd=elsewhere, capture_output=True, text=True,
                                       encoding="utf-8", env={**os.environ, "GITHUB_ACTIONS": ""}, timeout=300)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertTrue(json.loads(completed.stdout)["success"])

    def test_the_python_runner_reads_relative_arguments_in_the_callers_directory_and_runs_in_the_root(self) -> None:
        with tempfile.TemporaryDirectory() as elsewhere:
            suite = Path(elsewhere) / "Suite"
            suite.mkdir()
            (suite / "test_working_directory.py").write_text(RUNNER_PROBE, encoding="utf-8")
            completed = subprocess.run([sys.executable, str(paths.SCRIPTS_ROOT / "Lib" / "unittest_runner.py"),
                                        "--junit", "Report.xml", "--name", "Probe", "Suite"], cwd=elsewhere,
                                       capture_output=True, text=True, encoding="utf-8", timeout=300,
                                       env={**os.environ, RUNNER_PROBE_ROOT: str(REPOSITORY_ROOT)})
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            report = ElementTree.parse(Path(elsewhere) / "Report.xml").getroot()
        self.assertEqual([element.get("name") for element in report.iter("testsuite")], ["Probe.Suite"])

    def test_the_python_runner_labels_suites_by_their_path_below_the_repository_root(self) -> None:
        with tempfile.TemporaryDirectory() as elsewhere, working_directory(Path(elsewhere)):
            self.assertEqual(unittest_runner.suite_label(REPOSITORY_ROOT / "Tools" / "MCP" / "tests"),
                             "Tools.MCP.tests")

if __name__ == "__main__":
    unittest.main()
