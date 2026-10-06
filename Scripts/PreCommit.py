#!/usr/bin/env python3
"""The commit gate (Docs/Architecture.md §15.9): regenerate, the static checks, Debug build, unit + feature suites.

Steps, in order:
  generate  Scripts/Generate.py, so the build sees the current premake scripts
  static    the static checks of CI.py's lint stage, shared through Scripts/Lib/scripts.py so the two gates cannot
            drift apart (§15.1 T0, §15.8 "lint"):
              checkbuildconfig  Scripts/CheckBuildConfig.py on the workspace (ABI defines,
                                JPH_CROSS_PLATFORM_DETERMINISTIC, the Jolt instruction set, the precise floating-point
                                model, the Dist solution) and on every fixture workspace under Tests/Data/BuildConfig/,
                                each of which must fail
              lint              Scripts/Lint.py, and Lint.py --self-test
              format            Scripts/Format.py --check
  build     Scripts/Build.py --config Debug (skipped when generate failed)
  tests     Scripts/Test.py --suite unit,feature --config Debug (skipped when the build failed); a suite that does not
            exist yet is reported as not available by Test.py and does not fail the gate
The static checks run even after an earlier failure, so one run reports every problem. A missing script fails its
step.

Exit codes: 0 every step passed, otherwise the code of the first failing step (1 failed, 2 usage error, 3 a required
tool or file is missing, 5 timeout).
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"PreCommit.py requires Python 3.10 or newer (this is Python {platform.python_version()})")

import argparse

from Lib.report import (
    EXIT_FAILED,
    EXIT_SUCCESS,
    Console,
    Status,
    Step,
    configure_stdio,
    emit_json,
    overall_exit_code,
)
from Lib.scripts import run_script, run_static_checks

TIMEOUTS = {"generate": 600.0, "build": 7200.0, "tests": 3600.0}


def run_step(name: str, script: str, arguments: list[str], console: Console) -> Step:
    result = run_script(name, script, arguments, console, TIMEOUTS[name])
    console.result(result)
    return result


def skipped(name: str, reason: str, console: Console) -> Step:
    step = Step(name, Status.NOT_RUN, reason)
    console.result(step)
    return step


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    parser = argparse.ArgumentParser(
        description="Commit gate: generate, the static checks (CheckBuildConfig, Lint, Lint self-test, format check), "
        "Debug build, unit and feature suites (§15.9).",
        epilog="Exit codes: 0 every step passed, else the first failing step's code (1 failed, 2 usage, 3 missing "
               "tool or file, 5 timeout).",
    )
    parser.add_argument("--json", action="store_true", help="print a machine-readable result on stdout")
    arguments = parser.parse_args(sys.argv[1:] if argv is None else argv)
    console = Console(arguments.json)

    def finished(step: Step) -> Step:
        console.result(step)
        return step

    generate = run_step("generate", "Generate.py", [], console)
    steps = [generate, *run_static_checks(console, finished)]
    if generate.failed:
        steps.append(skipped("build", "generate failed", console))
    else:
        steps.append(run_step("build", "Build.py", ["--config", "Debug"], console))
    if steps[-1].status != Status.PASSED:
        steps.append(skipped("tests", "the Debug build did not pass", console))
    else:
        steps.append(run_step("tests", "Test.py", ["--suite", "unit,feature", "--config", "Debug"], console))

    exit_code = overall_exit_code(steps)
    if exit_code == EXIT_SUCCESS and any(step.status == Status.NOT_RUN for step in steps):
        exit_code = EXIT_FAILED
    console.summary("PreCommit summary", steps)
    console.print(f"\nPreCommit {'passed' if exit_code == EXIT_SUCCESS else 'FAILED'} (exit code {exit_code})")
    if arguments.json:
        emit_json({"success": exit_code == EXIT_SUCCESS, "exitCode": exit_code,
                   "steps": [step.to_json() for step in steps]})
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
