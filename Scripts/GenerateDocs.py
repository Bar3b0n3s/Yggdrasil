#!/usr/bin/env python3
"""Generate registry-owned references, or compare them with committed files (§2.3, §11.9, §13.10).

Runs the freshly built Editor without a window, renderer, audio device or project, which writes the whole reference set
(ADR 0022 decision 13). Methods.json, the Markdown references and Schemas/ go to Docs/Reference/, which is fully
generated: a stale, missing or extra file fails --check, and write mode deletes extras. Engine.d.luau goes to
Resources/Scripting/ and the MCP catalogue to Tools/MCP/. Check mode never rewrites a reference. Temporary user data,
output and any crash reports remain inside one disposable directory.

It works from any directory (Docs/Architecture.md §2.3): once it has parsed its arguments, main() makes the repository
root of its own checkout the working directory.
"""

from __future__ import annotations

import argparse
import sys
import tempfile
import time
from pathlib import Path

from Lib import paths
from Lib.process import ToolNotFoundError, child_environment, run_streamed
from Lib.report import Console, Status, Step, configure_stdio, emit_json, overall_exit_code


REFERENCE_DIRECTORY = "Docs/Reference"
# Dump files that live outside Docs/Reference/.
RELOCATED = {
    "Engine.d.luau": "Resources/Scripting/Engine.d.luau",
    "catalog.json": "Tools/MCP/catalog.json",
}


def destination_of(relative: str) -> str | None:
    """The repository path of a dump file ('/'-separated), or None for a file the reference set does not contain."""
    if relative in RELOCATED:
        return RELOCATED[relative]
    if relative == "Methods.json" or (relative.endswith(".md") and "/" not in relative):
        return f"{REFERENCE_DIRECTORY}/{relative}"
    if relative.startswith("Schemas/") and relative.endswith(".schema.json") and relative.count("/") == 1:
        return f"{REFERENCE_DIRECTORY}/{relative}"
    return None


def collect_generated(output: Path) -> tuple[dict[str, bytes], list[str]]:
    """Every dumped file by repository path, and the dump files that map to no destination."""
    generated: dict[str, bytes] = {}
    unexpected: list[str] = []
    for file in sorted(path for path in output.rglob("*") if path.is_file()):
        relative = file.relative_to(output).as_posix()
        destination = destination_of(relative)
        if destination is None:
            unexpected.append(relative)
        else:
            generated[destination] = file.read_bytes()
    return generated, unexpected


def committed_references() -> set[str]:
    """Every file under Docs/Reference/, as repository paths."""
    directory = paths.REPOSITORY_ROOT / REFERENCE_DIRECTORY
    if not directory.is_dir():
        return set()
    return {path.relative_to(paths.REPOSITORY_ROOT).as_posix() for path in directory.rglob("*") if path.is_file()}


def remove_empty_directories(directory: Path) -> None:
    """Removes the empty directories below `directory` (deepest first), never `directory` itself."""
    directories = [entry for entry in directory.rglob("*") if entry.is_dir()]
    for path in sorted(directories, key=lambda entry: len(entry.parts), reverse=True):
        if not any(path.iterdir()):
            path.rmdir()


def generate(config: str, check: bool, console: Console) -> Step:
    started = time.perf_counter()
    editor = paths.output_directory(config) / "Editor" / paths.executable_name("Editor")
    if not editor.is_file():
        return Step("references", Status.FAILED, f"build the {config} Editor before generating references", exit_code=3)
    try:
        with tempfile.TemporaryDirectory(prefix="engine-reference-") as temporary:
            root = Path(temporary)
            output = root / "reference"
            result = run_streamed(
                [str(editor), "--headless", "--renderer", "none", "--audio-device", "none",
                 "--user-data-dir", str(root / "user"), "--engine-cache-dir", str(root / "cache"),
                 "--dump-reference", str(output)], cwd=paths.REPOSITORY_ROOT, env=child_environment(),
                timeout=120, echo=console.stream,
            )
            if result.timed_out:
                return Step("references", Status.TIMEOUT, f"reference editor {result.describe_exit()}",
                            time.perf_counter() - started, data={"outputTail": result.tail(30)})
            if not result.succeeded:
                return Step("references", Status.FAILED, f"reference editor {result.describe_exit()}",
                            time.perf_counter() - started, data={"outputTail": result.tail(30)})
            generated, unexpected = collect_generated(output)
            if unexpected:
                return Step("references", Status.FAILED, "the reference editor wrote files outside the reference set: "
                            + ", ".join(unexpected), time.perf_counter() - started)
            stale = sorted(name for name, data in generated.items()
                           if not (paths.REPOSITORY_ROOT / name).is_file()
                           or (paths.REPOSITORY_ROOT / name).read_bytes() != data)
            extra = sorted(committed_references() - generated.keys())
            data = {"generated": sorted(generated), "stale": stale, "extra": extra}
            if check:
                if stale or extra:
                    problems = []
                    if stale:
                        problems.append("stale or missing generated references: " + ", ".join(stale))
                    if extra:
                        problems.append("files in Docs/Reference that no registry generates: " + ", ".join(extra))
                    return Step("references", Status.FAILED, "; ".join(problems), time.perf_counter() - started,
                                data=data)
                return Step("references", Status.PASSED, f"verified {len(generated)} generated references",
                            time.perf_counter() - started, data=data)
            for name in stale:
                destination = paths.REPOSITORY_ROOT / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                staging = destination.with_name(destination.name + ".generating")
                staging.write_bytes(generated[name])
                staging.replace(destination)
            for name in extra:
                (paths.REPOSITORY_ROOT / name).unlink()
            if extra:
                remove_empty_directories(paths.REPOSITORY_ROOT / REFERENCE_DIRECTORY)
            return Step("references", Status.PASSED,
                        f"updated {len(stale)} and removed {len(extra)} of {len(generated)} generated references",
                        time.perf_counter() - started, data=data)
    except (OSError, ToolNotFoundError) as error:
        return Step("references", Status.FAILED, str(error), time.perf_counter() - started)


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="Exit codes: 0 references generated or verified, 1 generation or comparison failed, "
               "2 usage error, 3 editor missing, 5 editor timed out.",
    )
    parser.add_argument("--config", choices=("Debug", "Release"), default="Debug")
    parser.add_argument("--check", action="store_true",
                        help="fail if any committed reference is stale, missing or extra; write nothing")
    parser.add_argument("--json", action="store_true", help="emit a machine-readable result")
    options = parser.parse_args(sys.argv[1:] if argv is None else argv)
    paths.enter_repository_root()
    console = Console(options.json)
    step = generate(options.config, options.check, console)
    console.result(step)
    code = overall_exit_code([step])
    if options.json:
        emit_json({"success": code == 0, "exitCode": code, "steps": [step.to_json()]})
    return code


if __name__ == "__main__":
    sys.exit(main())
