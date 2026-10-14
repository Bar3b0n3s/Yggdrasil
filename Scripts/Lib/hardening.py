"""The hardening stage's AddressSanitizer build, long fuzz and soak (Docs/Architecture.md §15.1 T6, §15.8;
Docs/Decisions/0024-m15-contract.md decisions 12 to 15). Stream C implements this module and Scripts/Hardening.py, which
CI.py's hardening stage runs at milestone gates (CI.py --milestone).

AddressSanitizer: premake --sanitize=address instruments the Release configuration of every project; the workspace is
generated into ASAN_WORKSPACE (Generate.py --sanitize address --to bin-int/Asan) and built there, so its binaries live in
bin-int/Asan/bin/Release-<system>-<arch>/ and never replace the plain Release build. The MSVC runtime
clang_rt.asan_dynamic-x86_64.dll is copied next to Tests, Editor and Runtime there (never into Runtime/Redist, which
exports copy), and every process the sanitized suites start gets asan_environment(): ASAN_OPTIONS, and the runtime's
directory first on PATH, so the instrumented Runtime copies that tests export into temporary packages (the resource and
user-data acceptance tests) load it too.

Fuzz: Tests/Data/Fuzz/Targets.json lists every reader of external input with its fuzz case (§15.2), each at
FUZZ_HARDENING_ITERATIONS, and the stage runs the sanitized Tests binary with --test-suite=Fuzz
--fuzz-iterations=1000000 --fuzz-shard=<k>/<n> in fuzz_process_count() processes. Each case logs one
FUZZ_REPORT_PATTERN line per process (Tests/Source/Support/Fuzz.h, ReportFuzzRun) through the console sink, which writes
to the process's standard error at every level; the stage reads those lines from each process's captured standard error,
never from Tests.log, whose rotation could drop them. It fails when a target's shards do not cover [0, its iterations)
exactly once, or when any process fails.

Soak: every scene of every project soak_projects() names (Projects/FeatureTest, the golden scenes included, and each demo
project once it exists) plays SOAK_FRAMES lockstep ticks with rendering in the plain Release editor, sampled every
SOAK_SAMPLE_INTERVAL ticks (Lib/process.py query_process_memory and stats.get); and the exported Release FeatureTest
(bin/Exports/Release/FeatureTest, which the export stage leaves) runs its start scene for SOAK_FRAMES frames headless, its
memory sampled from outside every SOAK_RUNTIME_SAMPLE_SECONDS, so the Runtime-only paths (the pak mounts, the Runtime's
asset manager and frame loop) are soaked too. evaluate_soak applies the rules of decision 15 to each.
"""

from __future__ import annotations

import dataclasses
import re
from collections.abc import Mapping
from pathlib import Path

from Lib import paths

ASAN_WORKSPACE = paths.BIN_INT_ROOT / "Asan"
ASAN_RUNTIME_FILES = ("clang_rt.asan_dynamic-x86_64.dll", "clang_rt.asan_dynamic-x86_64.pdb")
# The projects whose output directories receive the runtime.
ASAN_RUNTIME_PROJECTS = ("Tests", "Editor", "Runtime")
FUZZ_TARGETS_FILE = paths.REPOSITORY_ROOT / "Tests" / "Data" / "Fuzz" / "Targets.json"
FUZZ_HARDENING_ITERATIONS = 1_000_000
FUZZ_MAX_PROCESSES = 32
# ReportFuzzRun's line (Tests/Source/Support/Fuzz.h). Fuzz case names contain no apostrophe.
FUZZ_REPORT_PATTERN = re.compile(
    r"Fuzz '(?P<case>[^']+)': iterations (?P<begin>\d+)\.\.(?P<end>\d+) of (?P<total>\d+) "
    r"\(seed 0x(?P<seed>[0-9a-f]{16})\): (?P<accepted>\d+) accepted, (?P<rejected>\d+) rejected"
)
SOAK_FRAMES = 10_000
SOAK_SAMPLE_INTERVAL = 250  # ticks between the samples of an editor scene soak
SOAK_RUNTIME_SAMPLE_SECONDS = 0.5  # seconds between the samples of the Runtime soak
# The rule's windows, as fractions of the soak (position = tick / SOAK_FRAMES in the editor, elapsed / total run time in
# the Runtime): the first half absorbs warm-up, buffers that fill and the Luau GC reaching its steady state.
SOAK_BASELINE_WINDOW = (0.50, 0.55)  # inclusive
SOAK_FINAL_WINDOW = (0.95, 1.00)  # inclusive
SOAK_GROWTH_LIMIT = 0.01  # private-memory growth must stay below 1 %


@dataclasses.dataclass(frozen=True)
class FuzzTarget:
    """One entry of Targets.json: {"Reader", "Case", "Iterations"}."""

    reader: str  # the reader of external input, as the module docstring of its header names it ("PakReader")
    case: str  # the doctest case name of its fuzz case (in the Fuzz suite)
    iterations: int  # what the hardening stage runs: FUZZ_HARDENING_ITERATIONS for every target, without exception


@dataclasses.dataclass(frozen=True)
class SoakSample:
    """One soak sample. The stats.get fields are None for the Runtime soak, which has no automation."""

    position: float  # how far into the soak, 0.0 to 1.0
    private_bytes: int
    resident_bytes: int
    entities: int | None = None
    bodies: int | None = None
    voices: int | None = None
    script_heap_bytes: int | None = None
    memory_allocation_count: int | None = None


