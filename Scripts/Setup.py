#!/usr/bin/env python3
"""Check the development toolchain and install the pinned tools (Docs/Architecture.md §2.3).

Checks: Python, Git, the Vulkan SDK (VULKAN_SDK at least the version pinned in Scripts/Lib/Toolchain.json, slangc
exactly at its pinned version, spirv-val), the Python interpreter the Shaders build rule invokes, and per host:
  Windows  Visual Studio 2026 with the x64 C++ tools (vswhere), the Windows SDK, the MSVC v145 CRT redistributables
  Linux    GCC 14+ (or Clang 18+ for --toolset clang), make, the X11 development headers GLFW needs (xorg-dev)
  macOS    Xcode 26+
clang-format 22.x is reported as a warning when missing (only Scripts/Format.py needs it).

Installs:
  premake  the pinned premake 5.0.0 release for the host into Vendor/premake/bin/ when it is absent, verified with
           SHA-256 (archive and executable). An existing binary must match the pinned digest.
  MCP      once Tools/MCP/requirements.lock exists (milestone M4): Tools/MCP/.venv from the hash-locked requirements
           and .mcp.json with the venv interpreter's absolute path (§13.8). Until then the step reports "skipped".

Exit codes: 0 every required check passed, 2 usage error, 3 a required check or installation failed (a tool or file
is missing, has the wrong version or could not be installed; Architecture §4.1 "initialization failed").
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"Setup.py requires Python 3.10 or newer (this is Python {platform.python_version()})")

import argparse
import hashlib
import json
import os
import re
import shutil
import time
import types
from pathlib import Path
from typing import Callable

from Lib import paths, premake, toolchain
from Lib.process import ToolNotFoundError, child_environment, run_captured, run_streamed
from Lib.report import (
    EXIT_INIT_FAILED,
    EXIT_SUCCESS,
    EXIT_USAGE,
    Console,
    Status,
    Step,
    configure_stdio,
    emit_json,
)

MINIMUM_PYTHON = (3, 10)

MCP_ROOT = paths.REPOSITORY_ROOT / "Tools" / "MCP"
MCP_LOCK = MCP_ROOT / "requirements.lock"
MCP_RUNNER = MCP_ROOT / "run.py"
MCP_VENV = MCP_ROOT / ".venv"
MCP_VENV_MARKER = MCP_VENV / ".requirements.sha256"
MCP_CONFIG = paths.REPOSITORY_ROOT / ".mcp.json"
MCP_SERVER_NAME = "engine"
PIP_TIMEOUT_SECONDS = 1200.0

# Headers the vendored GLFW (X11 backend) compiles against, with the Ubuntu package providing each
# (Vendor/GLFW/VENDOR.md: "sudo apt install xorg-dev", minimal set listed there).
LINUX_X11_HEADERS = (
    ("X11/Xlib.h", "libx11-dev"),
    ("X11/XKBlib.h", "libx11-dev"),
    ("X11/extensions/Xrandr.h", "libxrandr-dev"),
    ("X11/extensions/Xinerama.h", "libxinerama-dev"),
    ("X11/Xcursor/Xcursor.h", "libxcursor-dev"),
    ("X11/extensions/XInput2.h", "libxi-dev"),
    ("X11/extensions/Xext.h", "libxext-dev"),
)
LINUX_INCLUDE_ROOTS = (Path("/usr/include"), Path("/usr/local/include"), Path("/usr/include/x86_64-linux-gnu"))


class CheckFailed(Exception):
    """A check failed; the message is the step detail."""


class CheckWarning(Exception):
    """A check found something to fix that does not block building; the message is the step detail."""


def run_check(name: str, check: Callable[[], str], console: Console) -> Step:
    """Run one check: its return value is the detail of a passed step; exceptions become failed or warning steps."""
    started = time.monotonic()
    try:
        step = Step(name, Status.PASSED, check())
    except CheckWarning as warning:
        step = Step(name, Status.WARNING, str(warning))
    except (CheckFailed, toolchain.ToolchainError, premake.PremakeError, ToolNotFoundError, OSError) as error:
        step = Step(name, Status.FAILED, str(error))
    step.duration = time.monotonic() - started
    console.result(step)
    return step


# --------------------------------------------------------------------------------------------------------------------
# Common checks
# --------------------------------------------------------------------------------------------------------------------


def check_python() -> str:
    if sys.version_info < MINIMUM_PYTHON:
        raise CheckFailed(f"Python {platform.python_version()} is older than 3.10")
    return f"Python {platform.python_version()} ({sys.executable})"


def check_build_rule_python(host: paths.Host) -> str:
    """The Shaders project's custom build rule runs `python` on Windows, `python3` elsewhere (Engine/premake5.lua)."""
    name = "python" if host.is_windows else "python3"
    located = shutil.which(name)
    if not located:
        raise CheckFailed(f"'{name}' is not on PATH; the Shaders build rule runs '{name} Scripts/CompileShaders.py'")
    result = run_captured([located, "-c", "import sys; print('%d.%d.%d' % sys.version_info[:3])"], timeout=60)
    if not result.succeeded:
        # On Windows this is typically the Microsoft Store alias, which only opens the Store.
        raise CheckFailed(f"'{located}' does not run Python ({result.describe_exit()}); install Python 3.10+ and put "
                          f"it on PATH (disable the Store app execution alias)")
    version = (result.output.strip().splitlines() or [""])[-1]
    try:
        too_old = toolchain.parse_version(version)[:2] < MINIMUM_PYTHON
    except ValueError:
        raise CheckFailed(f"'{located}' reported an unexpected version: {version!r}") from None
    if too_old:
        raise CheckFailed(f"'{name}' on PATH is Python {version}; the build rule needs Python 3.10+")
    return f"'{name}' on PATH is Python {version} ({located})"


