#!/usr/bin/env python3
"""Compile the engine's Slang shaders to SPIR-V (Docs/Architecture.md §8.12).

This script is the only place where slangc flags are defined. It reads Resources/Shaders/Shaders.json, compiles
every entry point of every program in every permutation with slangc from the Vulkan SDK, validates each result with
spirv-val and writes, per variant, <Program>/<Entry>[.<KEY>-<VALUE>...].spv plus .refl.json and .d into the output
directory (bin/<OutputDir>/Shaders/ by default).

Compilation is incremental: a variant is rebuilt when its flags, the slangc version or any file listed in its depfile
changed. The slangc version must equal the one pinned in Scripts/Lib/Toolchain.json. After a successful run over all
programs the stamp file <output>/.stamp is written last; the Shaders premake project declares it as the output of its
custom build rule, so IDE builds re-run this script whenever a shader input changed.

Every slangc and spirv-val run has a timeout (TOOL_TIMEOUT_SECONDS), because this script runs inside IDE and make
builds where no other timeout applies; a run that exceeds it is terminated and fails its variant.

Exit codes: 0 success, 1 a shader failed to compile or validate, or a tool failed or timed out, 2 usage or
configuration error (bad arguments, invalid Shaders.json or Toolchain.json), 3 slangc or spirv-val is missing or
slangc does not have the pinned version.
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"CompileShaders.py requires Python 3.10 or newer (this is Python {platform.python_version()} at "
             f"{sys.executable}); regenerate with Scripts/Generate.py so build steps use its interpreter")

import argparse
import concurrent.futures
import dataclasses
import hashlib
import itertools
import json
import os
import re
import shutil
import subprocess
import time
from pathlib import Path
from typing import Any

from Lib import paths
from Lib.process import ProcessResult, ToolNotFoundError, run_captured
from Lib.report import EXIT_FAILED, EXIT_INIT_FAILED, EXIT_SUCCESS, EXIT_USAGE

REPOSITORY_ROOT = paths.REPOSITORY_ROOT
SHADER_ROOT_RELATIVE = "Resources/Shaders"
SHADER_ROOT = REPOSITORY_ROOT / SHADER_ROOT_RELATIVE
MANIFEST_PATH = SHADER_ROOT / "Shaders.json"
TOOLCHAIN_PATH = paths.TOOLCHAIN_PIN_PATH

CONFIGURATIONS = ("Debug", "Release", "Dist")
STAGES = ("vertex", "fragment", "compute")

STAMP_FILE_NAME = ".stamp"
CACHE_FILE_NAME = ".cache.json"
LOCK_FILE_NAME = ".lock"
CACHE_VERSION = 1
LOCK_TIMEOUT_SECONDS = 300.0
TOOL_TIMEOUT_SECONDS = 300.0  # per slangc or spirv-val run

# spirv-val runs on every output. The engine requires Vulkan 1.3 (which accepts SPIR-V 1.6), so every shader must
# be valid there; Vulkan 1.4 is used only through optional, feature-gated paths (§8.1).
VALIDATOR_FLAGS = ("--target-env", "vulkan1.3")

PROGRAM_NAME_PATTERN = re.compile(r"^[A-Za-z][A-Za-z0-9_]*$")
PERMUTATION_KEY_PATTERN = re.compile(r"^[A-Z][A-Z0-9_]*$")
PERMUTATION_VALUE_PATTERN = re.compile(r"^[A-Za-z0-9_]+$")


class UsageError(Exception):
    """Bad arguments, or an invalid Shaders.json or Toolchain.json (exit code 2)."""


class ToolMissing(Exception):
    """slangc or spirv-val is missing, or slangc is not the pinned version (exit code 3)."""


class ToolFailure(Exception):
    """A tool failed to run or timed out, or the output directory could not be locked (exit code 1)."""


@dataclasses.dataclass(frozen=True)
class Variant:
    """One slangc invocation: an entry point of a program in one permutation."""

    program: str
    source: str  # relative to the repository root, with forward slashes
    entry: str
    stage: str
    permutation: tuple[tuple[str, str], ...]  # sorted by key

    @property
    def stem(self) -> str:
        suffix = "".join(f".{key}-{value}" for key, value in self.permutation)
        return f"{self.program}/{self.entry}{suffix}"

    @property
    def description(self) -> str:
        if not self.permutation:
            return f"{self.program}/{self.entry}"
        values = ", ".join(f"{key}={value}" for key, value in self.permutation)
        return f"{self.program}/{self.entry} ({values})"


@dataclasses.dataclass
class CompileResult:
    variant: Variant
    succeeded: bool
    diagnostics: str = ""
    cache_entry: dict[str, Any] | None = None


# --------------------------------------------------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------------------------------------------------


def read_json(path: Path, description: str) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise UsageError(f"{description} not found: {path}") from None
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise UsageError(f"{description} is not valid JSON ({path}): {error}") from None


def load_pinned_slangc_version() -> tuple[str, str]:
    toolchain = read_json(TOOLCHAIN_PATH, "Toolchain pin file")
    try:
        slangc_version = toolchain["Slangc"]["Version"]
        sdk_version = toolchain["VulkanSdk"]["Version"]
    except (KeyError, TypeError):
        raise UsageError(f"{TOOLCHAIN_PATH} must contain Slangc.Version and VulkanSdk.Version") from None
    if not isinstance(slangc_version, str) or not isinstance(sdk_version, str):
        raise UsageError(f"{TOOLCHAIN_PATH}: Slangc.Version and VulkanSdk.Version must be strings")
    return slangc_version, sdk_version


def require_keys(value: Any, where: str, required: set[str], optional: set[str] | None = None) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise UsageError(f"{where}: expected an object")
    missing = sorted(required - value.keys())
    unknown = sorted(value.keys() - required - (optional or set()))
    if missing:
        raise UsageError(f"{where}: missing key(s) {', '.join(missing)}")
    if unknown:
        raise UsageError(f"{where}: unknown key(s) {', '.join(unknown)}")
    return value


def load_variants(manifest_path: Path) -> list[Variant]:
    """Read and validate Shaders.json and expand it into the sorted list of variants."""
    manifest = require_keys(read_json(manifest_path, "Shader manifest"), "Shaders.json", {"Programs"})
    programs = manifest["Programs"]
    if not isinstance(programs, list) or not programs:
        raise UsageError("Shaders.json: Programs must be a non-empty array")

    variants: list[Variant] = []
    seen_programs: set[str] = set()
    for index, program in enumerate(programs):
        where = f"Shaders.json: Programs[{index}]"
        require_keys(program, where, {"Name", "File", "Entries"}, {"Permutations"})

        name = program["Name"]
        if not isinstance(name, str) or not PROGRAM_NAME_PATTERN.match(name):
            raise UsageError(f"{where}: Name must match {PROGRAM_NAME_PATTERN.pattern}")
        if name in seen_programs:
            raise UsageError(f"{where}: duplicate program name '{name}'")
        seen_programs.add(name)
        where = f"Shaders.json: program '{name}'"

        file = program["File"]
        if not isinstance(file, str) or not file.endswith(".slang"):
            raise UsageError(f"{where}: File must be a .slang path relative to {SHADER_ROOT_RELATIVE}")
        relative = Path(file)
        if relative.is_absolute() or ".." in relative.parts or "\\" in file:
            raise UsageError(f"{where}: File must be a forward-slash path inside {SHADER_ROOT_RELATIVE}")
        if not (SHADER_ROOT / relative).is_file():
            raise UsageError(f"{where}: File '{file}' does not exist in {SHADER_ROOT_RELATIVE}")
        source = f"{SHADER_ROOT_RELATIVE}/{file}"

        entries = program["Entries"]
        if not isinstance(entries, list) or not entries:
            raise UsageError(f"{where}: Entries must be a non-empty array")
        parsed_entries: list[tuple[str, str]] = []
        for entry_index, entry in enumerate(entries):
            entry_where = f"{where}: Entries[{entry_index}]"
            require_keys(entry, entry_where, {"Name", "Stage"})
            entry_name, stage = entry["Name"], entry["Stage"]
            if not isinstance(entry_name, str) or not PROGRAM_NAME_PATTERN.match(entry_name):
                raise UsageError(f"{entry_where}: Name must match {PROGRAM_NAME_PATTERN.pattern}")
            if stage not in STAGES:
                raise UsageError(f"{entry_where}: Stage must be one of {', '.join(STAGES)}")
            if any(existing == entry_name for existing, _ in parsed_entries):
                raise UsageError(f"{entry_where}: duplicate entry point '{entry_name}'")
            parsed_entries.append((entry_name, stage))

        permutations = program.get("Permutations", {})
        if not isinstance(permutations, dict):
            raise UsageError(f"{where}: Permutations must be an object of KEY -> array of values")
        axes: list[list[tuple[str, str]]] = []
        for key in sorted(permutations):
            values = permutations[key]
            if not PERMUTATION_KEY_PATTERN.match(key):
                raise UsageError(f"{where}: permutation key '{key}' must match {PERMUTATION_KEY_PATTERN.pattern}")
            if (
                not isinstance(values, list)
                or not values
                or not all(isinstance(value, str) and PERMUTATION_VALUE_PATTERN.match(value) for value in values)
                or len(set(values)) != len(values)
            ):
                raise UsageError(
                    f"{where}: permutation '{key}' must be a non-empty array of unique strings matching "
                    f"{PERMUTATION_VALUE_PATTERN.pattern}"
                )
            axes.append([(key, value) for value in values])

        for entry_name, stage in parsed_entries:
            for combination in itertools.product(*axes):
                variants.append(Variant(name, source, entry_name, stage, tuple(combination)))

    return sorted(variants, key=lambda variant: variant.stem)


def default_output_directory(config: str) -> Path:
    """bin/<OutputDir>/Shaders, with premake's OutputDir naming for the host (Lib/paths.py).

    The Shaders project always passes --output-dir explicitly; this default serves manual runs on the host.
    """
    try:
        return paths.output_directory(config) / "Shaders"
    except paths.UnsupportedHostError as error:
        raise UsageError(f"{error}; pass --output-dir") from None


# --------------------------------------------------------------------------------------------------------------------
# Toolchain
# --------------------------------------------------------------------------------------------------------------------


def find_tool(name: str, override: str | None) -> Path:
    executable = f"{name}.exe" if os.name == "nt" else name
    if override:
        path = Path(override)
        if not path.is_file():
            raise ToolMissing(f"{name} not found at {path}")
        return path

    candidates: list[Path] = []
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        candidates += [Path(sdk) / "Bin" / executable, Path(sdk) / "bin" / executable]
    for candidate in candidates:
        if candidate.is_file():
            return candidate

    found = shutil.which(name)
    if found:
        return Path(found)
    raise ToolMissing(
        f"{name} not found: install the Vulkan SDK and set VULKAN_SDK (checked by Scripts/Setup.py), "
        f"or pass --{name}"
    )


def run_tool(command: list[str], cwd: Path) -> ProcessResult:
    """Run a tool with combined output and the TOOL_TIMEOUT_SECONDS limit. A timed-out run raises ToolFailure."""
    try:
        result = run_captured(command, cwd=cwd, timeout=TOOL_TIMEOUT_SECONDS)
    except ToolNotFoundError as error:
        raise ToolFailure(str(error)) from None
    if result.timed_out:
        raise ToolFailure(f"{Path(command[0]).name} {result.describe_exit()} (limit {TOOL_TIMEOUT_SECONDS:.0f} s) and "
                          f"was terminated")
    return result


def query_slangc_version(slangc: Path) -> str:
    result = run_tool([str(slangc), "-version"], REPOSITORY_ROOT)
    version = result.output.strip()
    if not result.succeeded or not version:
        raise ToolFailure(f"'{slangc} -version' failed ({result.describe_exit()}): {version}")
    return version.splitlines()[-1].strip()


# --------------------------------------------------------------------------------------------------------------------
# Compilation
# --------------------------------------------------------------------------------------------------------------------


def slangc_flags(variant: Variant, config: str) -> list[str]:
    """Every slangc flag except the source and output paths (§8.12). The only place these are defined."""
    flags = [
        "-target", "spirv",
        "-profile", "spirv_1_6",
        "-entry", variant.entry,
        "-stage", variant.stage,
        "-fvk-use-entrypoint-name",
        "-matrix-layout-column-major",
        # HLSL registers shifted to NVRHI's default VulkanBindingOffsets: SRV 0, sampler 128, CBV 256, UAV 384 (§8.4).
        "-fvk-t-shift", "0", "all",
        "-fvk-s-shift", "128", "all",
        "-fvk-b-shift", "256", "all",
        "-fvk-u-shift", "384", "all",
        "-I", SHADER_ROOT_RELATIVE,
        "-warnings-as-errors", "all",
        "-O2",
    ]
    if config in ("Debug", "Release"):
        flags.append("-g")
    flags += [f"-D{key}={value}" for key, value in variant.permutation]
    return flags


def variant_key(variant: Variant, config: str, slangc_version: str) -> str:
    """Hash of everything besides the depfile inputs that determines a variant's outputs."""
    description = {
        "CacheVersion": CACHE_VERSION,
        "Source": variant.source,
        "Flags": slangc_flags(variant, config),
        "ValidatorFlags": list(VALIDATOR_FLAGS),
        "SlangcVersion": slangc_version,
    }
    return hashlib.sha256(json.dumps(description, sort_keys=True).encode("utf-8")).hexdigest()


