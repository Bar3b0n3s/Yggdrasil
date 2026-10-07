"""Running the other developer scripts as steps (used by CI.py and PreCommit.py), the static checks both gates run, and
the contract mode both gates share.

Each script runs in a child process with this interpreter, so it behaves exactly as when it is run by hand; its output
streams through. The scripts print "[FAILED] <step>: <detail>" for each failing step, and the last such line becomes
part of the step detail here.
"""

from __future__ import annotations

import re
import sys
from collections.abc import Callable
from pathlib import Path

from . import paths
from .process import child_environment, format_command, run_streamed
from .report import EXIT_FAILED, EXIT_INIT_FAILED, EXIT_TIMEOUT, EXIT_USAGE, Console, Status, Step

_PROPAGATED_EXIT_CODES = (EXIT_FAILED, EXIT_USAGE, EXIT_INIT_FAILED, EXIT_TIMEOUT)


def run_script(name: str, script: str, arguments: list[str], console: Console, timeout: float,
               expected_exit: int = 0, expected_output: re.Pattern[str] | None = None) -> Step:
    """Run Scripts/<script> with `arguments`. The step passes when the script exits with `expected_exit` and, when
    `expected_output` is given, at least one output line matches it (so an expected failure fails for the expected
    reason)."""
    path = paths.SCRIPTS_ROOT / script
    if not path.is_file():
        return Step(name, Status.FAILED, f"Scripts/{script} does not exist (Architecture §2.3)",
                    exit_code=EXIT_INIT_FAILED)
    console.heading(f"{name}: {format_command([Path(sys.executable).name, f'Scripts/{script}', *arguments])}")
    result = run_streamed([sys.executable, str(path), *arguments], cwd=paths.REPOSITORY_ROOT, env=child_environment(),
                          timeout=timeout, echo=console.stream, collect=expected_output)
    if result.timed_out:
        # The end of its output shows how far it got (Lint.py reports each step's duration as it finishes).
        return Step(name, Status.TIMEOUT, f"Scripts/{script} {result.describe_exit()}", result.duration,
                    data={"outputTail": result.tail(40)})
    if result.exit_code != expected_exit:
        # Propagate the script's §4.1 exit code; an unexpected result of an expected-failure run is a plain failure.
        code = result.exit_code if expected_exit == 0 and result.exit_code in _PROPAGATED_EXIT_CODES else EXIT_FAILED
        expectation = "" if expected_exit == 0 else f", expected exit code {expected_exit}"
        reported = [line for line in result.output.splitlines() if line.startswith(("[FAILED] ", "[TIMEOUT] "))]
        cause = f": {reported[-1]}" if reported else ""
        return Step(name, Status.FAILED, f"Scripts/{script} {result.describe_exit()}{expectation}{cause}",
                    result.duration, exit_code=code, data={"outputTail": result.tail(40)})
    if expected_output is not None and not result.collected:
        return Step(name, Status.FAILED, f"Scripts/{script} {result.describe_exit()} as expected, but no output line "
                                         f"matches /{expected_output.pattern}/", result.duration,
                    data={"outputTail": result.tail(40)})
    # An expected failure passes: say so, because its own output reports a failure.
    expectation = " as required" if expected_exit != 0 else ""
    detail = f"; {result.collected[0].strip()}" if result.collected else ""
    return Step(name, Status.PASSED, f"Scripts/{script} {result.describe_exit()}{expectation}{detail}",
                result.duration)


# --------------------------------------------------------------------------------------------------------------------
# Contract mode (Roadmap rule 3, Docs/Decisions/0004-contract-stub-gate.md), shared by PreCommit.py and CI.py
# --------------------------------------------------------------------------------------------------------------------

CONTRACT_MODE_NOTE = ("contract mode: stubs and skipped tests allowed (Lint.py --allow-contract-stubs, Test.py "
                      "--allow-skips); only for the commit of a milestone's contract task")
STRICT_MODE_NOTE = ("strict mode: no contract stubs (ENGINE_CONTRACT_STUB) and no skipped test cases outside the "
                    "child-process targets (Test::ChildTargetSuite)")
CONTRACT_FLAG_HELP = ("contract mode, only for the commit of a milestone's contract task (Roadmap rule 3): allow "
                      "ENGINE_CONTRACT_STUB stubs and doctest::skip test cases (Lint.py --allow-contract-stubs, "
                      "Test.py --allow-skips). Without it the gate is strict")


def mode_note(contract: bool) -> str:
    """The line PreCommit.py and CI.py print at the start and in the summary of a run."""
    return CONTRACT_MODE_NOTE if contract else STRICT_MODE_NOTE


def lint_mode_arguments(contract: bool) -> list[str]:
    """Lint.py arguments for the static checks of PreCommit.py and CI.py (its self-test always runs strict)."""
    return ["--allow-contract-stubs"] if contract else []


def test_mode_arguments(contract: bool) -> list[str]:
    """Test.py arguments for the unit runs of PreCommit.py and CI.py."""
    return ["--allow-skips"] if contract else []