def check_git() -> str:
    try:
        result = run_captured(["git", "--version"], timeout=60)
    except ToolNotFoundError:
        raise CheckFailed("git is not on PATH") from None
    if not result.succeeded:
        raise CheckFailed(f"'git --version' failed: {result.tail(3)}")
    return result.output.strip()


def _import_compile_shaders() -> types.ModuleType:
    # CompileShaders.py is the single owner of how slangc and spirv-val are located and versioned; reusing it keeps
    # Setup's verdict identical to what the shader build does.
    import CompileShaders

    return CompileShaders


def check_vulkan_sdk(pins: toolchain.Pins) -> str:
    sdk = os.environ.get("VULKAN_SDK")
    if not sdk:
        raise CheckFailed(f"VULKAN_SDK is not set: install Vulkan SDK {pins.vulkan_sdk_version} from "
                          f"https://vulkan.lunarg.com/sdk/home and set VULKAN_SDK to it")
    root = Path(sdk)
    if not root.is_dir():
        raise CheckFailed(f"VULKAN_SDK={sdk} does not exist")
    header_version = toolchain.vulkan_sdk_header_version(root)
    required = toolchain.parse_version(pins.vulkan_sdk_version)[:3]
    if header_version < required:
        raise CheckFailed(f"Vulkan SDK at {sdk} has Vulkan headers {toolchain.version_text(header_version)}; "
                          f"{toolchain.version_text(required)} or newer is required (pin: {pins.vulkan_sdk_version})")
    full = next((part for part in reversed(root.parts) if re.fullmatch(r"\d+\.\d+\.\d+\.\d+", part)), None)
    shown = full or toolchain.version_text(header_version)
    return f"Vulkan SDK {shown} at {root.as_posix()} (required: {pins.vulkan_sdk_version} or newer)"


def check_slangc(pins: toolchain.Pins) -> str:
    compile_shaders = _import_compile_shaders()
    try:
        slangc = compile_shaders.find_tool("slangc", None)
        version = compile_shaders.query_slangc_version(slangc)
    except (compile_shaders.ToolMissing, compile_shaders.ToolFailure) as error:
        raise CheckFailed(str(error)) from None
    if version != pins.slangc_version:
        raise CheckFailed(f"slangc {version} ({slangc}) does not match the pinned {pins.slangc_version} "
                          f"(Scripts/Lib/Toolchain.json); install Vulkan SDK {pins.vulkan_sdk_version}")
    return f"slangc {version} ({Path(slangc).as_posix()})"