def output_paths(output_directory: Path, stem: str, temporary: bool = False) -> tuple[Path, Path, Path]:
    base = output_directory / (stem + (".tmp" if temporary else ""))
    return (
        base.with_name(base.name + ".spv"),
        base.with_name(base.name + ".refl.json"),
        base.with_name(base.name + ".d"),
    )


def parse_depfile(text: str) -> list[str]:
    """Return the prerequisites of a make-style depfile as written by slangc -depfile.

    Handles line continuations and the escapes slangc emits: "\\\\" (backslash), "\\:" (colon), "\\ " (space),
    "\\#" and "$$". The first unescaped colon of each rule separates the target from its prerequisites.
    """
    prerequisites: list[str] = []
    token: list[str] = []
    after_separator = False

    def flush() -> None:
        if token:
            if after_separator:
                prerequisites.append("".join(token))
            token.clear()

    index = 0
    while index < len(text):
        character = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""
        if character == "\\" and following == "\n":
            flush()
            index += 2
        elif character == "\\" and following == "\r" and text[index + 2 : index + 3] == "\n":
            flush()
            index += 3
        elif character == "\\" and following in (" ", "\\", ":", "#"):
            token.append(following)
            index += 2
        elif character == "$" and following == "$":
            token.append("$")
            index += 2
        elif character == ":" and not after_separator:
            token.clear()
            after_separator = True
            index += 1
        elif character in "\r\n":
            flush()
            after_separator = False
            index += 1
        elif character in " \t":
            flush()
            index += 1
        else:
            token.append(character)
            index += 1
    flush()
    return prerequisites


