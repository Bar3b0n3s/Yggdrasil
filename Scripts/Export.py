#!/usr/bin/env python3
"""Export a project as a standalone game through the editor's one-shot export (Docs/Architecture.md §2.3, §12.1, §14.2;
Docs/Decisions/0024-m15-contract.md decision 3).

Runs bin/<editor config>/Editor/Editor --headless --project P --export D --config C [--testing] [--smoke-test]
--export-report <temporary file> with --renderer (vulkan by default), and --user-data-dir and --engine-cache-dir when
given (tests pass both, so nothing is written to the profile or the checkout's bin/EngineCache), then prints the
outcome: the package directory and its files, or the error and its issues. The editor exports from the checkout's bin
directory, so the Runtime of the target configuration must be built (python Scripts/Build.py --config C --project
Runtime); a missing build output is the editor's exit code 3. With --json the report document is printed with an
"exitCode" member added: {"success": true, "result": <project.export's result>, "exitCode": 0} or {"success": false,
"error": {"code", "message", "issues", ...}, "exitCode": 1}.

Exit codes: the editor's (0 exported; 1 the export failed; 3 the project does not open, its lock is held, or a build
output or the editor is missing), 2 usage error, 5 timeout (the editor is stopped).
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"Export.py requires Python 3.10 or newer (this is Python {platform.python_version()})")

import argparse
import json
import tempfile
from pathlib import Path
from typing import Any

from Lib import paths
from Lib.process import ToolNotFoundError, child_environment, run_streamed
from Lib.report import (
    EXIT_FAILED,
    EXIT_INIT_FAILED,
    EXIT_SUCCESS,
    EXIT_TIMEOUT,
    EXIT_USAGE,
    Console,
    configure_stdio,
    emit_json,
)

DEFAULT_TIMEOUT_SECONDS = 1800.0
EXPORT_CONFIGURATIONS = ("Debug", "Release", "Dist")
EDITOR_CONFIGURATIONS = ("Debug", "Release")  # the editor has no Dist build (§2.2)
SHOWN_ISSUES = 20


def editor_executable(config: str) -> Path:
    """bin/<Config>-<system>-<arch>/Editor/Editor(.exe)."""
    return paths.output_directory(config) / "Editor" / paths.executable_name("Editor")


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Export a project as a standalone game (Editor --export).",
        epilog="Exit codes: 0 exported, 1 the export failed, 2 usage error, 3 the project does not open or a build "
               "output is missing, 5 timeout.",
    )
    parser.add_argument("--project", type=Path, required=True, help="the project (.eproj or its directory)")
    parser.add_argument("--config", required=True, choices=EXPORT_CONFIGURATIONS, help="the configuration to export")
    parser.add_argument("--out", type=Path, required=True,
                        help="the package directory: new, empty or holding an earlier export, which is replaced")
    parser.add_argument("--testing", action="store_true", help="a testing export (FeatureTest suites and replays)")
    parser.add_argument("--smoke-test", action="store_true", help="run the exported game headless before completing")
    parser.add_argument("--editor-config", default="Release", choices=EDITOR_CONFIGURATIONS,
                        help="the configuration of the editor that exports (default: Release)")
    parser.add_argument("--renderer", default="vulkan", choices=("vulkan", "none"),
                        help="the editor's renderer; none exports environments only from cooked bakes "
                             "(default: vulkan)")
    parser.add_argument("--user-data-dir", type=Path, help="the editor's user-data root (default: the platform's)")
    parser.add_argument("--engine-cache-dir", type=Path,
                        help="the engine cooked cache the editor bakes built-ins into (default: the checkout's "
                             "bin/EngineCache)")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS,
                        help=f"seconds before the editor is stopped (default: {DEFAULT_TIMEOUT_SECONDS:.0f})")
    parser.add_argument("--json", action="store_true", help="print the export report as JSON on stdout")
    arguments = parser.parse_args(argv)
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    return arguments


def editor_command(arguments: argparse.Namespace, editor: Path, report: Path) -> list[str]:
    command = [str(editor), "--headless", "--renderer", arguments.renderer,
               "--project", str(arguments.project.resolve()), "--export", str(arguments.out.resolve()),
               "--config", arguments.config, "--export-report", str(report)]
    if arguments.testing:
        command.append("--testing")
    if arguments.smoke_test:
        command.append("--smoke-test")
    if arguments.user_data_dir is not None:
        command.append(f"--user-data-dir={arguments.user_data_dir.resolve()}")
    if arguments.engine_cache_dir is not None:
        command.append(f"--engine-cache-dir={arguments.engine_cache_dir.resolve()}")
    return command


def describe_report(document: dict[str, Any], console: Console) -> None:
    """The outcome in a few lines: the package and its files, or the error and its issues."""
    if document.get("success"):
        result = document.get("result", {})
        files = result.get("files", [])
        console.print(f"Exported {result.get('config', '?')} to {result.get('outputDirectory', '?')} "
                      f"({len(files)} files)")
        console.print(f"Executable: {result.get('executable', '?')}")
        for warning in result.get("warnings", []):
            console.print(f"warning: {warning}")
        return
    error = document.get("error", {})
    console.print(f"Export failed ({error.get('errorCode', error.get('code', '?'))}): {error.get('message', '?')}")
    if error.get("hint"):
        console.print(f"hint: {error['hint']}")
    issues = error.get("issues", [])
    for issue in issues[:SHOWN_ISSUES]:
        hint = f" (hint: {issue['hint']})" if issue.get("hint") else ""
        console.print(f"  {issue.get('pointer') or '(root)'}: {issue.get('message', '')}{hint}")
    if len(issues) > SHOWN_ISSUES:
        console.print(f"  ... and {len(issues) - SHOWN_ISSUES} more")


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    console = Console(arguments.json)
    editor = editor_executable(arguments.editor_config)

    def finish(exit_code: int, document: dict[str, Any]) -> int:
        if arguments.json:
            emit_json({**document, "exitCode": exit_code})
        return exit_code

    if not editor.is_file():
        message = (f"the {arguments.editor_config} editor {paths.display_path(editor)} is not built: build it with "
                   f"python Scripts/Build.py --config {arguments.editor_config} --project Editor")
        console.print(f"Export failed: {message}")
        return finish(EXIT_INIT_FAILED, {"success": False, "error": {"message": message, "issues": []}})

    with tempfile.TemporaryDirectory(prefix="EngineExport-") as directory:
        report_path = Path(directory) / "Report.json"
        command = editor_command(arguments, editor, report_path)
        console.heading(f"Export {arguments.project} ({arguments.config})")
        try:
            result = run_streamed(command, env=child_environment(), timeout=arguments.timeout, echo=console.stream)
        except ToolNotFoundError as error:
            console.print(f"Export failed: {error}")
            return finish(EXIT_INIT_FAILED, {"success": False, "error": {"message": str(error), "issues": []}})
        if result.timed_out:
            message = f"the editor did not finish the export within {arguments.timeout:.0f} s and was stopped"
            console.print(f"Export failed: {message}")
            return finish(EXIT_TIMEOUT, {"success": False, "error": {"message": message, "issues": []}})
        exit_code = result.exit_code if result.exit_code is not None else EXIT_FAILED
        try:
            document = json.loads(report_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
            # A usage error or a crash leaves no report: the editor's output above says why.
            message = f"the editor exited with code {exit_code} without an export report ({error})"
            console.print(f"Export failed: {message}")
            if exit_code == EXIT_SUCCESS:
                exit_code = EXIT_FAILED
            return finish(exit_code, {"success": False, "error": {"message": message, "issues": []}})
    if not isinstance(document, dict):
        console.print("Export failed: the export report is not a JSON object")
        return finish(EXIT_FAILED, {"success": False, "error": {"message": "the export report is not a JSON object",
                                                                 "issues": []}})
    describe_report(document, console)
    if exit_code == EXIT_USAGE:
        console.print("The editor refused the command line (exit code 2).")
    return finish(exit_code, document)


if __name__ == "__main__":
    sys.exit(main())
