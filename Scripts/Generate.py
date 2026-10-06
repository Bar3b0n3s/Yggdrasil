#!/usr/bin/env python3
"""Generate the workspace with the pinned premake (Docs/Architecture.md §2.2, §2.3).

Default action per host: vs2026 (Windows), gmake (Linux), xcode4 (macOS); ninja is available on Linux and macOS,
gmake also on macOS. premake runs with --fatal, so a warning from the premake scripts fails generation.

When generating for the host, the custom `compile-commands` action (Scripts/Premake/CompileCommands.lua, included by
premake5.lua) then writes compile_commands.json for clang-tidy and clangd.

Exit codes: 0 success, 1 premake failed, 2 usage error, 3 premake is not installed (run Scripts/Setup.py),
5 premake timed out.
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"Generate.py requires Python 3.10 or newer (this is Python {platform.python_version()})")

import argparse
from pathlib import Path

from Lib import paths, premake
from Lib.process import ToolNotFoundError
from Lib.report import (
    EXIT_INIT_FAILED,
    EXIT_SUCCESS,
    EXIT_USAGE,
    Console,
    Status,
    Step,
    configure_stdio,
    emit_json,
    overall_exit_code,
)

COMPILE_COMMANDS_ACTION = "compile-commands"
GENERATE_TIMEOUT_SECONDS = 300.0
SYSTEMS = ("windows", "linux", "macosx")


def premake_arguments(action: str, target_os: str, toolset: str | None, location: Path | None) -> list[str]:
    # Build steps (the shader rule) run this verified Python 3.10+ interpreter, not whatever the IDE's PATH finds.
    arguments = ["--fatal", f"--os={target_os}", f"--python={Path(sys.executable).resolve().as_posix()}"]
    # "msc" is the vs2026 default (msc-v145); every other toolset is selected explicitly.
    if toolset and toolset != "msc":
        arguments.append(f"--cc={toolset}")
    if location is not None:
        arguments.append(f"--to={location}")
    arguments.append(action)
    return arguments


def generate(action: str, target_os: str, toolset: str | None, location: Path | None, console: Console,
             verbose: bool) -> Step:
    """Run one premake generator action. The step's data lists the files premake wrote (it skips unchanged ones)."""
    arguments = premake_arguments(action, target_os, toolset, location)
    name = f"generate {action} ({target_os})"
    result = premake.run(arguments, timeout=GENERATE_TIMEOUT_SECONDS)
    files = premake.generated_files(result.output)
    if verbose or not result.succeeded:
        console.print(result.output.rstrip())
    if result.timed_out:
        return Step(name, Status.TIMEOUT, f"premake {result.describe_exit()}", result.duration)
    if not result.succeeded:
        return Step(name, Status.FAILED, f"premake {' '.join(arguments)} failed ({result.describe_exit()})",
                    result.duration, data={"output": result.tail(40)})
    where = paths.display_path(location) if location is not None else "in place"
    written = f"{len(files)} file(s) written" if files else "up to date, no file changed"
    return Step(name, Status.PASSED, f"{written} ({where})", result.duration, data={"generatedFiles": files})


def compile_commands(location: Path | None, console: Console, verbose: bool) -> Step:
    """Run the custom compile-commands action that premake5.lua registers (Scripts/Premake/CompileCommands.lua)."""
    name = COMPILE_COMMANDS_ACTION
    destination = [f"--to={location}"] if location is not None else []
    arguments = ["--fatal", *destination, COMPILE_COMMANDS_ACTION]
    result = premake.run(arguments, timeout=GENERATE_TIMEOUT_SECONDS)
    if verbose or not result.succeeded:
        console.print(result.output.rstrip())
    if result.timed_out:
        return Step(name, Status.TIMEOUT, f"premake {result.describe_exit()}", result.duration)
    if not result.succeeded:
        return Step(name, Status.FAILED, f"premake {' '.join(arguments)} failed ({result.describe_exit()})",
                    result.duration, data={"output": result.tail(40)})
    output = (location or paths.REPOSITORY_ROOT) / "compile_commands.json"
    return Step(name, Status.PASSED, paths.display_path(output), result.duration)