def portable_path(path: Path) -> str:
    """Repository-relative forward-slash path when inside the repository, else absolute."""
    resolved = path.resolve()
    try:
        return resolved.relative_to(REPOSITORY_ROOT).as_posix()
    except ValueError:
        return resolved.as_posix()


def resolve_portable_path(value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else REPOSITORY_ROOT / path


def file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def describe_dependency(path: Path) -> dict[str, Any]:
    status = path.stat()
    return {
        "Path": portable_path(path),
        "Size": status.st_size,
        "MTimeNs": status.st_mtime_ns,
        "Sha256": file_digest(path),
    }


def is_up_to_date(entry: dict[str, Any] | None, key: str, output_directory: Path, stem: str) -> bool:
    """True when the cached outputs of a variant are still valid. Refreshes timestamps of content-identical inputs."""
    if not isinstance(entry, dict) or entry.get("Key") != key:
        return False
    if not all(path.is_file() for path in output_paths(output_directory, stem)):
        return False
    dependencies = entry.get("Dependencies")
    if not isinstance(dependencies, list) or not dependencies:
        return False
    for dependency in dependencies:
        try:
            path = resolve_portable_path(dependency["Path"])
            status = path.stat()
            if status.st_size == dependency["Size"] and status.st_mtime_ns == dependency["MTimeNs"]:
                continue
            if status.st_size != dependency["Size"] or file_digest(path) != dependency["Sha256"]:
                return False
            dependency["MTimeNs"] = status.st_mtime_ns
        except (OSError, KeyError, TypeError):
            return False
    return True


def compile_variant(
    variant: Variant, config: str, key: str, slangc: Path, spirv_val: Path, output_directory: Path, verbose: bool
) -> CompileResult:
    spv, reflection, depfile = output_paths(output_directory, variant.stem, temporary=True)
    spv.parent.mkdir(parents=True, exist_ok=True)
    command = (
        [str(slangc), variant.source]
        + slangc_flags(variant, config)
        + ["-reflection-json", str(reflection), "-depfile", str(depfile), "-o", str(spv)]
    )
    log = [subprocess.list2cmdline(command)] if verbose else []

    try:
        compiled = run_tool(command, REPOSITORY_ROOT)
        log.append(compiled.output.rstrip())
        if not compiled.succeeded or not all(path.is_file() for path in (spv, reflection, depfile)):
            return CompileResult(variant, False, "\n".join(filter(None, log)))

        validation_command = [str(spirv_val), *VALIDATOR_FLAGS, str(spv)]
        if verbose:
            log.append(subprocess.list2cmdline(validation_command))
        validated = run_tool(validation_command, REPOSITORY_ROOT)
        log.append(validated.output.rstrip())
        if not validated.succeeded:
            log.append(f"spirv-val rejected {variant.description} ({validated.describe_exit()})")
            return CompileResult(variant, False, "\n".join(filter(None, log)))

        depfile_text = depfile.read_text(encoding="utf-8", errors="replace")
        dependencies = parse_depfile(depfile_text)
        if not dependencies:
            log.append(f"slangc wrote an empty depfile for {variant.description}")
            return CompileResult(variant, False, "\n".join(filter(None, log)))
        described = [describe_dependency(resolve_portable_path(dependency)) for dependency in sorted(set(dependencies))]

        # The depfile's target names the temporary .spv; point it at the final name before moving it into place.
        depfile.write_text(depfile_text.replace(".tmp.spv:", ".spv:", 1), encoding="utf-8", newline="\n")

        # Outputs replace the previous ones only once compiled and validated, so a failed compile never leaves a
        # partial or invalid .spv behind (the editor keeps loading the last good one).
        for temporary, final in zip((spv, reflection, depfile), output_paths(output_directory, variant.stem)):
            os.replace(temporary, final)
        return CompileResult(variant, True, "\n".join(filter(None, log)), {"Key": key, "Dependencies": described})
    except (OSError, ToolFailure) as error:
        log.append(str(error))
        return CompileResult(variant, False, "\n".join(filter(None, log)))
    finally:
        for path in (spv, reflection, depfile):
            path.unlink(missing_ok=True)


# --------------------------------------------------------------------------------------------------------------------
# Output directory: lock, cache, cleanup, stamp
# --------------------------------------------------------------------------------------------------------------------


class OutputLock:
    """Exclusive lock on an output directory, so a hot reload and an IDE build never interleave."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.stream: Any = None

    def __enter__(self) -> OutputLock:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.stream = self.path.open("a+b")
        deadline = time.monotonic() + LOCK_TIMEOUT_SECONDS
        while True:
            try:
                if os.name == "nt":
                    import msvcrt

                    self.stream.seek(0)
                    msvcrt.locking(self.stream.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl

                    fcntl.flock(self.stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                return self
            except OSError:
                if time.monotonic() >= deadline:
                    self.stream.close()
                    raise ToolFailure(f"timed out waiting for another CompileShaders.py run to release {self.path}")
                time.sleep(0.1)

    def __exit__(self, *exception: object) -> None:
        try:
            if os.name == "nt":
                import msvcrt

                self.stream.seek(0)
                msvcrt.locking(self.stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl

                fcntl.flock(self.stream.fileno(), fcntl.LOCK_UN)
        finally:
            self.stream.close()


def load_cache(output_directory: Path, config: str) -> dict[str, Any]:
    try:
        cache = json.loads((output_directory / CACHE_FILE_NAME).read_text(encoding="utf-8"))
        if cache.get("Version") == CACHE_VERSION and cache.get("Configuration") == config:
            outputs = cache.get("Outputs")
            if isinstance(outputs, dict):
                return outputs
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, AttributeError):
        pass
    return {}


def write_atomically(path: Path, text: str) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(text, encoding="utf-8", newline="\n")
    os.replace(temporary, path)


def save_cache(output_directory: Path, config: str, slangc_version: str, outputs: dict[str, Any]) -> None:
    cache = {
        "Version": CACHE_VERSION,
        "Configuration": config,
        "SlangcVersion": slangc_version,
        "Outputs": dict(sorted(outputs.items())),
    }
    write_atomically(output_directory / CACHE_FILE_NAME, json.dumps(cache, indent="\t") + "\n")


def remove_leftover_temporaries(output_directory: Path) -> None:
    for pattern in ("*.tmp.spv", "*.tmp.refl.json", "*.tmp.d"):
        for path in output_directory.rglob(pattern):
            path.unlink(missing_ok=True)


def remove_outputs(output_directory: Path, stem: str) -> None:
    for path in output_paths(output_directory, stem):
        path.unlink(missing_ok=True)
    directory = (output_directory / stem).parent
    if directory != output_directory and directory.is_dir() and not any(directory.iterdir()):
        directory.rmdir()


# --------------------------------------------------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------------------------------------------------


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compile Resources/Shaders (Shaders.json) to SPIR-V with slangc and validate it with spirv-val.",
        epilog="Exit codes: 0 success, 1 compile, validation or tool failure (timeouts included), 2 usage or "
        "configuration error, 3 slangc or spirv-val missing, or slangc not the pinned version.",
    )
    parser.add_argument(
        "--config", choices=CONFIGURATIONS, default="Debug", help="build configuration (default: Debug)"
    )
    parser.add_argument(
        "--output-dir", type=Path, help="output directory (default: bin/<Config>-<system>-<architecture>/Shaders)"
    )
    parser.add_argument(
        "--program",
        action="append",
        default=[],
        metavar="NAME",
        help="compile only this program (repeatable); used by shader hot reload. Does not write the stamp file",
    )
    parser.add_argument("--force", action="store_true", help="recompile every selected variant")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="parallel slangc processes")
    parser.add_argument("--slangc", help="path to slangc (default: $VULKAN_SDK, then PATH)")
    parser.add_argument("--spirv-val", dest="spirv_val", help="path to spirv-val (default: $VULKAN_SDK, then PATH)")
    parser.add_argument("--verbose", action="store_true", help="print every command line")
    parser.add_argument("--json", action="store_true", help="print a machine-readable summary on stdout")
    arguments = parser.parse_args(argv)
    if arguments.jobs < 1:
        parser.error("--jobs must be at least 1")
    return arguments


def run(arguments: argparse.Namespace) -> dict[str, Any]:
    config: str = arguments.config
    output_directory: Path = (arguments.output_dir or default_output_directory(config)).resolve()

    variants = load_variants(MANIFEST_PATH)
    known_programs = {variant.program for variant in variants}
    unknown = sorted(set(arguments.program) - known_programs)
    if unknown:
        raise UsageError(
            f"unknown program(s) {', '.join(unknown)}; Shaders.json defines {', '.join(sorted(known_programs))}"
        )
    selected = [variant for variant in variants if not arguments.program or variant.program in arguments.program]

    pinned_version, sdk_version = load_pinned_slangc_version()
    slangc = find_tool("slangc", arguments.slangc)
    spirv_val = find_tool("spirv-val", arguments.spirv_val)
    slangc_version = query_slangc_version(slangc)
    if slangc_version != pinned_version:
        raise ToolMissing(
            f"slangc {slangc_version} ({slangc}) does not match the pinned version {pinned_version} from "
            f"{TOOLCHAIN_PATH.relative_to(REPOSITORY_ROOT).as_posix()}. Install Vulkan SDK {sdk_version}, or update "
            f"the pin together with any shader changes the new compiler needs."
        )

    summary: dict[str, Any] = {
        "config": config,
        "outputDirectory": output_directory.as_posix(),
        "slangcVersion": slangc_version,
        "compiled": [],
        "upToDate": [],
        "failed": [],
        "removed": [],
        "stampWritten": False,
    }

    with OutputLock(output_directory / LOCK_FILE_NAME):
        remove_leftover_temporaries(output_directory)
        cache = load_cache(output_directory, config)

        pending: list[tuple[Variant, str]] = []
        for variant in selected:
            key = variant_key(variant, config, slangc_version)
            if not arguments.force and is_up_to_date(cache.get(variant.stem), key, output_directory, variant.stem):
                summary["upToDate"].append(variant.stem)
            else:
                pending.append((variant, key))

        with concurrent.futures.ThreadPoolExecutor(max_workers=arguments.jobs) as executor:
            futures = [
                executor.submit(
                    compile_variant, variant, config, key, slangc, spirv_val, output_directory, arguments.verbose
                )
                for variant, key in pending
            ]
            results = [future.result() for future in futures]

        for result in results:
            if result.succeeded:
                cache[result.variant.stem] = result.cache_entry
                summary["compiled"].append(result.variant.stem)
            else:
                summary["failed"].append(
                    {
                        "variant": result.variant.description,
                        "source": result.variant.source,
                        "diagnostics": result.diagnostics,
                    }
                )
            if result.diagnostics and not arguments.json:
                print(result.diagnostics, file=sys.stderr if not result.succeeded else sys.stdout)
            if not result.succeeded and not arguments.json:
                # Canonical "origin: error: text" line so MSBuild and IDEs list the failure.
                source = (REPOSITORY_ROOT / result.variant.source).as_posix()
                print(f"{source}: error: shader {result.variant.description} failed to compile", file=sys.stderr)

        if not arguments.program:
            current = {variant.stem for variant in variants}
            for stem in sorted(set(cache) - current):
                remove_outputs(output_directory, stem)
                del cache[stem]
                summary["removed"].append(stem)

        save_cache(output_directory, config, slangc_version, cache)

        # Written last, and only by complete successful runs: a partial (--program) run must not mark programs it
        # did not look at as up to date for the IDE.
        if not summary["failed"] and not arguments.program:
            stamp = {"Configuration": config, "SlangcVersion": slangc_version, "Variants": len(variants)}
            write_atomically(output_directory / STAMP_FILE_NAME, json.dumps(stamp, indent="\t") + "\n")
            summary["stampWritten"] = True

    return summary


def main(argv: list[str] | None = None) -> int:
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    try:
        summary = run(arguments)
    except UsageError as error:
        if arguments.json:
            print(json.dumps({"success": False, "error": str(error)}, indent=2))
        print(f"CompileShaders.py: error: {error}", file=sys.stderr)
        return EXIT_USAGE
    except ToolMissing as error:
        if arguments.json:
            print(json.dumps({"success": False, "error": str(error)}, indent=2))
        print(f"CompileShaders.py: error: {error}", file=sys.stderr)
        return EXIT_INIT_FAILED
    except ToolFailure as error:
        if arguments.json:
            print(json.dumps({"success": False, "error": str(error)}, indent=2))
        print(f"CompileShaders.py: error: {error}", file=sys.stderr)
        return EXIT_FAILED

    summary["success"] = not summary["failed"]
    if arguments.json:
        print(json.dumps(summary, indent=2))
    else:
        print(
            f"Shaders ({summary['config']}): {len(summary['compiled'])} compiled, {len(summary['upToDate'])} up to "
            f"date, {len(summary['failed'])} failed, {len(summary['removed'])} removed -> {summary['outputDirectory']}"
        )
    return EXIT_SUCCESS if summary["success"] else EXIT_FAILED


if __name__ == "__main__":
    sys.exit(main())