def check_spirv_val() -> str:
    compile_shaders = _import_compile_shaders()
    try:
        spirv_val = compile_shaders.find_tool("spirv-val", None)
    except compile_shaders.ToolMissing as error:
        raise CheckFailed(str(error)) from None
    result = run_captured([str(spirv_val), "--version"], timeout=60)
    if not result.succeeded:
        raise CheckFailed(f"'{spirv_val} --version' failed ({result.describe_exit()}): {result.tail(3)}")
    return f"{result.output.strip().splitlines()[0]} ({Path(spirv_val).as_posix()})"


def check_clang_format() -> str:
    try:
        executable, version = toolchain.find_clang_format()
    except toolchain.ToolchainError as error:
        raise CheckWarning(f"{error} (needed by Scripts/Format.py and the commit gate)") from None
    return f"clang-format {toolchain.version_text(version)} ({executable.as_posix()})"


# --------------------------------------------------------------------------------------------------------------------
# Host toolchains
# --------------------------------------------------------------------------------------------------------------------


def windows_checks(console: Console) -> tuple[list[Step], toolchain.VisualStudio | None]:
    found: dict[str, toolchain.VisualStudio] = {}

    def visual_studio() -> str:
        instance = toolchain.find_visual_studio()
        found["instance"] = instance
        return (f"{instance.display_name} {instance.version}, MSVC {instance.vc_tools_version}, "
                f"MSBuild {instance.msbuild.as_posix()}")

    def windows_sdk() -> str:
        version, directory = toolchain.find_windows_sdk()
        return f"Windows SDK {version} ({directory.as_posix()})"

    def crt_redist() -> str:
        instance = found.get("instance")
        if instance is None:
            raise CheckFailed("needs Visual Studio 2026 (see above)")
        version, directory = toolchain.find_crt_redist(instance)
        return f"app-local CRT {version} ({directory.as_posix()})"

    steps = [
        run_check("visual-studio", visual_studio, console),
        run_check("windows-sdk", windows_sdk, console),
        run_check("crt-redistributables", crt_redist, console),
    ]
    return steps, found.get("instance")


def linux_checks(console: Console) -> list[Step]:
    def compilers() -> str:
        explicit = os.environ.get("CXX")
        if explicit:
            family, _ = toolchain.identify_compiler(explicit)
            selected = toolchain.select_compiler("gcc" if family == "gcc" else "clang", os.environ)
            return f"{selected.describe()} from CXX (generate with --toolset {selected.toolset})"
        gcc = toolchain.select_compiler("gcc", os.environ)
        try:
            clang = toolchain.select_compiler("clang", os.environ).describe()
        except toolchain.ToolchainError as error:
            clang = f"Clang toolset unavailable, only needed for --toolset clang: {error}"
        return f"{gcc.describe()}; {clang}"

    def make() -> str:
        try:
            result = run_captured(["make", "--version"], timeout=60)
        except ToolNotFoundError:
            raise CheckFailed("GNU make is not installed (sudo apt install make)") from None
        return result.output.strip().splitlines()[0]

    def ninja() -> str:
        try:
            result = run_captured(["ninja", "--version"], timeout=60)
        except ToolNotFoundError:
            raise CheckWarning("ninja is not installed (only needed for Generate.py --action ninja)") from None
        return f"ninja {result.output.strip()}"

    def x11_headers() -> str:
        missing = [
            (header, package)
            for header, package in LINUX_X11_HEADERS
            if not any((root / header).is_file() for root in LINUX_INCLUDE_ROOTS)
        ]
        if missing:
            packages = " ".join(sorted({package for _, package in missing}))
            headers = ", ".join(header for header, _ in missing)
            raise CheckFailed(f"missing X11 development headers ({headers}): sudo apt install xorg-dev (or {packages})")
        return f"{len(LINUX_X11_HEADERS)} X11 headers found (GLFW X11 backend)"

    return [
        run_check("compilers", compilers, console),
        run_check("make", make, console),
        run_check("ninja", ninja, console),
        run_check("x11-headers", x11_headers, console),
    ]