@dataclasses.dataclass(frozen=True)
class SoakResult:
    subject: str  # "<project>:<scene>" for an editor soak, "Runtime:<project>" for the Runtime soak
    growth: float  # (final - baseline) / baseline of private bytes; 0.0 when it could not be computed
    passed: bool
    failures: tuple[str, ...]  # one line per broken rule


def asan_options() -> str:
    """The ASAN_OPTIONS every sanitized process gets: colon-separated, always with halt_on_error=1,
    allocator_may_return_null=0 and quarantine_size_mb=<n> (bounded so FUZZ_MAX_PROCESSES processes fit in memory), plus
    the handler settings the crash-handler tests need; n and the handler settings are decided by stream C's first spike
    and recorded in ADR 0024's integration section."""
    raise NotImplementedError("contract stub: hardening (ADR 0024 decision 12, stream C)")


def asan_runtime_directory() -> Path:
    """The MSVC tools' bin/Hostx64/x64 directory holding ASAN_RUNTIME_FILES (Lib/toolchain.py's Visual Studio).
    Raises FileNotFoundError naming Visual Studio's "C++ AddressSanitizer" component when the runtime is missing."""
    raise NotImplementedError("contract stub: hardening (ADR 0024 decision 12, stream C)")


def asan_environment(base: Mapping[str, str]) -> dict[str, str]:
    """A copy of `base` (an environment) for a process of the sanitized suites: ASAN_OPTIONS set to asan_options(), and
    asan_runtime_directory() put first on PATH (on Windows the variable's existing spelling, Path or PATH, is kept), so
    every child the process starts, an exported Runtime copy included, finds the runtime. Raises FileNotFoundError as
    asan_runtime_directory."""
    raise NotImplementedError("contract stub: hardening (ADR 0024 decision 12, stream C)")


def stage_asan_runtime(workspace: Path, config: str) -> list[Path]:
    """Copies ASAN_RUNTIME_FILES next to each of ASAN_RUNTIME_PROJECTS in <workspace>/bin/<OutputDir>/ (unchanged files
    are kept) and returns the copied paths. Raises FileNotFoundError as asan_runtime_directory, or when a project's
    output directory does not exist."""
    raise NotImplementedError("contract stub: hardening (ADR 0024 decision 12, stream C)")


def read_fuzz_targets(path: Path = FUZZ_TARGETS_FILE) -> list[FuzzTarget]:
    """Targets.json ({"Format": "FuzzTargets", "Version": 1, "Targets": [...]}, sorted by Reader, unique Reader and
    Case). Raises ValueError naming the file and the problem, including a target whose Iterations is not
    FUZZ_HARDENING_ITERATIONS or whose Case holds an apostrophe."""
    raise NotImplementedError("contract stub: hardening (ADR 0024 decision 13, stream C)")


def fuzz_process_count(logical_cpus: int, available_bytes: int, peak_bytes_per_process: int) -> int:
    """How many sanitized fuzz processes run at once: min(logical_cpus, FUZZ_MAX_PROCESSES,
    available_bytes // peak_bytes_per_process), at least 1. The stage passes the machine's logical CPUs, its available
    physical memory and the peak per process stream C measured (recorded in ADR 0024's integration section). Raises
    ValueError for a count or size below 1."""
    raise NotImplementedError("contract stub: hardening (ADR 0024 decision 13, stream C)")


def evaluate_fuzz_logs(targets: list[FuzzTarget], lines: list[str]) -> list[str]:
    """The failures of a fuzz run whose processes wrote `lines` to their standard error: "fuzz: <reader>: did not run"
    for a target without a report, "fuzz: <reader>: ran <n> of <iterations> iterations" when its shards' ranges do not
    cover [0, iterations) exactly once (a shard reported twice counts twice), in the order of `targets`; then
    "fuzz: <case>: not a target" for each reported case no target lists, sorted."""
    raise NotImplementedError("contract stub: hardening (ADR 0024 decision 13, stream C)")


def soak_projects(repository_root: Path) -> list[Path]:
    """The projects the soak covers: every directory of <repository_root>/Projects/ that holds a .eproj file (FeatureTest
    from M15, each demo from its milestone, §15.1 T6), sorted by name."""
    raise NotImplementedError("contract stub: hardening (ADR 0024 decision 15, stream C)")


def soak_scenes(project: Path) -> list[str]:
    """The project-relative paths of every .scene below <project>/Assets/Scenes/, the golden scenes included, sorted."""
    raise NotImplementedError("contract stub: hardening (ADR 0024 decision 15, stream C)")


def evaluate_soak(subject: str, samples: list[SoakSample]) -> SoakResult:
    """The soak rules of decision 15 over `samples`:
      - private memory: the baseline is the median private_bytes of the samples in SOAK_BASELINE_WINDOW, the final value
        the maximum in SOAK_FINAL_WINDOW, and growth must stay below SOAK_GROWTH_LIMIT;
      - script heap (a garbage-collected sawtooth): the final window's minimum must not exceed the baseline window's
        maximum;
      - entities, bodies, voices and the device's memory allocation count (small integers): the final window's maximum
        must not exceed the baseline window's maximum.
    A rule whose field is None in every sample is not applied (the Runtime soak). A window without a sample, or a baseline
    private_bytes of 0, is a failure naming it; no rule divides by zero."""
    raise NotImplementedError("contract stub: hardening (ADR 0024 decision 15, stream C)")