# --------------------------------------------------------------------------------------------------------------------
# The static checks (the lint stage of CI.py and of PreCommit.py)
# --------------------------------------------------------------------------------------------------------------------

BUILD_FIXTURES_ROOT = paths.REPOSITORY_ROOT / "Tests" / "Data" / "BuildConfig"
# Every fixture workspace under Tests/Data/BuildConfig/, with the finding that proves CheckBuildConfig.py detects its
# defect (Roadmap M0 acceptance). A fixture directory without an entry here fails the static checks.
BUILD_FIXTURE_FINDINGS = {
    "MismatchedJoltDefine": re.compile(r"Tests includes JoltPhysics headers but lacks JPH_PROFILE_ENABLED"),
    "MissingDeterministicDefine": re.compile(r"includes Jolt headers but does not define "
                                             r"JPH_CROSS_PLATFORM_DETERMINISTIC"),
    "FastMathJolt": re.compile(r"JoltPhysics uses a non-precise floating-point model"),
    "UndefinedJoltDefine": re.compile(r"Tests includes JoltPhysics headers but lacks JPH_PROFILE_ENABLED"),
    "JoltDefineInBuildOptions": re.compile(r"Engine includes JoltPhysics headers but defines JPH_DOUBLE_PRECISION"),
    "LuauVectorSize": re.compile(r"Engine includes Luau headers but defines LUA_VECTOR_SIZE=4"),
    "ImGuiDrawIndex": re.compile(r"Engine includes ImGui headers but defines ImDrawIdx=unsigned int"),
    "ConsumerInstructionSet": re.compile(r"Tests is compiled for a different instruction set .* than JoltPhysics"),
    "FastMathComponents": re.compile(r"JoltPhysics uses a non-precise floating-point model: fast-math components are "
                                     r"in effect \(.*finite math only"),
    "FastTranscendentals": re.compile(r"\[windows\] .*Engine uses a non-precise floating-point model: "
                                      r"/Qfast_transcendentals"),
    "ClangOnlyContraction": re.compile(r"\[linux-clang\] .*JoltPhysics uses a non-precise floating-point model: "
                                       r"-ffp-contract is 'fast'"),
}

STATIC_CHECK_TIMEOUTS = {"checkbuildconfig": 1800.0, "lint": 3600.0, "format": 1800.0}


def run_static_checks(console: Console, finished: Callable[[Step], Step], contract: bool = False) -> list[Step]:
    """The static checks of §15.8 (T0 of §15.1), shared by CI.py's lint stage and PreCommit.py so the two gates cannot
    drift apart: CheckBuildConfig.py on the workspace and on every fixture workspace (each must fail with its own
    defect), Lint.py, Lint.py --self-test and Format.py --check. Each runs even after an earlier one failed, so one run
    reports every problem. `finished` receives every step as soon as it is known (to print it) and returns it.
    `contract` selects contract mode: Lint.py allows contract stubs and skipped tests (its self-test stays strict)."""

    def script(name: str, script_name: str, arguments: list[str], timeout: float, expected_exit: int = 0,
               expected_output: re.Pattern[str] | None = None) -> Step:
        return finished(run_script(name, script_name, arguments, console, timeout, expected_exit, expected_output))

    steps = [script("checkbuildconfig workspace", "CheckBuildConfig.py", [], STATIC_CHECK_TIMEOUTS["checkbuildconfig"])]
    steps += build_fixture_checks(script, finished)
    steps.append(script("lint", "Lint.py", lint_mode_arguments(contract), STATIC_CHECK_TIMEOUTS["lint"]))
    steps.append(script("lint self-test", "Lint.py", ["--self-test"], STATIC_CHECK_TIMEOUTS["lint"]))
    steps.append(script("format", "Format.py", ["--check"], STATIC_CHECK_TIMEOUTS["format"]))
    return steps


def build_fixture_checks(script: Callable[..., Step], finished: Callable[[Step], Step]) -> list[Step]:
    """Each fixture is the real workspace with one defect: CheckBuildConfig.py must exit 1 and report that defect."""
    present = sorted(path.parent.name for path in BUILD_FIXTURES_ROOT.glob("*/premake5.lua"))
    missing = sorted(set(BUILD_FIXTURE_FINDINGS) - set(present))
    unknown = sorted(set(present) - set(BUILD_FIXTURE_FINDINGS))
    problems = []
    if missing:
        problems.append(f"missing fixture workspace(s): {', '.join(missing)}")
    if unknown:
        problems.append(f"fixture workspace(s) without an expected finding in Scripts/Lib/scripts.py: "
                        f"{', '.join(unknown)}")
    if problems:
        return [finished(Step("checkbuildconfig fixtures", Status.FAILED,
                              f"{paths.display_path(BUILD_FIXTURES_ROOT)}: {'; '.join(problems)}"))]
    return [script(f"checkbuildconfig fixture {name}", "CheckBuildConfig.py",
                   ["--workspace", str(BUILD_FIXTURES_ROOT / name)], STATIC_CHECK_TIMEOUTS["checkbuildconfig"],
                   expected_exit=EXIT_FAILED, expected_output=BUILD_FIXTURE_FINDINGS[name])
            for name in present]
