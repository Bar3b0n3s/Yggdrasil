#!/usr/bin/env python3
"""Generate registry-owned references, or compare them with committed files (§2.3, §11.9).

Runs the freshly built Editor without a window, renderer, audio device or project. Check mode never rewrites a
reference. Temporary user data, output and any crash reports remain inside one disposable directory.
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


REFERENCES = {
    "Methods.json": "Docs/Reference/Methods.json",
    "ScriptAPI.md": "Docs/Reference/ScriptAPI.md",
    "Engine.d.luau": "Resources/Scripting/Engine.d.luau",
    "catalog.json": "Tools/MCP/catalog.json",
}


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
            generated = {destination: (output / source).read_bytes() for source, destination in REFERENCES.items()}
            stale = [name for name, data in generated.items()
                     if not (paths.REPOSITORY_ROOT / name).is_file()
                     or (paths.REPOSITORY_ROOT / name).read_bytes() != data]
            if check and stale:
                return Step("references", Status.FAILED, "stale generated references: " + ", ".join(stale),
                            time.perf_counter() - started)
            if not check:
                for name in stale:
                    destination = paths.REPOSITORY_ROOT / name
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    staging = destination.with_name(destination.name + ".generating")
                    staging.write_bytes(generated[name])
                    staging.replace(destination)
            action = "verified" if check else f"updated {len(stale)} of"
            return Step("references", Status.PASSED, f"{action} {len(generated)} generated references",
                        time.perf_counter() - started)
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
    parser.add_argument("--check", action="store_true", help="fail if any committed reference differs; write nothing")
    parser.add_argument("--json", action="store_true", help="emit a machine-readable result")
    options = parser.parse_args(sys.argv[1:] if argv is None else argv)
    console = Console(options.json)
    step = generate(options.config, options.check, console)
    console.result(step)
    code = overall_exit_code([step])
    if options.json:
        emit_json({"success": code == 0, "exitCode": code, "steps": [step.to_json()]})
    return code


if __name__ == "__main__":
    sys.exit(main())
