#!/usr/bin/env python3
"""Check the Runtime's linker map for code that must not ship (Docs/Architecture.md §14.4; Roadmap M15 acceptance "Dist:
map file contains no automation, Analysis, ImGui or AssetPipeline symbols"; Docs/Decisions/0024-m15-contract.md
decision 11).

Reads bin/<Config>-<system>-<arch>/Runtime/Runtime.map (or --map), in any format Lib/runtime_map.py reads (MSVC
link.exe, LLVM lld, GNU ld, Apple ld64), and reports every symbol of the forbidden families (automation, Luau Analysis,
ImGui, AssetPipeline, derived from their headers) and every required FeatureTest harness symbol that is missing. CI.py
runs it right after its Dist build. --self-test checks the seeded fixtures of Tests/Data/MapFiles/<format>/ instead:
Clean.map has no finding and every other map exactly the findings its Expected.json lists, in every format; the static
checks run it.
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
import time
from pathlib import Path

from Lib import paths, runtime_map
from Lib.report import (EXIT_FAILED, EXIT_INIT_FAILED, Console, Status, Step, configure_stdio, emit_json,
                        overall_exit_code)

MAP_FIXTURES = paths.REPOSITORY_ROOT / "Tests" / "Data" / "MapFiles"
FIXTURE_CASES = ("Clean", *runtime_map.FORBIDDEN_FAMILIES, "MissingRequired")
# Findings printed per family; --json carries every one.
SHOWN_FINDINGS = 20


def default_map(config: str) -> Path:
    return paths.output_directory(config) / "Runtime" / "Runtime.map"


def summarize(findings: list[str]) -> str:
    """"N finding(s): <family> <count>, ...; missing <names>" for a non-empty list."""
    counts = collections.Counter(finding.split(":", 1)[0].removeprefix("forbidden ") for finding in findings
                                 if finding.startswith("forbidden "))
    parts = [f"{family} {counts[family]}" for family in runtime_map.FORBIDDEN_FAMILIES if counts[family]]
    missing = [finding.removeprefix("missing required symbol: ") for finding in findings
               if finding.startswith("missing required symbol: ")]
    if missing:
        parts.append("missing " + ", ".join(missing))
    return f"{len(findings)} finding(s): " + "; ".join(parts)


def check(map_path: Path, console: Console) -> Step:
    started = time.perf_counter()
    name = f"runtime map {paths.display_path(map_path)}"
    if not map_path.is_file():
        return Step(name, Status.FAILED, f"{paths.display_path(map_path)} does not exist: build the Runtime first "
                                         f"(Build.py --config <config>; Runtime/premake5.lua links a map in every "
                                         f"configuration)", exit_code=EXIT_INIT_FAILED)
    try:
        parsed = runtime_map.parse_map(map_path.read_text(encoding="utf-8", errors="replace"))
        patterns = runtime_map.family_patterns(paths.REPOSITORY_ROOT)
    except (OSError, ValueError) as error:
        return Step(name, Status.FAILED, f"{paths.display_path(map_path)}: {error}", time.perf_counter() - started)
    findings = runtime_map.check_map(parsed, patterns)
    data = {"map": paths.display_path(map_path), "format": parsed.format, "symbols": len(parsed.symbols),
            "findings": findings}
    duration = time.perf_counter() - started
    if not findings:
        return Step(name, Status.PASSED, f"{parsed.format} map, {len(parsed.symbols)} symbols: no automation, "
                                         f"Analysis, ImGui or AssetPipeline symbol; "
                                         f"{', '.join(runtime_map.REQUIRED_SYMBOLS)} present", duration, data=data)
    shown: dict[str, int] = collections.Counter()
    for finding in findings:
        family = finding.split(":", 1)[0]
        shown[family] += 1
        if shown[family] <= SHOWN_FINDINGS:
            console.print(f"  {finding}")
        elif shown[family] == SHOWN_FINDINGS + 1:
            console.print(f"  ... more {family} findings (--json lists every one)")
    return Step(name, Status.FAILED, summarize(findings), duration, exit_code=EXIT_FAILED, data=data)


def self_test() -> list[Step]:
    """Every format's fixtures against their Expected.json (the static checks' run)."""
    started = time.perf_counter()
    try:
        patterns = runtime_map.family_patterns(paths.REPOSITORY_ROOT)
    except (OSError, ValueError) as error:
        return [Step("runtime map self-test", Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED)]
    steps: list[Step] = []
    for map_format in runtime_map.MAP_FORMATS:
        directory = MAP_FIXTURES / map_format
        name = f"runtime map self-test {map_format}"
        problems: list[str] = []
        try:
            expected = json.loads((directory / "Expected.json").read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            steps.append(Step(name, Status.FAILED, f"{paths.display_path(directory / 'Expected.json')}: {error}",
                              exit_code=EXIT_INIT_FAILED))
            continue
        if not isinstance(expected, dict) or sorted(expected) != sorted(FIXTURE_CASES):
            problems.append(f"Expected.json must list exactly {', '.join(FIXTURE_CASES)}")
        else:
            for case in FIXTURE_CASES:
                path = directory / f"{case}.map"
                try:
                    parsed = runtime_map.parse_map(path.read_text(encoding="utf-8"))
                except (OSError, ValueError) as error:
                    problems.append(f"{case}.map: {error}")
                    continue
                if parsed.format != map_format:
                    problems.append(f"{case}.map reads as {parsed.format}, not {map_format}")
                findings = runtime_map.check_map(parsed, patterns)
                if findings != expected[case]:
                    unexpected = [finding for finding in findings if finding not in expected[case]]
                    absent = [finding for finding in expected[case] if finding not in findings]
                    problems.append(f"{case}.map: unexpected {unexpected}, missing {absent}")
                elif case != "Clean" and not findings:
                    problems.append(f"{case}.map has no finding: a seeded fixture must fail")
        if problems:
            steps.append(Step(name, Status.FAILED, "; ".join(problems), exit_code=EXIT_FAILED))
        else:
            steps.append(Step(name, Status.PASSED, f"{len(FIXTURE_CASES)} fixtures have exactly their findings"))
    duration = time.perf_counter() - started
    for step in steps:
        step.duration = duration / len(steps)
    return steps


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Exit codes: 0 clean, 1 findings (or a map or fixture that cannot be read), 2 usage error, 3 the map is "
               "missing.",
    )
    parser.add_argument("--map", type=Path, help="the map file to check (default: the --config Runtime's map)")
    parser.add_argument("--config", choices=paths.CONFIGURATIONS, default="Dist",
                        help="the build configuration whose Runtime map is checked (default: Dist)")
    parser.add_argument("--self-test", action="store_true",
                        help="check the seeded fixtures of Tests/Data/MapFiles/ instead of a build's map")
    parser.add_argument("--json", action="store_true", help="emit a machine-readable result")
    options = parser.parse_args(sys.argv[1:] if argv is None else argv)
    if options.self_test and options.map is not None:
        parser.error("--self-test checks the fixtures and takes no --map")
    console = Console(options.json)
    if options.self_test:
        steps = self_test()
    else:
        try:
            map_path = options.map.resolve() if options.map is not None else default_map(options.config)
        except paths.UnsupportedHostError as error:
            parser.error(str(error))
        steps = [check(map_path, console)]
    for step in steps:
        console.result(step)
    code = overall_exit_code(steps)
    if options.json:
        emit_json({"success": code == 0, "exitCode": code, "steps": [step.to_json() for step in steps]})
    return code


if __name__ == "__main__":
    sys.exit(main())