def macos_checks(console: Console) -> list[Step]:
    def xcode() -> str:
        version = toolchain.xcode_version()
        if version[0] < toolchain.MINIMUM_XCODE:
            raise CheckFailed(f"Xcode {toolchain.version_text(version)} is older than {toolchain.MINIMUM_XCODE} "
                              f"(Architecture §16); install it and select it with xcode-select")
        return f"Xcode {toolchain.version_text(version)}"

    return [run_check("xcode", xcode, console)]


# --------------------------------------------------------------------------------------------------------------------
# Installation: premake, MCP virtual environment
# --------------------------------------------------------------------------------------------------------------------


def setup_premake(host: paths.Host, install: bool, reinstall: bool, console: Console) -> str:
    asset = premake.pinned_asset(host)
    executable = premake.executable_path()
    digest = premake.installed_digest()
    if digest is not None and digest != asset.executable_sha256 and not reinstall:
        raise CheckFailed(
            f"{paths.display_path(executable)} is not the pinned premake {premake.PREMAKE_VERSION} release binary "
            f"(sha256 {digest}, expected {asset.executable_sha256}); run Setup.py --reinstall-premake"
        )
    action = "verified"
    if digest is None or reinstall:
        if not install:
            raise CheckFailed(f"{paths.display_path(executable)} is missing (--no-install given)")
        premake.install(host, lambda message: console.print(f"  premake: {message}"))
        action = "installed"
    version = premake.version(executable)
    if version != premake.PREMAKE_VERSION:
        raise CheckFailed(f"{paths.display_path(executable)} reports version {version}, expected "
                          f"{premake.PREMAKE_VERSION}")
    return f"premake {version} {action} ({paths.display_path(executable)}, sha256 {asset.executable_sha256[:16]}...)"


def venv_python() -> Path:
    return MCP_VENV / ("Scripts/python.exe" if os.name == "nt" else "bin/python")


def mcp_configuration_text() -> str:
    configuration = {
        "mcpServers": {
            MCP_SERVER_NAME: {
                "type": "stdio",
                "command": venv_python().resolve().as_posix(),
                "args": [MCP_RUNNER.resolve().as_posix()],
            }
        }
    }
    return json.dumps(configuration, indent="\t") + "\n"


def setup_mcp(install: bool, console: Console) -> Step:
    started = time.monotonic()
    if not MCP_LOCK.is_file():
        step = Step(
            "mcp-venv",
            Status.SKIPPED,
            f"Tools/MCP not present yet: {paths.display_path(MCP_LOCK)} does not exist (the MCP bridge lands in M4)",
        )
        console.result(step)
        return step

    def check() -> str:
        if not MCP_RUNNER.is_file():
            raise CheckFailed(f"{paths.display_path(MCP_LOCK)} exists but {paths.display_path(MCP_RUNNER)} is missing")
        lock_digest = hashlib.sha256(MCP_LOCK.read_bytes()).hexdigest()
        marker = f"{lock_digest} python-{platform.python_version()}\n"
        interpreter = venv_python()
        current = interpreter.is_file() and MCP_VENV_MARKER.is_file() and MCP_VENV_MARKER.read_text() == marker
        configuration = mcp_configuration_text()
        configured = MCP_CONFIG.is_file() and MCP_CONFIG.read_text(encoding="utf-8") == configuration
        if current and configured:
            return f"{paths.display_path(MCP_VENV)} up to date; {paths.display_path(MCP_CONFIG)} current"
        if not install:
            raise CheckFailed(f"{paths.display_path(MCP_VENV)} or {paths.display_path(MCP_CONFIG)} is missing or out "
                              f"of date (--no-install given)")
        if not current:
            if not interpreter.is_file():
                console.print(f"  mcp: creating {paths.display_path(MCP_VENV)}")
                created = run_streamed([sys.executable, "-m", "venv", str(MCP_VENV)], echo=console.stream,
                                       timeout=600, env=child_environment())
                if not created.succeeded or not interpreter.is_file():
                    raise CheckFailed(f"creating the virtual environment failed ({created.describe_exit()})")
            console.print(f"  mcp: installing {paths.display_path(MCP_LOCK)} (hash-checked)")
            installed = run_streamed(
                [str(interpreter), "-m", "pip", "install", "--disable-pip-version-check", "--no-input",
                 "--require-hashes", "--requirement", str(MCP_LOCK)],
                echo=console.stream,
                timeout=PIP_TIMEOUT_SECONDS,
                env=child_environment(),
            )
            if not installed.succeeded:
                raise CheckFailed(f"pip install --require-hashes failed ({installed.describe_exit()})")
            MCP_VENV_MARKER.write_text(marker, encoding="utf-8")
        temporary = MCP_CONFIG.with_name(MCP_CONFIG.name + ".tmp")
        temporary.write_text(configuration, encoding="utf-8", newline="\n")
        os.replace(temporary, MCP_CONFIG)
        return f"{paths.display_path(MCP_VENV)} ready; wrote {paths.display_path(MCP_CONFIG)} ({MCP_SERVER_NAME})"

    step = run_check("mcp-venv", check, console)
    step.duration = time.monotonic() - started
    return step