def parse_arguments(argv: list[str], host: paths.Host) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate the workspace (premake5.lua) with the pinned premake 5.0.0.",
        epilog="Exit codes: 0 success, 1 premake failed, 2 usage error, 3 premake is not installed, 5 timeout.",
    )
    actions = sorted({action for system in premake.ACTIONS_BY_SYSTEM.values() for action in system})
    defaults = ", ".join(f"{system} {premake.default_action(system)}" for system in SYSTEMS)
    parser.add_argument("--action", choices=actions,
                        help=f"generator (default for --os: {defaults})")
    parser.add_argument("--os", dest="target_os", choices=SYSTEMS, default=host.system,
                        help=f"target operating system (default: the host, {host.system})")
    parser.add_argument("--toolset", choices=("msc", "gcc", "clang"),
                        help="compiler toolset: msc or clang (clang-cl) for vs2026, gcc or clang for gmake/ninja "
                             "(default: premake's, gcc on Linux), clang for xcode4")
    parser.add_argument("--to", type=Path, help="write the generated files to this directory instead of in place")
    parser.add_argument("--no-compile-commands", action="store_true",
                        help="do not run the compile-commands action after generating")
    parser.add_argument("--verbose", action="store_true", help="print premake's output")
    parser.add_argument("--json", action="store_true", help="print a machine-readable result on stdout")
    arguments = parser.parse_args(argv)
    arguments.action = arguments.action or premake.default_action(arguments.target_os)
    if arguments.action not in premake.ACTIONS_BY_SYSTEM[arguments.target_os]:
        parser.error(f"--action {arguments.action} does not target {arguments.target_os} "
                     f"(available: {', '.join(premake.ACTIONS_BY_SYSTEM[arguments.target_os])})")
    if arguments.toolset and arguments.toolset not in premake.TOOLSETS_BY_ACTION[arguments.action]:
        parser.error(f"--toolset {arguments.toolset} is not available for {arguments.action} "
                     f"(available: {', '.join(premake.TOOLSETS_BY_ACTION[arguments.action])})")
    if arguments.to is not None:
        arguments.to = arguments.to.resolve()
    return arguments


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    try:
        host = paths.host()
    except paths.UnsupportedHostError as error:
        print(f"Generate.py: error: {error}", file=sys.stderr)
        return EXIT_USAGE
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv, host)
    console = Console(arguments.json)

    try:
        premake.find_premake()
    except ToolNotFoundError as error:
        step = Step(f"generate {arguments.action} ({arguments.target_os})", Status.FAILED, str(error),
                    exit_code=EXIT_INIT_FAILED)
        console.result(step)
        if arguments.json:
            emit_json({"success": False, "action": arguments.action, "os": arguments.target_os,
                       "steps": [step.to_json()]})
        return EXIT_INIT_FAILED

    console.heading(f"Generate {arguments.action} ({arguments.target_os})")
    steps = [generate(arguments.action, arguments.target_os, arguments.toolset, arguments.to, console,
                      arguments.verbose)]
    console.result(steps[-1])
    if not steps[-1].failed:
        if arguments.no_compile_commands:
            steps.append(Step(COMPILE_COMMANDS_ACTION, Status.SKIPPED, "--no-compile-commands given"))
        elif arguments.target_os != host.system:
            steps.append(Step(COMPILE_COMMANDS_ACTION, Status.SKIPPED,
                              f"compile_commands.json is generated for the host ({host.system}) only"))
        else:
            steps.append(compile_commands(arguments.to, console, arguments.verbose))
        console.result(steps[-1])

    exit_code = overall_exit_code(steps)
    if arguments.json:
        emit_json({
            "success": exit_code == EXIT_SUCCESS,
            "action": arguments.action,
            "os": arguments.target_os,
            "toolset": arguments.toolset,
            "location": arguments.to.as_posix() if arguments.to else paths.REPOSITORY_ROOT.as_posix(),
            "steps": [step.to_json() for step in steps],
        })
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