# --------------------------------------------------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------------------------------------------------


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check the toolchain (Python, Git, Vulkan SDK, compilers) and install the pinned premake.",
        epilog="Exit codes: 0 every required check passed, 2 usage error, 3 a required check or installation failed.",
    )
    parser.add_argument("--no-install", action="store_true",
                        help="only check: never download premake or create the MCP virtual environment")
    parser.add_argument("--reinstall-premake", action="store_true",
                        help="download and verify premake again even if a binary is present")
    parser.add_argument("--json", action="store_true", help="print a machine-readable result on stdout")
    arguments = parser.parse_args(argv)
    if arguments.no_install and arguments.reinstall_premake:
        parser.error("--no-install and --reinstall-premake are mutually exclusive")
    return arguments


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    console = Console(arguments.json)
    try:
        host = paths.host()
    except paths.UnsupportedHostError as error:
        console.print(f"Setup.py: error: {error}")
        if arguments.json:
            emit_json({"success": False, "error": str(error)})
        return EXIT_USAGE

    console.heading(f"Setup ({host.system}, {host.machine})")
    steps: list[Step] = [
        run_check("python", check_python, console),
        run_check("git", check_git, console),
    ]
    pins: dict[str, toolchain.Pins] = {}

    def load_pins() -> str:
        pins["value"] = toolchain.load_pins()
        return (f"Vulkan SDK {pins['value'].vulkan_sdk_version}, slangc {pins['value'].slangc_version} "
                f"({paths.display_path(paths.TOOLCHAIN_PIN_PATH)})")

    steps.append(run_check("toolchain-pins", load_pins, console))
    if "value" in pins:
        steps.append(run_check("vulkan-sdk", lambda: check_vulkan_sdk(pins["value"]), console))
        steps.append(run_check("slangc", lambda: check_slangc(pins["value"]), console))
    steps.append(run_check("spirv-val", check_spirv_val, console))
    steps.append(run_check("build-rule-python", lambda: check_build_rule_python(host), console))

    if host.is_windows:
        windows_steps, _ = windows_checks(console)
        steps += windows_steps
    elif host.is_linux:
        steps += linux_checks(console)
    else:
        steps += macos_checks(console)
    steps.append(run_check("clang-format", check_clang_format, console))

    steps.append(run_check(
        "premake",
        lambda: setup_premake(host, not arguments.no_install, arguments.reinstall_premake, console),
        console,
    ))
    steps.append(setup_mcp(not arguments.no_install, console))

    # Every check printed its own result line above; end with the verdict.
    failed = [step for step in steps if step.failed]
    warnings = [step for step in steps if step.status == Status.WARNING]
    if failed:
        console.print(f"\nSetup failed: {', '.join(step.name for step in failed)}")
    else:
        suffix = f" ({len(warnings)} warning(s): {', '.join(step.name for step in warnings)})" if warnings else ""
        console.print(f"\nSetup complete{suffix}.")
    if arguments.json:
        emit_json({
            "success": not failed,
            "host": {"system": host.system, "architecture": host.architecture, "machine": host.machine},
            "steps": [step.to_json() for step in steps],
        })
    return EXIT_INIT_FAILED if failed else EXIT_SUCCESS


if __name__ == "__main__":
    sys.exit(main())
