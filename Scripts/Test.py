#!/usr/bin/env python3
"""Run the test suites (Docs/Architecture.md §2.3, §15).

Suites and the configurations the §15.8 matrix runs them in when --config is not given:
  unit         Debug, Release   the doctest executable bin/<OutputDir>/Tests/Tests, every test suite except GPU and
                                Golden (T0 static + T1 unit)
  gpu          Debug, Release   the GPU test cases (doctest suite GPU, T2), validation and synchronization validation
                                on, run twice per configuration: under the device's API version (1.4 where available)
                                and capped with --vulkan-api=1.3, so both paths of every Vulkan 1.4 capability run
                                (§8.1, §15.3); --vulkan-api restricts the run to one cap
  golden       Release          the golden images (doctest suite Golden, §15.4), compared with
                                Tests/Golden/<DeviceClass>/
  feature      Debug, Release   FeatureTest in editor mode (§15.5, §15.6): Editor --headless --project
                                Projects/FeatureTest --run-tests, then the six per-mode coverage gates of its report and
                                a byte check that the run left Projects/FeatureTest unchanged outside Library/
  automation   Release          the Python suites Tests/Automation and Tools/MCP/tests (§15.7) against the headless
                                editor bin/<OutputDir>/Editor/Editor, run by Scripts/Lib/unittest_runner.py with
                                Tools/MCP/.venv's interpreter (Scripts/Setup.py creates it; the bridge's tests need the
                                MCP SDK), then the method coverage gate (§15.6 gate 5, Roadmap M4)
  export       Release, Dist    FeatureTest's testing export in each configuration, run with --feature-test, its six
                                per-mode gates, and the cross-mode union merge over the Editor, Release and Dist reports
                                (§15.1 T3, §15.6; Docs/Decisions/0024-m15-contract.md decision 9)
  determinism  Debug, Release,  every Testing.Replays file of FeatureTest: verified with input.replay {verify,
               Dist             strictHash} in the --renderer none editor of Debug and Release, and read from the
                                exported Release and Dist runs' reports (§15.1 T3d; ADR 0024 decision 10): one step
                                over all requested configurations, which compares their hashes
  games        Debug, Release,  every staged demo game below Projects/ and Tests/Projects/ (§15.1 T5, §15.8; ADR 0026
               Dist             decisions 7 and 8): its provenance audit, the editor half of Debug and Release (checks
                                and test.run in a strict editor on its staged files) and the exported Dist half (a
                                testing Dist export run with --feature-test, its hashes compared with the editor
                                halves', and a plain Dist export's smoke test)
--config takes Debug and Release for the doctest, feature and automation suites (test executables and the editor do not
exist in Dist), Release and Dist for the export suite, and any of the three for the determinism and games suites. A
suite named in --suite must take every configuration --config names (a usage error otherwise); with the default --suite
all, each suite runs in the requested configurations it takes and reports each other one as a "skipped" step ("not run
in <Config>"), so `--config Debug` runs every suite that has a Debug run.
A suite that does not exist yet is reported with the distinct status "not-available"; it neither passes nor fails.

GPU and golden runs (Docs/Decisions/0009-m5-decisions.md decision 14): a test case without a usable Vulkan device passes
after logging that it found none, unless --require-gpu is given (CI.py and PreCommit.py pass it unless they get
--gpu-optional), which makes it fail. Such a run ends with the status "warning" and the number of test cases that found
no device. A gpu run under the 1.4 cap also ends with "warning" when host-image-copy cases were skipped (the device or a
format lacks the 1.4 path, §8.1); under the 1.3 cap those skips are expected and only reported in the detail. The
golden run reports the device class that rendered, read from the harness's log lines
(Tests/Source/Support/GoldenImage.cpp), and ends with "warning" when goldens are missing for that device class (smoke
mode) or when --update-golden wrote candidates into Tests/Golden/<DeviceClass>/, which must be reviewed (the git diff)
before they are committed. A device class without an image's golden while another class of the same vendor has it (the
driver's major version changed; M18's device-class check) fails that test case, never smoke-tests it, and the step's
detail names the images and points to the rename procedure (Tests/Golden/README.md). A failed comparison writes
<Name>-actual.png, <Name>-expected.png and <Name>-diff.png into bin/TestResults/Golden/.

Skipped test cases (Roadmap rule 3, Docs/Decisions/0004-contract-stub-gate.md): after every run of the Tests binary
(unit, gpu, golden), it lists the test cases the run's filters select, skipped ones included (doctest --no-skip
--list-test-cases --reporters=xml --out=<file>; each test case carries its suite and its doctest::skip flag). The only
test cases that may stay skipped are the child-process targets of the ChildTargets suite (Test::ChildTargetSuite): a
skipped test case in any other suite, such as a contract task's not yet implemented test, fails the run and is named.
--allow-skips (contract mode, a contract checkpoint's targeted runs on a milestone branch, ADR 0028 decision 11;
PreCommit.py --contract and CI.py --contract pass it too, but no gate run of the backlog program uses them) reports them
without failing. The counts are in the step detail and, with --json, in the step's "skipCheck" object.

The automation suite (Roadmap M4, Docs/Decisions/0008-m4-decisions.md decisions 33 and 34; its skip rule is
decision 16):
  - The tests start editors themselves (Tests/Automation/harness.py, the bridge's launcher); ENGINE_AUTOMATION_CONFIG
    names the configuration whose editor they use. A missing editor build or virtual environment fails the run (exit
    code 3); Python tests never skip, so a skipped test fails it too (--allow-skips reports them instead).
  - Method coverage: every engine_client call is appended to the file ENGINE_AUTOMATION_COVERAGE names. After the run,
    `Editor --headless --renderer none --dump-reference <dir>` lists the registered methods (Methods.json leaves out the
    debug.* test hooks), and the run fails naming every registered method no Python test called. With --filter the
    gate is not checked, because a partial run cannot cover every method.
  - --filter takes comma-separated unittest -k patterns on test ids ("test_batch", "*Launcher*,test_client").
  - Tests that need a rendering editor (the screenshot methods, Docs/Decisions/0009-m5-decisions.md decision 33) probe
    for a Vulkan device first (Tests/Automation/harness.py, require_gpu). Without one they print the GPU suite's
    no-device line and pass without running, which ends the run with the status "warning" like a gpu run; with
    --require-gpu (ENGINE_AUTOMATION_REQUIRE_GPU=1 for the tests) they fail instead.

The feature suite (Docs/Decisions/0022-m14-contract.md decision 7) runs `Editor --headless --project
Projects/FeatureTest --run-tests --strict --audio-device none --expect-no-gpu-errors --junit <report>` per
configuration, with --user-data-dir in a temporary directory. --strict reads the committed project strictly: an unknown
field or component in its .eproj, scenes or prefabs is an error, not a warning (Architecture §6); the export and
determinism suites' editors take it too. With --junit the report is bin/TestResults/Feature-<Config>.xml and the
editor's JSON report lands beside it as Feature-<Config>.json; without it both live in a temporary directory that is
removed. It fails, printing one "UNCOVERED gate <g> (Editor): <id>" line per gap, when the editor exits non-zero, when
any of the gates 1, 1b, 2, 3, 4 and 9 is absent, unevaluated or failed, when a case row is skipped, and when any file
under Projects/FeatureTest outside Library/ differs from a byte snapshot taken before the run. An .hdr import needs the
GPU baker, so the run needs a Vulkan device: without one it prints the GPU suite's no-device line and passes without
running, as a warning, unless --require-gpu is given (Scripts/Lib/coverage.py reads and evaluates the report).

The export suite (Docs/Decisions/0024-m15-contract.md decision 9) needs a Vulkan device, like the feature suite: without
one it prints the no-device line and passes without running, as a warning, unless --require-gpu is given. It copies
Projects/FeatureTest with Library/ into a temporary directory, starts the Release editor on the copy (`--headless
--renderer vulkan --audio-device none --automation --strict`) and, per configuration, calls project.export {config,
testing: true}, moves the package to bin/Exports/<Config>/FeatureTest/ (Scripts/Lib/exports.py) and runs
`<package>/FeatureTest --headless --feature-test` (Release adds --expect-no-gpu-errors). With --junit the run's working
directory is the repository root, so its reports are bin/TestResults/FeatureTest-<Config>.{json,xml}; without it, a
temporary directory. A configuration fails when the export or the run fails (printing the end of
<package>/bin/TestUserData/FeatureTest/Logs/FeatureTest.log), when the run's user data is not in
<package>/bin/TestUserData/FeatureTest or a folder named after the product exists beside it, when
coverage.evaluate_feature_report against the configuration's own mode fails ("UNCOVERED gate <g> (<Mode>): <id>"
lines), and when the report's Api.Debug suite row does not count exactly one break (exports.debug_break_failures:
Debug.Break stays a counted no-op in the shipping binary, ADR 0024 decision 6). With both configurations, the "merge
coverage" step merges the Editor report (--editor-report, default bin/TestResults/Feature-Release.json) with both
exported ones and prints one "merge: <rule>: <subject>" line per failure; an Editor report older than the Release editor
or a FeatureTest file outside Library/ (or missing) fails with exit code 3 naming `Test.py --suite feature --config
Release --junit`, which is never rerun here. With --junit it writes bin/TestResults/Export.xml: "export <Config>" per
configuration and "merge coverage".

The determinism suite (ADR 0022 decision 7, ADR 0024 decision 10, §15.1 T3d) has four runs (exports.DETERMINISM_RUNS):
Debug selects Editor Debug, Release Editor Release and Exported Release, Dist Exported Dist. Each editor run copies
Projects/FeatureTest without Library/ into a temporary directory, starts `Editor --headless --renderer none
--audio-device none --automation --strict` on it and runs input.replay {path, verify: true, strictHash: true} for every
file Testing.Replays matches (the editor's glob rules), then the input check of each one with input events (it must
fail input.replay {verify, strictHash, withoutInput}, M16 friction S2-01); it needs no GPU. An exported run reads the
"replay:<path>" rows of bin/TestResults/FeatureTest-<Config>.json, which the export suite of the same tree wrote, and
is never rerun: a report older than the <Config> Runtime, the Release editor or a FeatureTest file outside Library/ (or
missing) fails the step with exit code 3 naming `Test.py --suite export --junit` under --require-gpu, and otherwise
leaves that run out of the comparison with the reason, as a warning. A replay fails when an editor run's verification
or input check fails, when a run has no hash, when a hash differs from the committed FinalStateHash, or when the runs
disagree. With --junit it writes bin/TestResults/Determinism.xml, one test case per replay, whose failure lists each
run's hash. A project whose Testing.Replays matches nothing ends as a warning.

The games suite (Docs/Decisions/0026-m16-contract.md decisions 7 and 8; Docs/Decisions/0027-m17-contract.md decision
4) runs over the staged game projects (Scripts/Lib/games.py discover_projects in index mode: every directory directly
below Projects/ or Tests/Projects/ whose root holds one staged *.eproj, Projects/FeatureTest excluded); a project found
on disk but not staged gets one passed step, "games <Name>", and nothing else, and without any project the suite passes
with one step, "games". <Name> in the names of the steps and reports is the .eproj's file stem. Per project:
  - "games <Name> audit": Scripts/Lib/provenance.py's audit of the staged files (the acceptance mode, as
    Scripts/AuditProvenance.py --project runs it), failing with each finding's code and path;
  - "games <Name> Editor <Config>" for Debug and Release: the project's staged blobs materialized into a temporary
    directory (no Library/, no Build/), the strict rendering editor of that configuration on them (`--headless
    --renderer vulkan --audio-device none --automation --strict --expect-no-gpu-errors`, with --user-data-dir and
    --export-root in the temporary directory), then first the copy's build settings (games.check_build_settings:
    StartScene and Export.BuildScenes set, as the demo acceptance of Roadmap M17, M25 and M27 asks, and
    Export.BuildScenes listing StartScene, the demos' convention; its lines come first, and the other checks run either
    way), script.check and project.validate (warnings included) clean, project.upgrade {dryRun: true} without a changed
    file (each one names `Editor --headless --project <dir> --upgrade`), and test.run {} passed in Editor mode with a
    suite row for every Testing.Suites entry of that mode, no skipped case row and a passing "replay:<path>" row at the
    committed FinalStateHash for every replay Testing.Replays matches; at least one of those replays must have input
    events (games.check_replay_acceptance: a game whose Testing.Replays matches no replay, or only replays without
    input, fails, as the demo acceptance of Roadmap M16, M17, M25 and M27 asks); every such replay with input events
    must then fail input.replay {verify, strictHash, withoutInput} (the input check, M16 friction S2-01: a replay whose
    verification passes without its input proves nothing); a .replay below Assets/ that Testing.Replays leaves out fails
    it; after session.shutdown the editor must exit 0 and the copy must be unchanged outside Library/;
  - "games <Name> Dist", which the Release editor drives (in the Release half's session when Release runs too):
    project.export {config: Dist, testing: true}, whose result's executable must lie below its outputDirectory and
    whose manifest (Game.json, on macOS in the bundle's Contents/Resources) names the game (games.read_game_name), each
    a one-line failure otherwise, installed to bin/Exports/Dist/<Game>/, <Game> being that Name (ProjectSettings::Name,
    after which the exporter names the executable and the testing user data, whatever the file stem); the package's
    executable run with `--headless --feature-test` in a temporary working directory (so the export suite's
    FeatureTest-Dist reports are never touched), its exit code (printing the end of its log), its user data
    (bin/TestUserData/<Game>/ in the package, holding Logs/<exe>.log, named after the executable's stem as the Runtime
    names it, Architecture §4.4), its report in Dist mode, each replay's hash against the committed one
    and every editor half of the run, and games.check_replay_acceptance; then a plain project.export {config: Dist,
    smokeTest: true}, whose smoke test must exit 0 (one that did not run warns, and fails with --require-gpu). In a
    session of its own (a run without Release), whatever fails after these checks (a refused session.shutdown, a broken
    connection, a timeout, the editor's exit code) fails it too.
The builds the requested halves need (each editor half's editor; the Release editor and the Dist Runtime for the Dist
half) are looked up after the audits: a missing one fails its half with exit code 3. Then the device is probed once;
without one the editor and Dist halves pass without running, as warnings, unless --require-gpu is given, and nothing
is copied or started. With --junit it writes bin/TestResults/Games-<Config>.xml per requested configuration (the audit
and suite-level steps in the first one), Games-<Name>-Editor-<Config>.json (the test.run document) and
Games-<Name>-Dist.{json,xml} (the exported run's reports).

--merge-coverage <Editor.json> <Release.json> <Dist.json> runs only the cross-mode union merge of §15.6
(coverage.merge_coverage, decision 6) and prints one "merge: <rule>: <subject>" line per failure; it exits 0 when the
union covers every registered entry and every report passed, 1 otherwise. A report that does not exist or cannot be read
is a mode without a report: "merge: report: <mode>" for its position, exit 1.

Sanitized runs (the hardening stage, Docs/Decisions/0024-m15-contract.md decision 12): --build-root <dir> takes the
executables from <dir>/<OutputDir>/ instead of bin/<OutputDir>/ (the AddressSanitizer workspace builds into
bin-int/Asan/bin), and --sanitizer address gives every process the suite starts the AddressSanitizer environment
(Scripts/Lib/hardening.py asan_environment: ASAN_OPTIONS and the runtime's directory first on PATH, so the sanitized
executables and every child they start load MSVC's runtime; exit code 3 when the runtime is missing). The step names
gain " (ASan)" and the reports the suffix -Asan (Unit-Release-Asan.xml, Feature-Release-Asan.{xml,json}); every
"ERROR: AddressSanitizer:" line a process prints is collected into the step's detail and fails the step.
--test-timeout-scale <s> passes --test-timeout-scale=<s> to the Tests binary, which multiplies every per-test-case
limit. --fuzz-scale <s> passes --fuzz-scale=<s> to the unit suite's Tests runs (each fuzz case runs round(count * s)
iterations, at least its minimum, Tests/Source/Support/Fuzz.h). Without it the Debug unit run passes --fuzz-scale=0.1,
so it executes the first tenth of each fuzz case's sequence, and the Release unit run passes none and executes every
count (the product owner's decision, Docs/Decisions/0024-m15-contract.md decision 14; the hardening stage runs
1,000,000 per reader).
--exclude-test-suites <names> leaves those doctest test suites out of the unit suite's Tests runs, besides GPU and
Golden: the hardening stage's sanitized unit run leaves out Fuzz, whose cases its fuzz step runs over a superset of the
same iterations.
--debug-heap (Windows only; the unit, gpu and golden suites; the hardening stage's debug-heap GPU step,
Docs/Decisions/0029-m18-contract.md) starts each Tests process on the Windows debug heap (Scripts/Lib/debugheap.py:
under a debugger that detaches at the loader's initial breakpoint, so every heap of the process, the graphics driver's
included, checks block tails, fills freed blocks and validates parameters, while its crash handler still reports a
crash; the processes it starts run on their own heaps). The step names gain " (debug heap)" and the reports the suffix
-DebugHeap (Gpu-Debug-Vulkan14-DebugHeap.xml).

Machine-wide locks (Scripts/Lib/locks.py; Docs/Architecture.md §2.3, ADR 0028 decision 16): before its first suite, a
run that includes a GPU-using suite (gpu, golden, feature, automation, export, determinism or games; --update-golden
is a golden run) takes the GPU lock, shared by every checkout of the repository, and holds it to its end, so one GPU
run goes at a time; every other run only waits until no gate (CI.py) runs. Both wait while a gate runs, unless the run
is inside it or inside another run that holds a lock (Hardening.py's suites). The wait reports the holder (pid,
worktree, command) and lasts at most --lock-timeout seconds (exit code 5); --no-wait fails at once instead (exit code
3). --merge-coverage takes no lock.

--junit writes bin/TestResults/<Suite>-<Config>[-<Cap>].xml (JUnit; test suite named "<Suite>.<Config>[.<Cap>]", where
<Cap> is Vulkan13 or Vulkan14 for the gpu suite; the automation suite has one test suite per test directory and one for
the coverage gate; the feature suite keeps the editor's own suites FeatureTest and FeatureTest.coverage; the determinism
suite writes Determinism.xml). The console then shows a summary and every failure parsed from that file instead of
doctest's console reporter. If the test process dies before writing it, a JUnit file with one error test case records
the crash.

Exit codes: 0 every requested suite that exists passed, 1 a suite failed or none of the requested suites exists
yet, 2 usage error, 3 a test executable or the MCP virtual environment is missing (build it, or run Setup.py), a
report the export or determinism suite reads is missing or stale (run the suite that writes it), a game's
provenance files are missing or git fails, or --no-wait found a machine lock held, 5 timeout (of a run, or of the wait
for a machine lock).
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"Test.py requires Python 3.10 or newer (this is Python {platform.python_version()})")

import argparse
import dataclasses
import datetime
import hashlib
import json
import os
import re
import shutil
import tempfile
import time
import xml.etree.ElementTree as ElementTree
from collections.abc import Iterator
from pathlib import Path
from typing import Any

from Lib import coverage, debugheap, exports, games, hardening, locks, paths, provenance
from Lib.process import (
    ToolNotFoundError,
    child_environment,
    describe_exit_code,
    format_command,
    run_captured,
    run_streamed,
)
from Lib.report import (
    EXIT_FAILED,
    EXIT_INIT_FAILED,
    EXIT_SUCCESS,
    EXIT_USAGE,
    Console,
    Status,
    Step,
    configure_stdio,
    emit_json,
    overall_exit_code,
    write_junit,
)

SUITE_TIMEOUT_SECONDS = 1800.0
# The doctest suites of the device-dependent stages (Tests/Source/Support/HeadlessGpuFixture.h: Test::GpuSuite and
# Test::GoldenSuite). The unit stage runs every other suite (§15.8).
GPU_TEST_SUITE = "GPU"
GOLDEN_TEST_SUITE = "Golden"
UNIT_EXCLUDED_TEST_SUITES = f"{GPU_TEST_SUITE},{GOLDEN_TEST_SUITE}"
DOCTEST_SUMMARY_PATTERN = re.compile(
    r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed\s*\|\s*(\d+) skipped"
)
# The doctest suite of the test cases that only run in a child process spawned by another test: Test::ChildTargetSuite
# (Tests/Source/Support/TestOptions.h). They are the only test cases that stay skipped (Docs/Decisions/
# 0003-m1-contract-decisions.md decision 7).
CHILD_TARGET_SUITE = "ChildTargets"
LISTING_TIMEOUT_SECONDS = 120.0
UNEXPECTED_SKIPS_IN_DETAIL = 3
NAMES_IN_DETAIL = 4

# The Vulkan API caps of the gpu suite (§8.1): the Tests option --vulkan-api=<cap>. 1.4 is also the cap of a run
# without the option.
VULKAN_API_CAPS = ("1.4", "1.3")
# The cap whose runs must exercise the Vulkan 1.4 paths: a skipped host-image-copy case there is a warning.
FULL_API_CAP = "1.4"
# The log line Test::ReportGpuUnavailable writes for a GPU test case without a device (HeadlessGpuFixture.cpp).
NO_DEVICE_PATTERN = re.compile(r"GPU test without a device \(passes without running\): (?P<reason>.*)$")
# The log line of a host-image-copy case the device cannot run (HostImageUploadTests.cpp, ReportHostCopySkipped).
HOST_COPY_SKIP_PATTERN = re.compile(r"Host image copy skipped for (?P<case>'[^']*' \([^)]*\)): (?P<skip_reason>.*)$")
# The log lines of the golden harness (GoldenImage.cpp): "Golden image '<name>' on device class '<class>'" followed by
# ": matched", ": goldens missing", ": wrote the candidate", ": device class changed" or " does not match".
GOLDEN_PATTERN = re.compile(
    r"Golden image '(?P<name>[^']+)' on device class '(?P<device>[^']+)'"
    r"(?::\s(?P<event>matched|goldens missing|wrote the candidate|device class changed)|\s(?P<mismatch>does not match))"
)
# The first line of every AddressSanitizer report ("==1234==ERROR: AddressSanitizer: heap-buffer-overflow on ...").
SANITIZER_ERROR_PATTERN = re.compile(r"ERROR: AddressSanitizer: .*$")
REPORTED_LINES_PATTERN = re.compile(
    f"{NO_DEVICE_PATTERN.pattern}|{HOST_COPY_SKIP_PATTERN.pattern}|{GOLDEN_PATTERN.pattern}|"
    f"{SANITIZER_ERROR_PATTERN.pattern}"
)
# --sanitizer: the sanitizers Test.py knows, with the label of their step names and the suffix of their report stems.
SANITIZERS = {"address": ("ASan", "Asan")}
# --debug-heap (the hardening stage's debug-heap GPU step, Docs/Decisions/0029-m18-contract.md): the label of its step
# names and the suffix of its report stems, and the suites whose Tests runs take it.
DEBUG_HEAP_LABELS = ("debug heap", "DebugHeap")
DEBUG_HEAP_SUITES = ("unit", "gpu", "golden")
# The --fuzz-scale of the unit runs that pass no --fuzz-scale, by configuration (Docs/Decisions/0024-m15-contract.md
# decision 14, approved by the product owner): Debug executes the first tenth of each fuzz case's sequence (at least its
# minimum); Release, absent here, executes every count.
DEFAULT_FUZZ_SCALES = {"Debug": 0.1}

# The automation suite (§15.7): its test directories, the runner, and the environment the tests read.
AUTOMATION_TEST_DIRECTORIES = (paths.REPOSITORY_ROOT / "Tests" / "Automation",
                               paths.REPOSITORY_ROOT / "Tools" / "MCP" / "tests")
UNITTEST_RUNNER = paths.REPOSITORY_ROOT / "Scripts" / "Lib" / "unittest_runner.py"
MCP_VENV = paths.REPOSITORY_ROOT / "Tools" / "MCP" / ".venv"
RUNNER_EXIT_SKIPPED = 4  # unittest_runner.py: every test passed, but some were skipped
AUTOMATION_CONFIG_VARIABLE = "ENGINE_AUTOMATION_CONFIG"
AUTOMATION_COVERAGE_VARIABLE = "ENGINE_AUTOMATION_COVERAGE"
# --require-gpu for the tests that start rendering editors (Tests/Automation/harness.py).
AUTOMATION_REQUIRE_GPU_VARIABLE = "ENGINE_AUTOMATION_REQUIRE_GPU"
# Settings of the bridge and the tests that a developer's environment must not leak into the suite.
AUTOMATION_CLEARED_VARIABLES = ("ENGINE_MCP_TRANSCRIPT", "ENGINE_MCP_USER_DATA_DIR")
DUMP_REFERENCE_TIMEOUT_SECONDS = 300.0
COVERAGE_CASE_NAME = "Method coverage: every registered method has at least one Python test"
UNCOVERED_IN_DETAIL = 5

# The feature and determinism suites (ADR 0022 decision 7): the FeatureTest project, the Library/ directory that both
# leave out (the editor's import cache), the options of the rendering editor's device probe (Tests/Automation/harness.py
# GPU_EDITOR_ARGUMENTS) and the text of the no-device line the GPU suite and the automation harness print.
FEATURE_PROJECT = paths.REPOSITORY_ROOT / "Projects" / "FeatureTest"
FEATURE_PROJECT_FILE = FEATURE_PROJECT / "FeatureTest.eproj"
PROJECT_CACHE_DIRECTORY = "Library"
# A project's export output (project.export without --export-root writes <Project>/Build/), which no copy takes.
PROJECT_BUILD_DIRECTORY = "Build"
GPU_PROBE_ARGUMENTS = ("--gpu-validation=sync", "--expect-no-gpu-errors")
GPU_PROBE_TIMEOUT_SECONDS = 120.0
NO_DEVICE_TEXT = "GPU test without a device (passes without running): "
FAILURES_IN_DETAIL = 3
DETERMINISM_REPORT_STEM = "Determinism"
EDITOR_SHUTDOWN_TIMEOUT_SECONDS = 60.0

# The export suite (Docs/Decisions/0024-m15-contract.md decision 9): its JUnit file, the options of the exporting editor
# (a rendering one: the FeatureTest export bakes Gradient.hdr), the project lock file a copy leaves out, the Editor
# report its merge reads by default, the name of the merge step and the lines of a failed run's log it prints.
EXPORT_REPORT_STEM = "Export"
# The feature, determinism and export suites read the committed FeatureTest strictly: unknown fields and components in
# its .eproj, scenes and prefabs are errors instead of warnings (Architecture §6; the editor reads every project it
# opens so, through --project and project.open alike).
STRICT_EDITOR_ARGUMENT = "--strict"
EXPORT_EDITOR_ARGUMENTS = ("--headless", "--renderer", "vulkan", "--audio-device", "none", "--automation",
                           STRICT_EDITOR_ARGUMENT)
DETERMINISM_EDITOR_ARGUMENTS = ("--headless", "--renderer", "none", "--audio-device", "none", "--automation",
                                STRICT_EDITOR_ARGUMENT)
PROJECT_LOCK_FILE = "Editor.lock"
DEFAULT_EDITOR_REPORT = paths.TEST_RESULTS_ROOT / "Feature-Release.json"
MERGE_STEP_NAME = "merge coverage"
LOG_TAIL_LINES = 40

# The games suite (Docs/Decisions/0026-m16-contract.md decision 8): the strict rendering editor of the editor halves and
# of the Dist half's exports, failing its exit code on any GPU error or warning; the exported configuration and the
# label of its run among exports.DETERMINISM_RUNS; the suite-level step and its note without a game; the findings an
# audit step's detail names before it counts the rest (the console and the step's data list every one).
GAMES_EDITOR_ARGUMENTS = ("--headless", "--renderer", "vulkan", "--audio-device", "none", "--automation",
                          STRICT_EDITOR_ARGUMENT, "--expect-no-gpu-errors")
DIST_HALF = games.EXPORTED_CONFIGURATION
EXPORTED_DIST_RUN = f"Exported {DIST_HALF}"
GAMES_STEP_NAME = "games"
NO_GAME_PROJECT_NOTE = "no game project below Projects/ or Tests/Projects/ (FeatureTest excluded)"
GAMES_FINDINGS_IN_DETAIL = 20
GIT_TIMEOUT_SECONDS = 120.0
# The folder of a macOS bundle's Contents/ that holds Game.json, beside the Contents/MacOS folder of its executable
# (Editor/Source/EditorCore/Export/AppBundle.h; ADR 0027 decision 4, B2).
MACOS_BUNDLE_RESOURCES = "Resources"


# The configurations whose binaries the doctest, feature and automation suites run: test executables and the editor do
# not exist in Dist (§2.2).
DEVELOPMENT_CONFIGURATIONS = ("Debug", "Release")


@dataclasses.dataclass(frozen=True)
class Suite:
    name: str
    configurations: tuple[str, ...]  # the §15.8 matrix
    available: bool
    planned: str = ""  # where an unavailable suite lands (Docs/Roadmap.md)
    allowed: tuple[str, ...] = DEVELOPMENT_CONFIGURATIONS  # the configurations --config may name for it


# The suites that run the GPU or an editor that may render: a run that includes one holds the machine-wide GPU lock.
GPU_SUITES = frozenset({"gpu", "golden", "feature", "automation", "export", "determinism", "games"})

SUITES = {
    suite.name: suite
    for suite in (
        Suite("unit", ("Debug", "Release"), True),
        Suite("gpu", ("Debug", "Release"), True),
        Suite("golden", ("Release",), True),
        Suite("feature", ("Debug", "Release"), True),
        Suite("automation", ("Release",), True),
        Suite("export", exports.EXPORTED_CONFIGURATIONS, True, allowed=exports.EXPORTED_CONFIGURATIONS),
        Suite("determinism", ("Debug", "Release", "Dist"), True, allowed=paths.CONFIGURATIONS),
        Suite("games", ("Debug", "Release", "Dist"), True, allowed=paths.CONFIGURATIONS),
    )
}


@dataclasses.dataclass(frozen=True)
class TestRun:
    """One run of the Tests executable: a suite in a configuration, with the options of that run."""

    suite: str
    config: str
    filters: tuple[str, ...]  # the doctest filters that select the suite's test cases
    options: tuple[str, ...] = ()  # Tests options of the run (--vulkan-api=1.3, --require-gpu, --update-golden)
    cap: str = ""  # the gpu suite's API cap ("1.3" or "1.4")
    sanitizer: str = ""  # --sanitizer of the run ("address"), or empty
    debug_heap: bool = False  # --debug-heap: the Tests process runs on the Windows debug heap (Lib/debugheap.py)

    @property
    def name(self) -> str:
        return (f"{self.suite} {self.config}" + (f" (Vulkan {self.cap})" if self.cap else "")
                + sanitizer_name_suffix(self.sanitizer) + (f" ({DEBUG_HEAP_LABELS[0]})" if self.debug_heap else ""))

    @property
    def report_stem(self) -> str:
        return "-".join([self.suite.capitalize(), self.config, *([cap_label(self.cap)] if self.cap else []),
                         *([SANITIZERS[self.sanitizer][1]] if self.sanitizer else []),
                         *([DEBUG_HEAP_LABELS[1]] if self.debug_heap else [])])

    @property
    def junit_suite_name(self) -> str:
        return ".".join([self.suite.capitalize(), self.config, *([cap_label(self.cap)] if self.cap else []),
                         *([SANITIZERS[self.sanitizer][1]] if self.sanitizer else []),
                         *([DEBUG_HEAP_LABELS[1]] if self.debug_heap else [])])


def cap_label(cap: str) -> str:
    return "Vulkan" + cap.replace(".", "")


def sanitizer_name_suffix(sanitizer: str | None) -> str:
    """" (ASan)" for a sanitized run's step name, empty otherwise."""
    return f" ({SANITIZERS[sanitizer][0]})" if sanitizer else ""


def sanitizer_stem_suffix(sanitizer: str | None) -> str:
    """"-Asan" for a sanitized run's report stem, empty otherwise."""
    return f"-{SANITIZERS[sanitizer][1]}" if sanitizer else ""


def suite_environment(arguments: argparse.Namespace, extra: dict[str, str] | None = None) -> dict[str, str]:
    """The environment of every process the suite starts: child_environment(extra), plus with --sanitizer the
    AddressSanitizer environment (hardening.asan_environment). Raises FileNotFoundError when the runtime is missing."""
    environment = child_environment(extra)
    if getattr(arguments, "sanitizer", None):
        environment = hardening.asan_environment(environment)
    return environment


def missing_sanitizer_runtime(name: str, error: FileNotFoundError) -> Step:
    return Step(name, Status.FAILED, f"the AddressSanitizer runtime is missing: {error}", exit_code=EXIT_INIT_FAILED)


def describe_sanitizer_errors(lines: list[str]) -> str:
    """The step detail's part for the "ERROR: AddressSanitizer:" lines a run printed: their count and the first one."""
    reports = [match.group(0) for match in map(SANITIZER_ERROR_PATTERN.search, lines) if match]
    if not reports:
        return ""
    return f"{len(reports)} AddressSanitizer report(s), first: {reports[0]}"


@dataclasses.dataclass
class TestCounts:
    executed: int = 0
    passed: int = 0
    failed: int = 0
    skipped: int = 0
    failures: list[str] = dataclasses.field(default_factory=list)


class SkipCheckError(Exception):
    """The Tests binary could not list its test cases."""


@dataclasses.dataclass
class SkipCheck:
    """The test cases a run's filters select that doctest::skip leaves out, by name."""

    skipped: list[str]
    child_targets: list[str]  # the selected test cases of the ChildTargets suite, skipped or not
    unexpected: list[str]  # skipped test cases outside the ChildTargets suite

    def to_json(self, allowed: bool) -> dict[str, Any]:
        return {"skipped": len(self.skipped), "childTargets": len(self.child_targets),
                "unexpected": self.unexpected, "allowed": allowed}


@dataclasses.dataclass
class DeviceReport:
    """What a gpu or golden run's log says about the device: test cases without one, and the golden images."""

    no_device: list[str] = dataclasses.field(default_factory=list)  # the reasons, one per test case
    host_copy_skips: list[str] = dataclasses.field(default_factory=list)  # "<case>: <reason>", one per skipped case
    device_classes: set[str] = dataclasses.field(default_factory=set)
    matched: set[str] = dataclasses.field(default_factory=set)
    goldens_missing: set[str] = dataclasses.field(default_factory=set)
    candidates_written: set[str] = dataclasses.field(default_factory=set)
    mismatched: set[str] = dataclasses.field(default_factory=set)
    # The images whose device class has no golden while a class of the same vendor has one (the device-class check).
    class_changed: set[str] = dataclasses.field(default_factory=set)

    @staticmethod
    def parse(lines: list[str]) -> DeviceReport:
        report = DeviceReport()
        for line in lines:
            no_device = NO_DEVICE_PATTERN.search(line)
            if no_device:
                # A test case reports once in the log; doctest's console reporter repeats the message in other words.
                report.no_device.append(no_device.group("reason").strip())
                continue
            host_copy_skip = HOST_COPY_SKIP_PATTERN.search(line)
            if host_copy_skip:
                skip_reason = host_copy_skip.group("skip_reason").strip()
                report.host_copy_skips.append(f"{host_copy_skip.group('case')}: {skip_reason}")
                continue
            golden = GOLDEN_PATTERN.search(line)
            if not golden:
                continue
            report.device_classes.add(golden.group("device"))
            name = golden.group("name")
            if golden.group("mismatch"):
                report.mismatched.add(name)
            elif golden.group("event") == "matched":
                report.matched.add(name)
            elif golden.group("event") == "goldens missing":
                report.goldens_missing.add(name)
            elif golden.group("event") == "device class changed":
                report.class_changed.add(name)
            else:
                report.candidates_written.add(name)
        return report

    def to_json(self) -> dict[str, Any]:
        return {
            "noDevice": len(self.no_device),
            "hostCopySkips": self.host_copy_skips,
            "deviceClasses": sorted(self.device_classes),
            "matched": sorted(self.matched),
            "goldensMissing": sorted(self.goldens_missing),
            "candidatesWritten": sorted(self.candidates_written),
            "mismatched": sorted(self.mismatched),
            "deviceClassChanged": sorted(self.class_changed),
        }

    def describe_host_copy_skips(self) -> str:
        return f"host image copy skipped for {len(self.host_copy_skips)} case(s) ({self.host_copy_skips[0]})"

    def notes(self, require_gpu: bool, cap: str) -> list[str]:
        """The things a reader must look at even though the run passed."""
        notes: list[str] = []
        if self.no_device and not require_gpu:
            notes.append(f"{len(self.no_device)} test case(s) found no GPU device and passed without running "
                         f"({self.no_device[0]}); --require-gpu fails them instead")
        if self.host_copy_skips and cap == FULL_API_CAP:
            notes.append(f"{self.describe_host_copy_skips()}: the Vulkan 1.4 path did not run there")
        classes = ", ".join(sorted(self.device_classes))
        if self.goldens_missing:
            notes.append(f"goldens missing on device class {classes} for {describe_names(self.goldens_missing)} "
                         f"(smoke mode, never a pass of the comparison)")
        if self.candidates_written:
            directories = ", ".join(f"Tests/Golden/{device}/" for device in sorted(self.device_classes))
            notes.append(f"--update-golden wrote {len(self.candidates_written)} candidate(s) into {directories}: "
                         f"review the diff before committing")
        return notes


def describe_names(names: set[str] | list[str]) -> str:
    ordered = sorted(names)
    shown = ", ".join(ordered[:NAMES_IN_DETAIL])
    more = len(ordered) - NAMES_IN_DETAIL
    return shown + (f" and {more} more" if more > 0 else "")


def list_skipped_test_cases(executable: Path, filters: list[str],
                            environment: dict[str, str] | None = None) -> SkipCheck:
    """Ask the Tests binary which test cases `filters` select, skipped ones included (--no-skip). doctest's XML
    reporter gives each <TestCase> its suite and its doctest::skip flag; the listing goes to a file (--out), so the log
    on stderr never interleaves with it. `environment` is the suite's (default: child_environment())."""
    with tempfile.TemporaryDirectory(prefix="Test-SkipCheck-") as directory:
        listing = Path(directory) / "TestCases.xml"
        command = [str(executable), *filters, "--no-skip", "--list-test-cases", "--reporters=xml", f"--out={listing}"]
        try:
            result = run_captured(command, cwd=paths.REPOSITORY_ROOT,
                                  env=environment if environment is not None else child_environment(),
                                  timeout=LISTING_TIMEOUT_SECONDS)
        except ToolNotFoundError as error:
            raise SkipCheckError(str(error)) from error
        if not result.succeeded:
            raise SkipCheckError(f"{format_command(command)} {result.describe_exit()}: {result.tail(5)}")
        try:
            cases = list(ElementTree.parse(listing).getroot().iter("TestCase"))
        except (OSError, ElementTree.ParseError) as error:
            raise SkipCheckError(f"cannot read the test case listing of {format_command(command)}: {error}") from error
    skipped = sorted(case.get("name", "") for case in cases if case.get("skipped") == "true")
    child_targets = sorted(case.get("name", "") for case in cases if case.get("testsuite") == CHILD_TARGET_SUITE)
    unexpected = sorted(case.get("name", "") for case in cases
                        if case.get("skipped") == "true" and case.get("testsuite") != CHILD_TARGET_SUITE)
    return SkipCheck(skipped, child_targets, unexpected)


def describe_unexpected_skips(skips: SkipCheck) -> str:
    names = ", ".join(f"'{name}'" for name in skips.unexpected[:UNEXPECTED_SKIPS_IN_DETAIL])
    more = len(skips.unexpected) - UNEXPECTED_SKIPS_IN_DETAIL
    return names + (f" and {more} more" if more > 0 else "")


def tests_executable(config: str, build_root: Path = paths.BIN_ROOT) -> Path:
    """<build root>/<OutputDir>/Tests/Tests: the repository's bin/ by default, --build-root otherwise."""
    return build_root / paths.output_directory_name(config) / "Tests" / paths.executable_name("Tests")


def junit_path(report_stem: str) -> Path:
    return paths.TEST_RESULTS_ROOT / f"{report_stem}.xml"


def read_junit(path: Path, suite_name: str | None) -> TestCounts:
    """Count a JUnit file's test cases and, with `suite_name`, rename its test suites (doctest names its suite after the
    executable's path)."""
    tree = ElementTree.parse(path)
    root = tree.getroot()
    counts = TestCounts()
    for test_suite in root.iter("testsuite"):
        if suite_name is not None:
            test_suite.set("name", suite_name)
    for case in root.iter("testcase"):
        problems = case.findall("failure") + case.findall("error")
        if case.find("skipped") is not None or case.get("status", "run") not in ("run", ""):
            counts.skipped += 1
            continue
        counts.executed += 1
        if problems:
            counts.failed += 1
            for problem in problems:
                message = problem.get("message", "") or ""
                text = (problem.text or "").strip()
                counts.failures.append(f"{case.get('name')} ({case.get('classname')}): {message} {text}".strip())
        else:
            counts.passed += 1
    tree.write(path, encoding="utf-8", xml_declaration=True)
    return counts


def write_crash_junit(path: Path, suite_name: str, message: str, output_tail: str) -> None:
    """A JUnit file for a run that produced none (crash, timeout), so CI result viewers show the failure."""
    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    root = ElementTree.Element("testsuites")
    suite = ElementTree.SubElement(
        root, "testsuite",
        {"name": suite_name, "tests": "1", "failures": "0", "errors": "1", "skipped": "0", "timestamp": timestamp},
    )
    case = ElementTree.SubElement(suite, "testcase", {"classname": suite_name, "name": "test process"})
    ElementTree.SubElement(case, "error", {"message": message}).text = output_tail
    ElementTree.indent(root, space="\t")
    path.parent.mkdir(parents=True, exist_ok=True)
    ElementTree.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)


def plan_runs(suite: str, config: str, arguments: argparse.Namespace) -> list[TestRun]:
    """The Tests runs of `suite` in `config`: one, except for the gpu suite, which runs once per API cap."""
    gpu_options = ("--require-gpu",) if arguments.require_gpu else ()
    sanitizer = getattr(arguments, "sanitizer", None) or ""
    debug_heap = bool(getattr(arguments, "debug_heap", False))
    timeout_scale = getattr(arguments, "test_timeout_scale", None)
    common = (f"--test-timeout-scale={timeout_scale:g}",) if timeout_scale else ()
    if suite == "unit":
        fuzz_scale = getattr(arguments, "fuzz_scale", None) or DEFAULT_FUZZ_SCALES.get(config)
        fuzz_options = (f"--fuzz-scale={fuzz_scale:g}",) if fuzz_scale else ()
        excluded = ",".join([UNIT_EXCLUDED_TEST_SUITES, *(getattr(arguments, "exclude_test_suites", None) or [])])
        return [TestRun(suite, config, (f"--test-suite-exclude={excluded}",), (*common, *fuzz_options),
                        sanitizer=sanitizer, debug_heap=debug_heap)]
    if suite == "gpu":
        caps = [arguments.vulkan_api] if arguments.vulkan_api else list(VULKAN_API_CAPS)
        return [TestRun(suite, config, (f"--test-suite={GPU_TEST_SUITE}",),
                        (f"--vulkan-api={cap}", *gpu_options, *common), cap, sanitizer, debug_heap) for cap in caps]
    if suite == "golden":
        golden_options = ("--update-golden",) if arguments.update_golden else ()
        return [TestRun(suite, config, (f"--test-suite={GOLDEN_TEST_SUITE}",), (*gpu_options, *golden_options, *common),
                        sanitizer=sanitizer, debug_heap=debug_heap)]
    raise ValueError(f"no runs for suite '{suite}'")


def build_hint(config: str, project: str, arguments: argparse.Namespace) -> str:
    """How to build a missing executable: Build.py for the repository's bin/, the workspace of --build-root
    otherwise."""
    build_root = getattr(arguments, "build_root", paths.BIN_ROOT)
    if build_root.resolve() == paths.BIN_ROOT.resolve():
        return f"run python Scripts/Build.py --config {config} --project {project}"
    return (f"build the workspace whose bin/ is {paths.display_path(build_root)} (python Scripts/Build.py "
            f"--workspace-dir {paths.display_path(build_root.parent)} --config {config})")


def run_tests(run: TestRun, arguments: argparse.Namespace, console: Console) -> Step:
    executable = tests_executable(run.config, arguments.build_root)
    if not executable.is_file():
        return Step(run.name, Status.FAILED, f"{paths.display_path(executable)} not found: "
                                             f"{build_hint(run.config, 'Tests', arguments)}",
                    exit_code=EXIT_INIT_FAILED)
    try:
        environment = suite_environment(arguments)
    except FileNotFoundError as error:
        return missing_sanitizer_runtime(run.name, error)
    filters = list(run.filters)
    if arguments.filter:
        filters.append(f"--test-case={arguments.filter}")
    command = [str(executable), *filters, *run.options]
    report = junit_path(run.report_stem) if arguments.junit else None
    if report is not None:
        report.parent.mkdir(parents=True, exist_ok=True)
        report.unlink(missing_ok=True)
        command += ["--reporters=junit", f"--out={report}"]

    console.heading(f"{run.name}: {format_command([paths.display_path(executable), *filters, *run.options])}")
    try:
        result = run_streamed(command, cwd=paths.REPOSITORY_ROOT, env=environment, timeout=arguments.timeout,
                              echo=console.stream, collect=REPORTED_LINES_PATTERN, debug_heap=run.debug_heap)
    except ToolNotFoundError as error:
        return Step(run.name, Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED)

    data: dict[str, object] = {"suite": run.suite, "config": run.config, "executable": paths.display_path(executable)}
    if run.cap:
        data["vulkanApi"] = run.cap
    if run.sanitizer:
        data["sanitizer"] = run.sanitizer
    if run.debug_heap:
        data["debugHeap"] = True
    sanitizer_errors = [line for line in result.collected if SANITIZER_ERROR_PATTERN.search(line)]
    if sanitizer_errors:
        data["sanitizerErrors"] = sanitizer_errors
        for line in sanitizer_errors:
            console.print(f"SANITIZER: {line.strip()}")
    counts: TestCounts | None = None
    if report is not None:
        data["junit"] = paths.display_path(report)
        try:
            counts = read_junit(report, run.junit_suite_name)
        except (OSError, ElementTree.ParseError):
            write_crash_junit(report, run.junit_suite_name, f"Tests {result.describe_exit()} without a JUnit report",
                              result.tail(60))
    else:
        summary = DOCTEST_SUMMARY_PATTERN.findall(result.output)
        if summary:
            # doctest's "test cases: N" counts the executed cases; filtered-out ones are reported as skipped.
            executed, passed, failed, skipped = (int(value) for value in summary[-1])
            counts = TestCounts(executed, passed, failed, skipped)

    if counts is not None:
        data.update({"testCases": counts.executed, "passed": counts.passed, "failed": counts.failed,
                     "skipped": counts.skipped})
        if counts.failures:
            data["failures"] = counts.failures
            for failure in counts.failures:
                console.print(f"FAILED: {failure}")

    # Only the device suites report on the device: the unit suite's own harness tests check golden images of a device
    # class no machine has, on the CPU.
    device = DeviceReport.parse(result.collected) if run.suite != "unit" else DeviceReport()
    if run.suite != "unit":
        data["device"] = device.to_json()

    if result.timed_out:
        if report is not None:
            write_crash_junit(report, run.junit_suite_name, f"Tests {result.describe_exit()}", result.tail(60))
        return Step(run.name, Status.TIMEOUT, f"Tests {result.describe_exit()}", result.duration, data=data)

    skips: SkipCheck | None = None
    skip_error = ""
    try:
        skips = list_skipped_test_cases(executable, filters, environment)
        data["skipCheck"] = skips.to_json(arguments.allow_skips)
        if not arguments.allow_skips:
            for skipped_case in skips.unexpected:
                console.print(f"UNEXPECTED SKIP: {skipped_case}")
    except SkipCheckError as error:
        skip_error = f"cannot list the skipped test cases: {error}"
        data["skipCheck"] = {"error": skip_error, "allowed": arguments.allow_skips}
        console.print(skip_error)

    if result.exit_code != 0 or sanitizer_errors:
        if counts is not None and counts.failed:
            detail = f"{counts.failed} of {counts.executed} test case(s) failed"
        else:
            detail = f"Tests exited with code {describe_exit_code(result.exit_code)}"
        if sanitizer_errors:
            detail += f"; {describe_sanitizer_errors(sanitizer_errors)}"
        if device.mismatched:
            detail += f"; golden mismatches: {describe_names(device.mismatched)} (outputs in bin/TestResults/Golden/)"
        if device.class_changed:
            detail += (f"; device class {', '.join(sorted(device.device_classes))} has no goldens for "
                       f"{describe_names(device.class_changed)}, but a class of the same vendor has them: the driver's "
                       "major version changed (Tests/Golden/README.md, \"Renaming a device class\")")
        return Step(run.name, Status.FAILED, detail, result.duration, data=data)
    if counts is None:
        return Step(run.name, Status.FAILED, "Tests exited with code 0 but printed no doctest summary", result.duration,
                    data=data)
    if counts.executed == 0:
        return Step(run.name, Status.FAILED, "no test case ran" + (" (check --filter)" if arguments.filter else ""),
                    result.duration, data=data)
    passed = f"{counts.passed} test case(s) passed"
    if skips is None:
        if arguments.allow_skips:
            return Step(run.name, Status.PASSED, f"{passed}; {skip_error} (--allow-skips)", result.duration, data=data)
        return Step(run.name, Status.FAILED, skip_error, result.duration, data=data)
    child_targets = f"{len(skips.child_targets)} child-process target(s) in the {CHILD_TARGET_SUITE} suite"
    if skips.unexpected and not arguments.allow_skips:
        return Step(run.name, Status.FAILED, f"{len(skips.unexpected)} skipped test case(s) outside the "
                                             f"{CHILD_TARGET_SUITE} suite: {describe_unexpected_skips(skips)}; only "
                                             f"the {child_targets} may stay skipped (Roadmap rule 3; --allow-skips is "
                                             f"contract mode, for a contract checkpoint's targeted runs)",
                                             result.duration, data=data)
    if arguments.allow_skips:
        detail = (f"{passed}; contract mode (--allow-skips): {len(skips.skipped)} skipped, {len(skips.unexpected)} "
                  f"of them outside the {CHILD_TARGET_SUITE} suite; {child_targets}")
    elif skips.skipped:
        detail = f"{passed}; {len(skips.skipped)} skipped, all of them among the {child_targets}"
    else:
        detail = f"{passed}; none skipped"
    if device.matched:
        detail += (f"; {len(device.matched)} golden image(s) matched on device class "
                   f"{', '.join(sorted(device.device_classes))}")
    if device.host_copy_skips and run.cap != FULL_API_CAP:
        detail += f"; {device.describe_host_copy_skips()}, as expected under this cap"
    notes = device.notes(arguments.require_gpu, run.cap)
    if notes:
        return Step(run.name, Status.WARNING, f"{detail}; {'; '.join(notes)}", result.duration, data=data)
    return Step(run.name, Status.PASSED, detail, result.duration, data=data)


def editor_executable(config: str, build_root: Path = paths.BIN_ROOT) -> Path:
    """<build root>/<OutputDir>/Editor/Editor: the repository's bin/ by default, --build-root otherwise."""
    return build_root / paths.output_directory_name(config) / "Editor" / paths.executable_name("Editor")


def runtime_executable(config: str, build_root: Path = paths.BIN_ROOT) -> Path:
    """<build root>/<OutputDir>/Runtime/Runtime: the Runtime a testing export of `config` packages, which the export and
    games suites look up before they export."""
    return build_root / paths.output_directory_name(config) / "Runtime" / paths.executable_name("Runtime")


def venv_interpreter() -> Path:
    return MCP_VENV / ("Scripts/python.exe" if os.name == "nt" else "bin/python")


class CoverageError(Exception):
    """The registered methods could not be listed."""


def registered_methods(editor: Path) -> list[str]:
    """Every registered method except the test hooks, from the editor's own catalogue (--dump-reference writes
    Methods.json without the debug.* hooks, ADR 0008 decision 9)."""
    with tempfile.TemporaryDirectory(prefix="Test-Reference-") as directory:
        output = Path(directory) / "Reference"
        command = [str(editor), "--headless", "--renderer", "none", "--dump-reference", str(output),
                   f"--user-data-dir={Path(directory) / 'UserData'}"]
        try:
            result = run_captured(command, cwd=paths.REPOSITORY_ROOT, env=child_environment(),
                                  timeout=DUMP_REFERENCE_TIMEOUT_SECONDS)
        except ToolNotFoundError as error:
            raise CoverageError(str(error)) from error
        if not result.succeeded:
            raise CoverageError(f"{editor.name} --dump-reference {result.describe_exit()}: {result.tail(3)}")
        try:
            catalog = json.loads((output / "Methods.json").read_text(encoding="utf-8"))
            names = sorted(str(method["name"]) for method in catalog["Methods"])
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise CoverageError(f"cannot read the method catalogue of {format_command(command)}: {error}") from error
    if not names:
        raise CoverageError(f"{paths.display_path(editor)} registers no methods")
    return names


def append_coverage_junit(report: Path, suite_name: str, uncovered: list[str], registered: int) -> None:
    """Adds the coverage gate to the automation JUnit file as a test suite of its own."""
    tree = ElementTree.parse(report)
    root = tree.getroot()
    suite = ElementTree.SubElement(root, "testsuite", {"name": f"{suite_name}.coverage", "tests": "1",
                                                       "failures": "1" if uncovered else "0", "errors": "0",
                                                       "skipped": "0", "time": "0"})
    case = ElementTree.SubElement(suite, "testcase", {"classname": f"{suite_name}.coverage",
                                                      "name": COVERAGE_CASE_NAME, "time": "0"})
    if uncovered:
        failure = ElementTree.SubElement(case, "failure", {
            "message": f"{len(uncovered)} of {registered} registered method(s) have no Python test"})
        failure.text = "\n".join(uncovered)
    ElementTree.indent(root, space="\t")
    tree.write(report, encoding="utf-8", xml_declaration=True)


def run_automation(config: str, arguments: argparse.Namespace, console: Console) -> Step:
    name = f"automation {config}"
    editor = editor_executable(config)
    if not editor.is_file():
        return Step(name, Status.FAILED, f"{paths.display_path(editor)} not found: run python Scripts/Build.py "
                                         f"--config {config} --project Editor", exit_code=EXIT_INIT_FAILED)
    interpreter = venv_interpreter()
    if not interpreter.is_file():
        return Step(name, Status.FAILED, f"{paths.display_path(interpreter)} not found: run python Scripts/Setup.py "
                                         f"(it creates the MCP bridge's virtual environment the suite runs in)",
                    exit_code=EXIT_INIT_FAILED)
    suite_name = f"Automation.{config}"
    report = junit_path(f"Automation-{config}") if arguments.junit else None
    data: dict[str, object] = {"suite": "automation", "config": config, "editor": paths.display_path(editor)}

    with tempfile.TemporaryDirectory(prefix="Test-Automation-") as directory:
        coverage_file = Path(directory) / "CalledMethods.txt"
        runner_report = report if report is not None else Path(directory) / "Automation.xml"
        runner_report.parent.mkdir(parents=True, exist_ok=True)
        runner_report.unlink(missing_ok=True)
        command = [str(interpreter), str(UNITTEST_RUNNER), "--junit", str(runner_report), "--name", suite_name,
                   *(["--filter", arguments.filter] if arguments.filter else []),
                   *(str(test_directory) for test_directory in AUTOMATION_TEST_DIRECTORIES)]
        environment = child_environment({AUTOMATION_CONFIG_VARIABLE: config,
                                         AUTOMATION_COVERAGE_VARIABLE: str(coverage_file)})
        for variable in (*AUTOMATION_CLEARED_VARIABLES, AUTOMATION_REQUIRE_GPU_VARIABLE):
            environment.pop(variable, None)
        if arguments.require_gpu:
            environment[AUTOMATION_REQUIRE_GPU_VARIABLE] = "1"
        console.heading(f"automation ({config}): {paths.display_path(editor)}")
        try:
            result = run_streamed(command, cwd=paths.REPOSITORY_ROOT, env=environment, timeout=arguments.timeout,
                                  echo=console.stream, collect=NO_DEVICE_PATTERN)
        except ToolNotFoundError as error:
            return Step(name, Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED)
        no_device = DeviceReport.parse(result.collected).no_device
        data["noDevice"] = len(no_device)

        counts: TestCounts | None = None
        try:
            counts = read_junit(runner_report, None)
        except (OSError, ElementTree.ParseError):
            write_crash_junit(runner_report, suite_name, f"the automation suite {result.describe_exit()} without a "
                                                         f"JUnit report", result.tail(60))
        if report is not None:
            data["junit"] = paths.display_path(report)
        if counts is not None:
            data.update({"testCases": counts.executed, "passed": counts.passed, "failed": counts.failed,
                         "skipped": counts.skipped})
            if counts.failures:
                # The runner already streamed each traceback; the summary names the test and its message.
                summaries = [failure.split(" Traceback (most recent call last):", 1)[0].splitlines()[0]
                             for failure in counts.failures]
                data["failures"] = summaries
                for summary in summaries:
                    console.print(f"FAILED: {summary}")
        if result.timed_out:
            return Step(name, Status.TIMEOUT, f"the automation suite {result.describe_exit()}", result.duration,
                        data=data)

        coverage_detail = "method coverage not checked (--filter selects part of the suite)"
        uncovered: list[str] = []
        coverage_failure = ""
        if not arguments.filter:
            try:
                registered = registered_methods(editor)
                uncovered = coverage.uncovered_methods(registered, coverage.read_called_methods(coverage_file))
                data["coverage"] = {"registered": len(registered), "uncovered": uncovered}
                coverage_detail = f"method coverage {len(registered) - len(uncovered)}/{len(registered)}"
                if report is not None and counts is not None:
                    append_coverage_junit(report, suite_name, uncovered, len(registered))
                for method in uncovered:
                    console.print(f"UNCOVERED METHOD: {method}")
                if uncovered:
                    shown = ", ".join(uncovered[:UNCOVERED_IN_DETAIL])
                    more = len(uncovered) - UNCOVERED_IN_DETAIL
                    coverage_failure = (f"{len(uncovered)} registered method(s) have no Python test: {shown}"
                                        + (f" and {more} more" if more > 0 else ""))
            except CoverageError as error:
                coverage_failure = f"cannot check method coverage: {error}"
                data["coverage"] = {"error": str(error)}

    skips_allowed = result.exit_code == RUNNER_EXIT_SKIPPED and arguments.allow_skips
    if result.exit_code not in (0, RUNNER_EXIT_SKIPPED) or counts is None:
        if counts is not None and counts.failed:
            detail = f"{counts.failed} of {counts.executed} test(s) failed"
        else:
            detail = f"the automation suite exited with code {describe_exit_code(result.exit_code)}"
        if coverage_failure:
            detail += f"; {coverage_failure}"
        return Step(name, Status.FAILED, detail, result.duration, data=data)
    if result.exit_code == RUNNER_EXIT_SKIPPED and not skips_allowed:
        return Step(name, Status.FAILED, f"{counts.skipped} test(s) skipped: Python tests never skip (a missing editor "
                                         f"build or virtual environment is a failure; --allow-skips is contract "
                                         f"mode)", result.duration, data=data)
    if coverage_failure:
        return Step(name, Status.FAILED, coverage_failure, result.duration, data=data)
    skipped = f"; contract mode (--allow-skips): {counts.skipped} skipped" if skips_allowed else ""
    detail = f"{counts.passed} test(s) passed{skipped}; {coverage_detail}"
    if no_device:
        # Only without --require-gpu: with it, such a test fails (harness.require_gpu).
        return Step(name, Status.WARNING, f"{detail}; {len(no_device)} test(s) found no GPU device and passed without "
                                          f"running ({no_device[0]}); --require-gpu fails them instead",
                    result.duration, data=data)
    return Step(name, Status.PASSED, detail, result.duration, data=data)



# --------------------------------------------------------------------------------------------------------------------
# The feature suite (ADR 0022 decision 7)
# --------------------------------------------------------------------------------------------------------------------


def probe_rendering_editor(editor: Path, environment: dict[str, str] | None = None) -> str | None:
    """None when a headless editor can create its device here (`Editor --headless --frames 1` with the GPU test options
    exits 0); otherwise why not: its exit code and the last error line it printed (Tests/Automation/harness.py).
    `environment` is the suite's (default: child_environment())."""
    with tempfile.TemporaryDirectory(prefix="Test-GpuProbe-") as directory:
        command = [str(editor), "--headless", "--frames", "1", *GPU_PROBE_ARGUMENTS, f"--user-data-dir={directory}"]
        try:
            result = run_captured(command, cwd=paths.REPOSITORY_ROOT,
                                  env=environment if environment is not None else child_environment(),
                                  timeout=GPU_PROBE_TIMEOUT_SECONDS)
        except ToolNotFoundError as error:
            return f"the editor could not run its device probe: {error}"
    if result.succeeded:
        return None
    lines = [line.strip() for line in result.output.splitlines() if line.strip()]
    errors = [line for line in lines if "[error]" in line or "[critical]" in line]
    cause = (errors or lines or ["no output"])[-1]
    return f"a headless editor {result.describe_exit()} creating its device: {cause}"


def snapshot_project(root: Path) -> dict[str, str]:
    """The SHA-256 of every file below `root` outside its Library/ directory, by '/'-separated relative path."""
    snapshot: dict[str, str] = {}
    for directory, directories, files in os.walk(root):
        current = Path(directory)
        if current == root:
            directories[:] = [name for name in directories if name != PROJECT_CACHE_DIRECTORY]
        directories.sort()
        for name in sorted(files):
            path = current / name
            snapshot[path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return snapshot


def compare_snapshots(before: dict[str, str], after: dict[str, str]) -> list[str]:
    """One "<change>: <path>" line per file that a run changed, added or removed, sorted by path."""
    changes: list[str] = []
    for path in sorted(set(before) | set(after)):
        if path not in after:
            changes.append(f"removed: {path}")
        elif path not in before:
            changes.append(f"added: {path}")
        elif before[path] != after[path]:
            changes.append(f"changed: {path}")
    return changes


def describe_lines(lines: list[str]) -> str:
    shown = "; ".join(lines[:FAILURES_IN_DETAIL])
    more = len(lines) - FAILURES_IN_DETAIL
    return shown + (f" and {more} more" if more > 0 else "")


def feature_command(editor: Path, report: Path, user_data: Path) -> list[str]:
    """The feature suite's FeatureTest run: the committed project in editor mode, read strictly (Editor --strict:
    unknown fields and components in its .eproj, scenes and prefabs are errors, Architecture §6)."""
    return [str(editor), "--headless", "--project", str(FEATURE_PROJECT), "--run-tests", STRICT_EDITOR_ARGUMENT,
            "--audio-device", "none", "--expect-no-gpu-errors", "--junit", str(report), f"--user-data-dir={user_data}"]


def run_feature(config: str, arguments: argparse.Namespace, console: Console) -> Step:
    """FeatureTest in editor mode: the run, the six per-mode gates of its report, no skipped case and an unchanged
    project (ADR 0022 decision 7)."""
    sanitizer = getattr(arguments, "sanitizer", None)
    name = f"feature {config}{sanitizer_name_suffix(sanitizer)}"
    editor = editor_executable(config, arguments.build_root)
    if not editor.is_file():
        return Step(name, Status.FAILED, f"{paths.display_path(editor)} not found: "
                                         f"{build_hint(config, 'Editor', arguments)}", exit_code=EXIT_INIT_FAILED)
    try:
        environment = suite_environment(arguments)
    except FileNotFoundError as error:
        return missing_sanitizer_runtime(name, error)
    data: dict[str, object] = {"suite": "feature", "config": config, "editor": paths.display_path(editor),
                               "project": paths.display_path(FEATURE_PROJECT)}
    if sanitizer:
        data["sanitizer"] = sanitizer
    reason = probe_rendering_editor(editor, environment)
    if reason is not None:
        data["noDevice"] = reason
        if arguments.require_gpu:
            return Step(name, Status.FAILED, f"no usable Vulkan device (--require-gpu): {reason}", data=data)
        console.print(f"{NO_DEVICE_TEXT}{name}: {reason}")
        return Step(name, Status.WARNING, f"no usable Vulkan device, so FeatureTest passed without running ({reason}); "
                                          f"--require-gpu fails instead", data=data)

    with tempfile.TemporaryDirectory(prefix="Test-Feature-") as directory:
        stem = f"Feature-{config}{sanitizer_stem_suffix(sanitizer)}"
        report = junit_path(stem) if arguments.junit else Path(directory) / f"{stem}.xml"
        json_report = report.with_suffix(".json")
        report.parent.mkdir(parents=True, exist_ok=True)
        report.unlink(missing_ok=True)
        json_report.unlink(missing_ok=True)
        command = feature_command(editor, report, Path(directory) / "UserData")
        console.heading(f"{name}: {format_command([paths.display_path(editor), *command[1:]])}")
        before = snapshot_project(FEATURE_PROJECT)
        try:
            result = run_streamed(command, cwd=paths.REPOSITORY_ROOT, env=environment,
                                  timeout=arguments.timeout, echo=console.stream, collect=SANITIZER_ERROR_PATTERN)
        except ToolNotFoundError as error:
            return Step(name, Status.FAILED, str(error), exit_code=EXIT_INIT_FAILED, data=data)
        changes = compare_snapshots(before, snapshot_project(FEATURE_PROJECT))
        if result.collected:
            data["sanitizerErrors"] = result.collected
        if arguments.junit:
            data["junit"] = paths.display_path(report)
            data["json"] = paths.display_path(json_report)
        if result.timed_out:
            return Step(name, Status.TIMEOUT, f"the FeatureTest run {result.describe_exit()}", result.duration,
                        data=data)

        failures: list[str] = []
        if result.exit_code != EXIT_SUCCESS:
            failures.append(f"the editor {result.describe_exit()}")
        if result.collected:
            failures.append(describe_sanitizer_errors(result.collected))
        try:
            feature = coverage.read_feature_report(json_report)
            data.update({"cases": len(json.loads(json_report.read_text(encoding="utf-8"))["cases"]),
                         "passedRun": feature.passed})
            failures += coverage.evaluate_feature_report(feature)
        except ValueError as error:
            failures.append(f"no readable report: {error}")
        failures += [f"Projects/FeatureTest {change} outside Library/" for change in changes]
        if changes:
            data["changedFiles"] = changes
    for failure in failures:
        console.print(failure if failure.startswith("UNCOVERED ") else f"FAILED: {failure}")
    if failures:
        data["failures"] = failures
        uncovered = [line for line in failures if line.startswith("UNCOVERED ")]
        other = [line for line in failures if not line.startswith("UNCOVERED ")]
        parts = ([describe_lines(other)] if other else []) + ([f"{len(uncovered)} uncovered entr"
                                                               f"{'y' if len(uncovered) == 1 else 'ies'} "
                                                               f"({describe_lines(uncovered)})"] if uncovered else [])
        return Step(name, Status.FAILED, "; ".join(parts), result.duration, data=data)
    return Step(name, Status.PASSED, "FeatureTest passed with the six editor-mode gates evaluated and green; "
                                     "Projects/FeatureTest unchanged", result.duration, data=data)


# --------------------------------------------------------------------------------------------------------------------
# The export suite (ADR 0024 decision 9)
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass
class ExportedRun:
    """One configuration of the export suite: its step and its run's report (None when none could be read)."""

    step: Step
    report: coverage.FeatureReport | None = None


def tail_lines(path: Path, count: int) -> str:
    """The last `count` lines of the text file `path`, or why it cannot be read."""
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        return f"({paths.display_path(path)} cannot be read: {error})"
    return "\n".join(lines[-count:])


def describe_export_error(error: Any) -> str:
    """An engine_client.EngineError of project.export: its detail and each issue's pointer and message."""
    issues = [f"{issue.get('pointer', '') or '(root)'}: {issue.get('message', '')}" for issue in error.issues
              if isinstance(issue, dict)]
    return error.detail + (f" ({'; '.join(issues)})" if issues else "")


def install_package(output: Path, config: str, name: str = exports.FEATURE_TEST_NAME) -> Path:
    """Moves the export in `output` to exports.package_directory(config, name) (bin/Exports/<config>/<name>), replacing
    an earlier one; returns the package directory. Raises OSError when the earlier package cannot be removed or the move
    fails."""
    package = exports.package_directory(config, name)
    if package.exists():
        shutil.rmtree(package)
    package.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(output), str(package))
    return package


def check_testing_user_data(package: Path, name: str = exports.FEATURE_TEST_NAME, log_name: str = "") -> list[str]:
    """The user-data failures of a --feature-test run of the package of the game `name` in `package` (ADR 0024 decision
    9 step 4, the Dist half of "Export: user data goes to the manifest Name folder"): its log, <log_name>.log (`name`
    when empty), must be in exports.testing_user_data(package, name), and no folder named after the product may exist
    beside that one. The Runtime names its log after its executable's stem (Architecture §4.4: Logs/<exe>.log), which
    is the Name on Windows (<Name>.exe) but loses the part after a dot of the Name elsewhere, so a caller that knows the
    executable passes its stem as `log_name`."""
    user_data = exports.testing_user_data(package, name)
    failures: list[str] = []
    log = user_data / "Logs" / f"{log_name or name}.log"
    if not log.is_file():
        failures.append(f"the run wrote no {paths.display_path(log)}")
    product = user_data.parent / paths.workspace_name()
    if product.exists():
        failures.append(f"the run wrote user data under the product's name ({paths.display_path(product)})")
    return failures


def run_export_configuration(client: Any, engine_client: Any, config: str, arguments: argparse.Namespace,
                             console: Console, reports: Path) -> ExportedRun:
    """One configuration of the export suite on the editor `client` drives: the testing export, its package moved to
    bin/Exports/<config>/FeatureTest, the package's --feature-test run with its reports in `reports`/bin/TestResults,
    the run's user data, the evaluation of its report against the configuration's own mode and its counted
    Debug.Break."""
    name = f"export {config}"
    data: dict[str, object] = {"suite": "export", "config": config}
    started = time.monotonic()
    console.heading(f"{name}: project.export {{config: {config}, testing: true}}")
    try:
        result = client.call("project.export", {"config": config, "testing": True}, timeout=arguments.timeout)
    except engine_client.EngineError as error:
        return ExportedRun(Step(name, Status.FAILED, f"project.export failed: {describe_export_error(error)}",
                                time.monotonic() - started, data=data))
    except TimeoutError as error:
        return ExportedRun(Step(name, Status.TIMEOUT, f"project.export did not finish: {error}",
                                time.monotonic() - started, data=data))
    data.update({"files": len(result.get("files", [])), "testSuites": result.get("testSuiteCount"),
                 "replays": result.get("replayCount")})
    try:
        package = install_package(Path(result["outputDirectory"]), config)
    except OSError as error:
        destination = paths.display_path(exports.package_directory(config))
        return ExportedRun(Step(name, Status.FAILED, f"cannot move the export to {destination}: {error}",
                                time.monotonic() - started, data=data))
    executable = exports.package_executable(package)
    data["package"] = paths.display_path(package)
    if not executable.is_file():
        return ExportedRun(Step(name, Status.FAILED, f"the package holds no {paths.display_path(executable)}",
                                time.monotonic() - started, data=data))

    # The run's working directory decides where its reports go (ADR 0022 decision 7): <working>/bin/TestResults.
    json_report = reports / "bin" / "TestResults" / f"{exports.FEATURE_TEST_NAME}-{config}.json"
    json_report.unlink(missing_ok=True)
    json_report.with_suffix(".xml").unlink(missing_ok=True)
    command = [str(executable), "--headless", "--feature-test",
               *(["--expect-no-gpu-errors"] if config != "Dist" else [])]
    console.heading(f"{name}: {format_command([paths.display_path(executable), *command[1:]])}")
    try:
        run = run_streamed(command, cwd=reports, env=child_environment(), timeout=arguments.timeout,
                           echo=console.stream)
    except ToolNotFoundError as error:
        return ExportedRun(Step(name, Status.FAILED, str(error), time.monotonic() - started, data=data))
    data["report"] = paths.display_path(json_report)
    log = exports.testing_user_data(package) / "Logs" / f"{exports.FEATURE_TEST_NAME}.log"
    if run.timed_out:
        console.print(tail_lines(log, LOG_TAIL_LINES))
        return ExportedRun(Step(name, Status.TIMEOUT, f"the {config} FeatureTest run {run.describe_exit()}",
                                time.monotonic() - started, data=data))

    failures: list[str] = []
    if run.exit_code != EXIT_SUCCESS:
        failures.append(f"the {config} FeatureTest run {run.describe_exit()}")
        console.print(f"The end of {paths.display_path(log)}:")
        console.print(tail_lines(log, LOG_TAIL_LINES))
    failures += check_testing_user_data(package)
    report: coverage.FeatureReport | None = None
    try:
        report = coverage.read_feature_report(json_report)
        failures += coverage.evaluate_feature_report(report, expected_mode=config)
        failures += exports.debug_break_failures(json_report)
    except ValueError as error:
        failures.append(f"no readable report: {error}")
    duration = time.monotonic() - started
    for failure in failures:
        console.print(failure if failure.startswith("UNCOVERED ") else f"FAILED: {failure}")
    if failures:
        data["failures"] = failures
        uncovered = [line for line in failures if line.startswith("UNCOVERED ")]
        other = [line for line in failures if not line.startswith("UNCOVERED ")]
        parts = ([describe_lines(other)] if other else []) + ([f"{len(uncovered)} uncovered entr"
                                                               f"{'y' if len(uncovered) == 1 else 'ies'} "
                                                               f"({describe_lines(uncovered)})"] if uncovered else [])
        return ExportedRun(Step(name, Status.FAILED, "; ".join(parts), duration, data=data), report)
    return ExportedRun(Step(name, Status.PASSED, f"the testing export ({data['testSuites']} suite(s), "
                                                 f"{data['replays']} replay(s)) passed --feature-test with the six "
                                                 f"{config}-mode gates evaluated and green; user data in "
                                                 f"{paths.display_path(exports.testing_user_data(package))}",
                            duration, data=data), report)


def run_export_merge(runs: dict[str, ExportedRun], arguments: argparse.Namespace, console: Console) -> Step:
    """The cross-mode union merge of the Editor report and both exported runs' (coverage.merge_coverage, §15.6), or why
    it cannot run: one configuration only (skipped), an Editor report that is missing or stale (exit code 3, never
    rerun here, ADR 0023)."""
    missing = [config for config in exports.EXPORTED_CONFIGURATIONS if config not in runs]
    if missing:
        return Step(MERGE_STEP_NAME, Status.SKIPPED, f"the merge needs the Editor, Release and Dist reports, and this "
                                                     f"run exported no {', '.join(missing)}")
    editor_report = arguments.editor_report or DEFAULT_EDITOR_REPORT
    data: dict[str, object] = {"suite": "export", "editorReport": paths.display_path(editor_report)}
    reason = exports.stale_report_reason(editor_report, exports.report_inputs("Release", "editor"))
    if reason is not None:
        return Step(MERGE_STEP_NAME, Status.FAILED, f"{reason}: run python Scripts/Test.py --suite feature --config "
                                                    f"Release --junit first", exit_code=EXIT_INIT_FAILED, data=data)
    reports: list[coverage.FeatureReport | None] = []
    try:
        reports.append(coverage.read_feature_report(editor_report))
    except ValueError as error:
        console.print(f"Test.py: the Editor report cannot be used: {error}")
        reports.append(None)
    reports += [runs[config].report for config in exports.EXPORTED_CONFIGURATIONS]
    readable = [report for report in reports if report is not None]
    failures = (unreadable_report_failures(reports) if len(readable) != len(reports)
                else coverage.merge_coverage(readable))
    for failure in failures:
        console.print(failure)
    if failures:
        data["failures"] = failures
        return Step(MERGE_STEP_NAME, Status.FAILED, f"{len(failures)} merge failure(s): {describe_lines(failures)}",
                    data=data)
    return Step(MERGE_STEP_NAME, Status.PASSED, "the union of the Editor, Release and Dist runs covers every "
                                                "registered entry and every run passed", data=data)


def run_export(configs: list[str], arguments: argparse.Namespace, console: Console) -> list[Step]:
    """The export suite (ADR 0024 decision 9): FeatureTest's testing export in each of `configs` from one Release editor
    on a copy of the project, each package's --feature-test run and evaluation, then the coverage merge."""
    editor = editor_executable(exports.EXPORTING_EDITOR_CONFIGURATION)
    names = [f"export {config}" for config in configs]
    if not editor.is_file():
        return [Step(name, Status.FAILED, f"{paths.display_path(editor)} not found: run python Scripts/Build.py "
                                          f"--config {exports.EXPORTING_EDITOR_CONFIGURATION} --project Editor",
                     exit_code=EXIT_INIT_FAILED) for name in names]
    for config in configs:
        runtime = runtime_executable(config)
        if not runtime.is_file():
            return [Step(f"export {config}", Status.FAILED, f"{paths.display_path(runtime)} not found: run python "
                                                            f"Scripts/Build.py --config {config} --project Runtime",
                         exit_code=EXIT_INIT_FAILED)]
    steps: list[Step]
    reason = probe_rendering_editor(editor)
    if reason is not None:
        data = {"suite": "export", "noDevice": reason}
        if arguments.require_gpu:
            steps = [Step(name, Status.FAILED, f"no usable Vulkan device (--require-gpu): {reason}", data=data)
                     for name in names]
        else:
            console.print(f"{NO_DEVICE_TEXT}export: {reason}")
            steps = [Step(name, Status.WARNING, f"no usable Vulkan device, so the testing export passed without "
                                                f"running ({reason}); --require-gpu fails instead", data=data)
                     for name in names]
        steps.append(Step(MERGE_STEP_NAME, Status.SKIPPED, "nothing was exported"))
    else:
        steps = drive_exports(editor, configs, arguments, console)
    if arguments.junit:
        report = junit_path(EXPORT_REPORT_STEM)
        write_junit(report, EXPORT_REPORT_STEM, steps)
        for step in steps:
            step.data["junit"] = paths.display_path(report)
    return steps


def drive_exports(editor: Path, configs: list[str], arguments: argparse.Namespace, console: Console) -> list[Step]:
    """run_export's work with a device: the Release editor on a copy of FeatureTest with its Library/, one
    run_export_configuration per configuration, the editor always stopped, then the merge."""
    engine_client = import_engine_client()
    runs: dict[str, ExportedRun] = {}
    with tempfile.TemporaryDirectory(prefix="Test-Export-") as directory:
        work = Path(directory)
        # With --junit the runs write bin/TestResults/FeatureTest-<Config>.* of the repository (ADR 0022 decision 7).
        reports = paths.REPOSITORY_ROOT if arguments.junit else work / "Reports"
        reports.mkdir(parents=True, exist_ok=True)
        try:
            project_file = copy_project(FEATURE_PROJECT, work / "FeatureTest", keep_cache=True)
            console.heading(f"export: {paths.display_path(editor)} on a copy of {paths.display_path(FEATURE_PROJECT)}")
            process = engine_client.launch_editor(editor, list(EXPORT_EDITOR_ARGUMENTS), work / "UserData",
                                                  paths.workspace_name(), environment=child_environment())
        except (RuntimeError, TimeoutError, ValueError, OSError) as error:
            return [Step(f"export {', '.join(configs)}", Status.FAILED,
                         f"the exporting editor could not start: {error}")]
        try:
            client = process.connect("export", call_log=engine_client.CallLog(None))
            with client:
                client.call("project.open", {"path": str(project_file.parent)}, timeout=arguments.timeout)
                for config in configs:
                    runs[config] = run_export_configuration(client, engine_client, config, arguments, console, reports)
                try:
                    client.call("session.shutdown", {"force": True}, timeout=EDITOR_SHUTDOWN_TIMEOUT_SECONDS)
                except (engine_client.EngineError, engine_client.ConnectionClosed, TimeoutError):
                    pass  # the process is stopped below either way
            process.wait(EDITOR_SHUTDOWN_TIMEOUT_SECONDS)
        except (engine_client.EngineError, engine_client.ConnectionClosed, engine_client.ProtocolError, TimeoutError,
                OSError) as error:
            done = [run.step for run in runs.values()]
            return done + [Step(f"export {config}", Status.FAILED, f"the exporting editor failed: {error}\n"
                                                                   f"{process.output()}")
                           for config in configs if config not in runs]
        finally:
            process.kill()
        steps = [runs[config].step for config in configs]
        steps.append(run_export_merge(runs, arguments, console))
    return steps


# --------------------------------------------------------------------------------------------------------------------
# The determinism suite (ADR 0022 decision 7, §15.1 T3d)
# --------------------------------------------------------------------------------------------------------------------


def glob_regex(pattern: str) -> re.Pattern[str]:
    """The Testing.Replays glob rules of MatchesPathGlob (Engine/Core/PathGlob.h), which the editor, the Runtime and
    testing exports share: '*' any characters but '/', '?' one character but '/', "**" any characters ("**/" also no
    directory at all). Both run the vectors of Tests/Data/PathGlob/Cases.json (ADR 0024 decision 7)."""
    parts: list[str] = []
    index = 0
    while index < len(pattern):
        if pattern.startswith("**/", index):
            parts.append("(?:.*/)?")
            index += 3
        elif pattern.startswith("**", index):
            parts.append(".*")
            index += 2
        elif pattern[index] == "*":
            parts.append("[^/]*")
            index += 1
        elif pattern[index] == "?":
            parts.append("[^/]")
            index += 1
        else:
            parts.append(re.escape(pattern[index]))
            index += 1
    return re.compile("".join(parts) + r"\Z")


def expand_replay_globs(project: Path, globs: list[str]) -> list[str]:
    """The project-relative .replay files below Assets/ that `globs` (Testing.Replays) match, sorted and unique, as the
    editor expands them. Raises ValueError for a glob without wildcards that names no replay."""
    assets = project / "Assets"
    files = sorted(path.relative_to(project).as_posix() for path in assets.rglob("*.replay") if path.is_file()) \
        if assets.is_dir() else []
    matched: set[str] = set()
    for glob in globs:
        pattern = glob[len("project://"):] if glob.startswith("project://") else glob
        expression = glob_regex(pattern)
        hits = [path for path in files if expression.match(path)]
        if not hits and not any(character in glob for character in "*?"):
            raise ValueError(f"the replay '{glob}' of Testing.Replays is missing")
        matched.update(hits)
    return sorted(matched)


@dataclasses.dataclass
class ReplayOutcome:
    """One replay verified by one editor: its observed state hash (empty when none) and why it failed."""

    path: str
    state_hash: str = ""
    problems: list[str] = dataclasses.field(default_factory=list)


def copy_project(source: Path, destination: Path, keep_cache: bool = False) -> Path:
    """Copies the project `source` to `destination`, without its Build/ directory (exports, never project input) and
    without its Library/ directory unless `keep_cache` (the import cache, which then saves the copy's editor the
    imports, without the project lock file of an editor that may hold the source); returns its project file."""
    cache = source / PROJECT_CACHE_DIRECTORY
    skipped = {PROJECT_BUILD_DIRECTORY} | (set() if keep_cache else {PROJECT_CACHE_DIRECTORY})

    def ignored(directory: str, names: list[str]) -> list[str]:
        if Path(directory) == source:
            return [name for name in names if name in skipped]
        if Path(directory) == cache:
            return [name for name in names if name == PROJECT_LOCK_FILE]
        return []

    shutil.copytree(source, destination, ignore=ignored)
    files = sorted(destination.glob("*.eproj"))
    if len(files) != 1:
        raise ValueError(f"{paths.display_path(source)} holds {len(files)} project files, not one")
    return files[0]


def verify_replays(editor: Path, project_file: Path, replays: list[str], user_data: Path,
                   timeout: float) -> list[ReplayOutcome]:
    """Starts `editor --headless --renderer none --audio-device none --automation --strict` on `project_file` and runs
    input.replay {path, verify: true, strictHash: true} for each of `replays`, in order, then the input check of each
    replay with input events (input_check_failures). Each outcome holds the observed stateHash and any failure; the
    editor is always stopped. Raises RuntimeError when the editor cannot be started or reached, and ValueError when a
    replay file cannot be read."""
    engine_client = import_engine_client()
    events = games.replay_event_counts(project_file.parent, replays)
    process = engine_client.launch_editor(editor, [*DETERMINISM_EDITOR_ARGUMENTS, "--project", str(project_file)],
                                          user_data, paths.workspace_name(), environment=child_environment())
    outcomes: list[ReplayOutcome] = []
    try:
        client = process.connect("determinism", call_log=engine_client.CallLog(None))
        with client:
            outcomes = replay_outcomes(client, engine_client, replays, timeout)
            checked = input_check_failures(client, engine_client, events, timeout)
            for outcome in outcomes:
                outcome.problems += checked.get(outcome.path, [])
            try:
                client.call("session.shutdown", {"force": True}, timeout=EDITOR_SHUTDOWN_TIMEOUT_SECONDS)
            except (engine_client.EngineError, engine_client.ConnectionClosed, TimeoutError):
                pass  # the process is stopped below either way
        process.wait(EDITOR_SHUTDOWN_TIMEOUT_SECONDS)
    except (engine_client.ConnectionClosed, engine_client.ProtocolError, TimeoutError, OSError) as error:
        raise RuntimeError(f"{editor.name} on {paths.display_path(project_file)}: {error}\n"
                           f"{process.output()}") from error
    finally:
        process.kill()
    return outcomes


def replay_outcomes(client: Any, engine_client: Any, replays: list[str], timeout: float) -> list[ReplayOutcome]:
    """input.replay {path, verify: true, strictHash: true} for each of `replays`, in order, on the open session of
    `client` (an engine_client.EngineClient; `engine_client` is the module, for its EngineError): each outcome holds the
    observed stateHash and any failure (a refused or failed verification, a replay stopped before its final tick).
    Connection errors (engine_client.ConnectionClosed, ProtocolError, TimeoutError) propagate to the caller."""
    outcomes: list[ReplayOutcome] = []
    for path in replays:
        outcome = ReplayOutcome(path)
        try:
            result = client.call("input.replay", {"path": path, "verify": True, "strictHash": True}, timeout=timeout)
        except engine_client.EngineError as error:
            result = error.data.get("replay") if isinstance(error.data.get("replay"), dict) else {}
            outcome.problems.append(f"input.replay failed: {error.detail}")
        outcome.state_hash = str(result.get("stateHash", ""))
        if result and not result.get("passed", False) and not outcome.problems:
            outcome.problems.append("verification failed")
        if result.get("stopReason"):
            outcome.problems.append(f"stopped before its final tick: {result['stopReason']}")
        outcomes.append(outcome)
    return outcomes


def input_check_failures(client: Any, engine_client: Any, events: dict[str, int],
                         timeout: float) -> dict[str, list[str]]:
    """The input check of the replays in `events` ({project-relative path: input event count}) that hold input events
    (M16 friction S2-01): each is verified again with input.replay {verify: true, strictHash: true, withoutInput: true}
    on the open session of `client`, and that verification must fail (games.evaluate_input_check), since a replay whose
    outcome does not depend on its input proves nothing about its gameplay. Returns {replay: failure lines} for the
    replays that fail it; afterwards no play session is left (play.stop, a failure naming the first replay checked when
    it is refused). Connection errors propagate to the caller."""
    failures: dict[str, list[str]] = {}
    checked = [replay for replay, count in events.items() if count > 0]
    for replay in checked:
        params = {"path": replay, "verify": True, "strictHash": True, "withoutInput": True}
        try:
            result = client.call("input.replay", params, timeout=timeout)
            lines = games.evaluate_input_check(replay, events[replay], result, None)
        except engine_client.EngineError as error:
            lines = games.evaluate_input_check(replay, events[replay], None, (error.error_code, error.detail))
        if lines:
            failures[replay] = lines
    if checked and client.call("play.state", {}, timeout=timeout).get("state") != "Edit":
        try:
            client.call("play.stop", {}, timeout=timeout)
        except engine_client.EngineError as error:
            failures.setdefault(checked[0], []).append(f"play.stop after the input check failed: {error.detail}")
    return failures


def determinism_problems(committed: dict[str, str], outcomes: dict[str, list[ReplayOutcome]],
                         exported: dict[str, dict[str, str]]) -> dict[str, list[str]]:
    """The problems of every committed replay (committed FinalStateHash by path), by path: each editor run's own
    verification problems ("<run>: <problem>", `outcomes` by run label), then exports.compare_determinism_runs over the
    hashes of the editor runs and the exported runs (`exported`: {run label: {path: hash}}), and for a replay with any
    problem a last line naming every run's hash ("hashes: <run> <hash>, ..."), so a failure reads on its own."""
    runs: dict[str, dict[str, str]] = {run: {outcome.path: outcome.state_hash for outcome in results
                                             if outcome.state_hash}
                                       for run, results in outcomes.items()}
    runs.update(exported)
    compared = exports.compare_determinism_runs(committed, runs)
    ordered = ([run for run in exports.DETERMINISM_RUNS if run in runs]
               + sorted(set(runs) - set(exports.DETERMINISM_RUNS)))
    problems: dict[str, list[str]] = {}
    for path in sorted(committed):
        lines = [f"{run}: {problem}" for run, results in outcomes.items() for outcome in results
                 if outcome.path == path for problem in outcome.problems]
        lines += compared[path]
        if lines:
            lines.append("hashes: " + ", ".join(f"{run} {runs[run].get(path) or '(none)'}" for run in ordered))
        problems[path] = lines
    return problems


def import_engine_client() -> Any:
    """Tools/Automation/engine_client.py, the standard-library automation client (Architecture §13.2)."""
    tools = str(paths.REPOSITORY_ROOT / "Tools" / "Automation")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import engine_client  # noqa: PLC0415  (Tools/Automation is on sys.path only from here)
    return engine_client


def write_determinism_junit(path: Path, problems: dict[str, list[str]]) -> None:
    root = ElementTree.Element("testsuites")
    failures = sum(1 for lines in problems.values() if lines)
    suite = ElementTree.SubElement(root, "testsuite", {"name": DETERMINISM_REPORT_STEM, "tests": str(len(problems)),
                                                       "failures": str(failures), "errors": "0", "skipped": "0"})
    for replay in sorted(problems):
        case = ElementTree.SubElement(suite, "testcase", {"classname": DETERMINISM_REPORT_STEM, "name": replay})
        if problems[replay]:
            ElementTree.SubElement(case, "failure", {"message": problems[replay][0]}).text = "\n".join(problems[replay])
    ElementTree.indent(root, space="\t")
    path.parent.mkdir(parents=True, exist_ok=True)
    ElementTree.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)


def read_exported_runs(configs: list[str]) -> tuple[dict[str, dict[str, str]], dict[str, str]]:
    """The exported runs of `configs` (Release, Dist) that the export suite of this tree left: ({run label: {replay
    path: hash}} of every current report, {run label: why it is not compared} for every missing, stale or unreadable
    one; exports.stale_report_reason over exports.report_inputs, ADR 0024 decision 10)."""
    runs: dict[str, dict[str, str]] = {}
    reasons: dict[str, str] = {}
    for config in configs:
        label = f"Exported {config}"
        report = paths.TEST_RESULTS_ROOT / f"{exports.FEATURE_TEST_NAME}-{config}.json"
        reason = exports.stale_report_reason(report, exports.report_inputs(config, "exported"))
        if reason is None:
            try:
                runs[label] = exports.read_replay_hashes(report)
                continue
            except ValueError as error:
                reason = str(error)
        reasons[label] = reason
    return runs, reasons


def run_determinism(configs: list[str], arguments: argparse.Namespace, console: Console) -> Step:
    """Every FeatureTest replay in the runs `configs` select (exports.DETERMINISM_RUNS: the Debug and Release editors,
    the exported Release and Dist runs' reports), compared with its committed hash and across the runs (one step, ADR
    0022 decision 7, ADR 0024 decision 10)."""
    name = f"determinism {', '.join(configs)}"
    data: dict[str, object] = {"suite": "determinism", "configs": configs}
    editors = {config: editor_executable(config) for config in configs if config in DEVELOPMENT_CONFIGURATIONS}
    exported_configs = [config for config in configs if config in exports.EXPORTED_CONFIGURATIONS]
    for config, editor in editors.items():
        if not editor.is_file():
            return Step(name, Status.FAILED, f"{paths.display_path(editor)} not found: run python Scripts/Build.py "
                                             f"--config {config} --project Editor", exit_code=EXIT_INIT_FAILED)
    started = time.monotonic()
    try:
        project = json.loads(FEATURE_PROJECT_FILE.read_text(encoding="utf-8"))
        globs = project.get("Testing", {}).get("Replays", [])
        if not isinstance(globs, list) or not all(isinstance(glob, str) for glob in globs):
            raise ValueError("Testing.Replays must be a list of strings")
        replays = expand_replay_globs(FEATURE_PROJECT, globs)
        committed: dict[str, str] = {}
        for replay in replays:
            document = json.loads((FEATURE_PROJECT / replay).read_text(encoding="utf-8"))
            committed[replay] = str(document.get("FinalStateHash", ""))
    except (OSError, ValueError, AttributeError) as error:
        return Step(name, Status.FAILED, f"cannot read the replays of {paths.display_path(FEATURE_PROJECT_FILE)}: "
                                         f"{error}", data=data)
    data["replays"] = replays
    if not replays:
        if arguments.junit:
            write_determinism_junit(junit_path(DETERMINISM_REPORT_STEM), {})
        return Step(name, Status.WARNING, f"Testing.Replays of {paths.display_path(FEATURE_PROJECT_FILE)} matches no "
                                          f"replay, so nothing was verified", data=data)

    # The exported runs come from the export suite's reports of this tree, never rerun (ADR 0023).
    exported, not_compared = read_exported_runs(exported_configs)
    if not_compared:
        data["notCompared"] = not_compared
        if arguments.require_gpu:
            label, reason = next(iter(not_compared.items()))
            return Step(name, Status.FAILED, f"{label} has no current report ({reason}): run python Scripts/Test.py "
                                             f"--suite export --config {','.join(exported_configs)} --junit",
                        exit_code=EXIT_INIT_FAILED, data=data)

    outcomes: dict[str, list[ReplayOutcome]] = {}
    for config, editor in editors.items():
        label = f"Editor {config}"
        console.heading(f"determinism {label}: {len(replays)} replay(s) in {paths.display_path(editor)} "
                        f"(--renderer none)")
        with tempfile.TemporaryDirectory(prefix=f"Test-Determinism-{config}-") as directory:
            try:
                project_file = copy_project(FEATURE_PROJECT, Path(directory) / "FeatureTest")
                outcomes[label] = verify_replays(editor, project_file, replays, Path(directory) / "UserData",
                                                 arguments.timeout)
            except (RuntimeError, ValueError, OSError) as error:
                return Step(name, Status.FAILED, f"the {config} editor could not verify the replays: {error}",
                            data=data)
        for outcome in outcomes[label]:
            console.print(f"  {label} {outcome.path}: {outcome.state_hash or '(no hash)'}"
                          + (f" ({'; '.join(outcome.problems)})" if outcome.problems else ""))
    for label, hashes in exported.items():
        for replay in replays:
            console.print(f"  {label} {replay}: {hashes.get(replay) or '(no hash)'}")
    problems = determinism_problems(committed, outcomes, exported)
    data["hashes"] = {**{label: {outcome.path: outcome.state_hash for outcome in results}
                         for label, results in outcomes.items()}, **exported}
    if arguments.junit:
        report = junit_path(DETERMINISM_REPORT_STEM)
        write_determinism_junit(report, problems)
        data["junit"] = paths.display_path(report)
    duration = time.monotonic() - started
    failed = {replay: lines for replay, lines in problems.items() if lines}
    for replay, lines in sorted(failed.items()):
        for line in lines:
            console.print(f"FAILED: {replay}: {line}")
    if failed:
        data["failures"] = failed
        first = sorted(failed)[0]
        return Step(name, Status.FAILED, f"{len(failed)} of {len(replays)} replay(s) failed: {first}: "
                                         f"{failed[first][0]}", duration, data=data)
    runs = [*outcomes, *exported]
    compared = f" in {', '.join(runs)}" + (", which agree" if len(runs) > 1 else "")
    detail = f"{len(replays)} replay(s) matched their committed FinalStateHash{compared}" if runs \
        else f"no run of {len(replays)} replay(s) was compared"
    if not_compared:
        reasons = "; ".join(f"{label}: {reason}" for label, reason in not_compared.items())
        return Step(name, Status.WARNING, f"{detail}; not compared ({reasons}): run python Scripts/Test.py --suite "
                                          f"export --junit first, and --require-gpu fails instead", duration, data=data)
    return Step(name, Status.PASSED, detail, duration, data=data)


# --------------------------------------------------------------------------------------------------------------------
# The games suite (Docs/Decisions/0026-m16-contract.md decisions 7 and 8, §15.1 T5)
# --------------------------------------------------------------------------------------------------------------------


def games_audit_step(project: games.GameProject, console: Console) -> Step:
    """"games <Name> audit": provenance.audit_project of the project's staged files (index mode, the acceptance mode);
    it needs neither a build nor a device. A failure names its findings ("<code> <path>"; the console and the step's
    "audit" data list every one) with exit code 1, or 3 when a required file is missing or git fails."""
    name = f"games {project.name} audit"
    data: dict[str, object] = {"suite": "games", "project": paths.display_path(project.directory)}
    started = time.monotonic()
    console.heading(f"{name}: the provenance audit of the staged {paths.display_path(project.directory)}")
    try:
        audit = provenance.audit_project(project.directory, "index")
    except (RuntimeError, ValueError) as error:
        return Step(name, Status.FAILED, f"the provenance audit could not run: {error}", time.monotonic() - started,
                    exit_code=EXIT_INIT_FAILED, data=data)
    data["audit"] = provenance.audit_to_json(audit)
    for warning in audit.warnings:
        console.print(f"  warning {warning.code}: {warning.path}: {warning.detail}")
    for finding in audit.findings:
        console.print(f"  {finding.code}: {finding.path}: {finding.detail}")
    duration = time.monotonic() - started
    counts = audit.counts
    if not audit.findings:
        return Step(name, Status.PASSED, f"index: {counts.get('files', 0)} engine-consumed file(s), "
                                         f"{counts.get('entries', 0)} entries, {counts.get('transcriptLines', 0)} "
                                         f"transcript line(s) ({counts.get('requests', 0)} requests), "
                                         f"{len(audit.warnings)} warning(s)", duration, data=data)
    named = "; ".join(f"{finding.code} {finding.path or '(the project)'}"
                      for finding in audit.findings[:GAMES_FINDINGS_IN_DETAIL])
    more = len(audit.findings) - GAMES_FINDINGS_IN_DETAIL
    return Step(name, Status.FAILED, f"{len(audit.findings)} provenance finding(s): {named}"
                                     + (f" and {more} more" if more > 0 else ""),
                duration, exit_code=provenance.audit_exit_code(audit), data=data)


def git_toplevel(directory: Path) -> Path:
    """The work tree root of the git repository that holds `directory` (git rev-parse --show-toplevel). Raises
    RuntimeError naming the command and its output when git is missing or fails."""
    command = ["git", "-C", str(directory), "rev-parse", "--show-toplevel"]
    try:
        result = run_captured(command, cwd=paths.REPOSITORY_ROOT, env=child_environment(), timeout=GIT_TIMEOUT_SECONDS)
    except ToolNotFoundError as error:
        raise RuntimeError(f"{format_command(command)} could not run: {error}") from error
    if not result.succeeded:
        raise RuntimeError(f"{format_command(command)} {result.describe_exit()}: {result.tail(3)}")
    return Path(result.output.strip()).resolve()


def materialize_game(project: games.GameProject, destination: Path) -> Path:
    """The staged files of `project` as a fresh clone of the commit about to be made holds them
    (provenance.materialize_index below `destination`: no Library/, no Build/, no untracked or ignored file); returns
    the copy's project directory. Raises RuntimeError when git fails and ValueError when the index holds no project
    file."""
    copy = provenance.materialize_index(git_toplevel(project.directory), project.directory.resolve(), destination)
    if not (copy / project.project_file.name).is_file():
        raise ValueError(f"the index holds no {paths.display_path(project.project_file)}")
    return copy


def game_replay_hashes(document: Any) -> dict[str, str]:
    """{replay path: finalStateHash} of the "replay:<path>" suite rows of a TestRunResult document that carry a hash,
    as exports.read_replay_hashes reads them from a report file."""
    rows = document.get("suites") if isinstance(document, dict) else None
    hashes: dict[str, str] = {}
    for row in rows if isinstance(rows, list) else []:
        if not isinstance(row, dict) or not isinstance(row.get("suite"), str):
            continue
        state_hash = row.get("finalStateHash")
        if row["suite"].startswith(games.REPLAY_SUITE_PREFIX) and isinstance(state_hash, str) and state_hash:
            hashes[row["suite"][len(games.REPLAY_SUITE_PREFIX):]] = state_hash
    return hashes


def failed_with(step: Step, line: str, status: Status = Status.FAILED) -> Step:
    """`step` failed with `line` added to its failures: a session check that came after the step was decided (the
    exit code of the editor that also ran it, a refused session.shutdown, a broken connection or a timeout). A step
    that had not failed takes `status`; one that had keeps its own."""
    failures = [*step.data.get("failures", []), line] if step.failed else [line]
    data = {**step.data, "failures": failures}
    return Step(step.name, step.status if step.failed else status, describe_lines(failures), step.duration,
                exit_code=step.exit_code, data=data)


def failed_with_lines(step: Step, lines: list[str], status: Status = Status.FAILED) -> Step:
    """failed_with for each of `lines` in order; `step` unchanged when there is none."""
    for line in lines:
        step = failed_with(step, line, status)
    return step


@dataclasses.dataclass
class GameCopy:
    """The staged copy one editor session of a game runs on, and what the games suite reads from it."""

    directory: Path  # the copy's project directory (materialize_game)
    project_file: Path
    testing: games.TestingSettings
    matched: list[str]  # the project-relative replays Testing.Replays matches (expand_replay_globs)
    committed: dict[str, str]  # their committed FinalStateHash
    events: dict[str, int] = dataclasses.field(default_factory=dict)  # their input event counts (the input check)


def check_game_in_editor(client: Any, engine_client: Any, project: games.GameProject, config: str, copy: GameCopy,
                         hashes: dict[str, dict[str, str]], data: dict[str, object], arguments: argparse.Namespace,
                         console: Console) -> list[str]:
    """The editor half's checks on the open session (decision 8 step 2), as failure lines: first the build settings of
    the copy's project file (games.check_build_settings: StartScene and Export.BuildScenes set, and Export.BuildScenes
    listing StartScene, Docs/Decisions/0027-m17-contract.md decision 4, B1; its lines come first, a project file it
    cannot read is one line naming the file, and the other checks run either way), then script.check and
    project.validate clean, the upgrade dry run with no changed file, test.run {} evaluated with games.evaluate_run
    in Editor mode (its replay hashes go into `hashes` as "Editor <config>", and with --junit the document into
    games.editor_report_path), at least one committed replay with input events (games.check_replay_acceptance), the
    input check of every committed replay with input events (input_check_failures: its verification without them must
    fail, M16 friction S2-01), and no replay below Assets/ that Testing.Replays leaves out."""
    console.heading(f"games {project.name} Editor {config}: the build settings of {copy.project_file.name}")
    try:
        failures = games.check_build_settings(copy.project_file)
    except ValueError as error:
        failures = [f"the build settings cannot be checked: {error}"]

    def call(method: str, params: dict[str, Any]) -> dict[str, Any] | None:
        console.heading(f"games {project.name} Editor {config}: {method}")
        try:
            return client.call(method, params, timeout=arguments.timeout)
        except engine_client.EngineError as error:
            failures.append(f"{method} failed: {describe_export_error(error)}")
            return None

    checked = call("script.check", {})
    if checked is not None:
        failures += games.check_script_check(checked)
    validated = call("project.validate", {})
    if validated is not None:
        failures += games.check_validate(validated)
    upgraded = call("project.upgrade", {"dryRun": True})
    if upgraded is not None:
        failures += games.check_upgrade_dry_run(upgraded, project)
    result = call("test.run", {})
    if result is not None:
        try:
            # A result over the bounded-output limit comes back as a file in the copy's Library/ (§13.4).
            document = engine_client.load_offloaded(result)
        except (OSError, ValueError, engine_client.ProtocolError) as error:
            failures.append(f"the test.run result cannot be read: {error}")
        else:
            failures += games.evaluate_run(document, "Editor", copy.testing, copy.committed)
            hashes[f"Editor {config}"] = game_replay_hashes(document)
            data["hashes"] = hashes[f"Editor {config}"]
            if arguments.junit:
                report = games.editor_report_path(project, config)
                report.parent.mkdir(parents=True, exist_ok=True)
                report.write_text(json.dumps({key: value for key, value in document.items() if key != "_meta"},
                                             indent="\t") + "\n", encoding="utf-8")
                data["report"] = paths.display_path(report)
    failures += games.check_replay_acceptance(copy.events)
    with_events = sorted(replay for replay, count in copy.events.items() if count > 0)
    if with_events:
        console.heading(f"games {project.name} Editor {config}: input.replay {{verify, strictHash, withoutInput}} of "
                        f"{len(with_events)} replay(s) with input events, which must fail")
        checked = input_check_failures(client, engine_client, copy.events, arguments.timeout)
        data["inputChecked"] = with_events
        for replay in sorted(checked):
            failures += checked[replay]
    failures += games.check_replays_matched(copy.directory, copy.matched)
    return failures


def locate_testing_package(result: dict[str, Any]) -> tuple[Path, Path, str]:
    """(the export's output directory, its executable relative to that directory, the game's Name) of the result of a
    testing project.export {config: Dist} (Docs/Decisions/0027-m17-contract.md decision 4, B2): the executable is the
    result's `executable`, which must lie below its `outputDirectory`, and the Name is the package manifest's
    (games.read_game_name), read where the exporter writes it: <outputDirectory>/Game.json, or on macOS the bundle's
    Contents/Resources/Game.json beside the Contents/MacOS folder that holds the executable (Exporter.cpp:
    GetPackageResources). The bundle is never spelled from the .eproj's stem or the Name, so the stem cannot come back
    on a host the Windows gate does not see. Raises ValueError with the one failure line of the Dist step: a result
    without outputDirectory or executable, an executable outside outputDirectory, or a manifest that read_game_name
    refuses (naming the manifest)."""
    output = result.get("outputDirectory")
    if not isinstance(output, str) or not output:
        raise ValueError(f"the testing {DIST_HALF} export's result has no outputDirectory")
    executable = result.get("executable")
    if not isinstance(executable, str) or not executable:
        raise ValueError(f"the testing {DIST_HALF} export's result has no executable")
    output_directory, executable_path = Path(output), Path(executable)
    relative = executable_path.relative_to(output_directory) if executable_path.is_relative_to(output_directory) \
        else None
    if relative is None or not relative.parts or ".." in relative.parts:
        raise ValueError(f"the testing {DIST_HALF} export's executable {executable} is not below its outputDirectory "
                         f"{output}")
    if paths.host().is_macos:
        manifest = executable_path.parent.parent / MACOS_BUNDLE_RESOURCES / games.GAME_MANIFEST_FILE
    else:
        manifest = output_directory / games.GAME_MANIFEST_FILE
    try:
        game = games.read_game_name(manifest)
    except ValueError as error:
        raise ValueError(f"the testing {DIST_HALF} export's manifest does not name the game: {error}") from error
    return output_directory, relative, game


def check_game_export(client: Any, engine_client: Any, project: games.GameProject, copy: GameCopy,
                      hashes: dict[str, dict[str, str]], work: Path, data: dict[str, object],
                      arguments: argparse.Namespace, console: Console) -> Step:
    """The Dist half on the open session of the Release editor (decision 8 step 3): project.export {config: Dist,
    testing: true}, located with locate_testing_package (its executable from the result, the game's Name from its
    manifest, Docs/Decisions/0027-m17-contract.md decision 4, B2) and installed to bin/Exports/Dist/<Game>/, the
    package's --feature-test run from a temporary working directory (its reports copied to games.dist_report_path with
    --junit, so the export stage's FeatureTest-Dist reports are never touched), its exit code, testing user data and
    games.evaluate_run in Dist mode, games.compare_game_hashes over the committed hashes, every editor half of this run
    (`hashes`) and the exported run, games.check_replay_acceptance (a game whose determinism rests on no replay with
    input fails here too), then a plain project.export {config: Dist, smokeTest: true} into the editor's export root,
    checked with games.check_smoke. <Game> is the project's Name, whatever the .eproj's file stem (project.name), which
    names only the step and its reports."""
    name = f"games {project.name} Dist"
    started = time.monotonic()
    console.heading(f"{name}: project.export {{config: {DIST_HALF}, testing: true}}")
    try:
        result = client.call("project.export", {"config": DIST_HALF, "testing": True}, timeout=arguments.timeout)
    except engine_client.EngineError as error:
        return Step(name, Status.FAILED, f"the testing {DIST_HALF} export failed: {describe_export_error(error)}",
                    time.monotonic() - started, data=data)
    data.update({"testSuites": result.get("testSuiteCount"), "replays": result.get("replayCount")})
    try:
        output, relative_executable, game = locate_testing_package(result)
    except ValueError as error:
        return Step(name, Status.FAILED, str(error), time.monotonic() - started, data=data)
    try:
        package = install_package(output, DIST_HALF, game)
    except OSError as error:
        destination = paths.display_path(exports.package_directory(DIST_HALF, game))
        return Step(name, Status.FAILED, f"cannot move the testing export to {destination}: {error}",
                    time.monotonic() - started, data=data)
    executable = package / relative_executable
    data["package"] = paths.display_path(package)
    if not executable.is_file():
        return Step(name, Status.FAILED, f"the package holds no {paths.display_path(executable)}",
                    time.monotonic() - started, data=data)

    # The Runtime names its reports FeatureTest-Dist whatever the game (RuntimeApp.cpp), below its working directory.
    run_directory = work / "DistRun"
    run_directory.mkdir(parents=True, exist_ok=True)
    report = run_directory / "bin" / "TestResults" / f"{exports.FEATURE_TEST_NAME}-{DIST_HALF}.json"
    command = [str(executable), "--headless", "--feature-test"]
    console.heading(f"{name}: {format_command([paths.display_path(executable), *command[1:]])}")
    try:
        run = run_streamed(command, cwd=run_directory, env=child_environment(), timeout=arguments.timeout,
                           echo=console.stream)
    except ToolNotFoundError as error:
        return Step(name, Status.FAILED, str(error), time.monotonic() - started, data=data)
    # The user data takes the Name, the log the executable's stem (check_testing_user_data).
    log_name = relative_executable.stem
    log = exports.testing_user_data(package, game) / "Logs" / f"{log_name}.log"
    if run.timed_out:
        console.print(tail_lines(log, LOG_TAIL_LINES))
        return Step(name, Status.TIMEOUT, f"the {DIST_HALF} --feature-test run {run.describe_exit()}",
                    time.monotonic() - started, data=data)
    failures: list[str] = []
    if run.exit_code != EXIT_SUCCESS:
        failures.append(f"the {DIST_HALF} --feature-test run {run.describe_exit()}")
        console.print(f"The end of {paths.display_path(log)}:")
        console.print(tail_lines(log, LOG_TAIL_LINES))
    failures += check_testing_user_data(package, game, log_name)
    exported: dict[str, str] = {}
    try:
        failures += games.evaluate_run(json.loads(report.read_text(encoding="utf-8")), DIST_HALF, copy.testing,
                                       copy.committed)
        exported = exports.read_replay_hashes(report)
    except (OSError, ValueError) as error:
        failures.append(f"no readable --feature-test report: {error}")
    if arguments.junit:
        for suffix in (".json", ".xml"):
            source = report.with_suffix(suffix)
            if source.is_file():
                destination = games.dist_report_path(project, suffix)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, destination)
        data["report"] = paths.display_path(games.dist_report_path(project, ".json"))
    runs = {**hashes, EXPORTED_DIST_RUN: exported}
    data["hashes"] = runs
    failures += games.compare_game_hashes(copy.committed, runs)
    failures += games.check_replay_acceptance(copy.events)

    console.heading(f"{name}: project.export {{config: {DIST_HALF}, smokeTest: true}}")
    warnings: list[str] = []
    try:
        smoke = client.call("project.export", {"config": DIST_HALF, "smokeTest": True}, timeout=arguments.timeout)
        smoke_failures, warnings = games.check_smoke(smoke, arguments.require_gpu)
        failures += smoke_failures
        data["smokeTestRan"] = smoke.get("smokeTestRan")
    except engine_client.EngineError as error:
        failures.append(f"the plain {DIST_HALF} export failed: {describe_export_error(error)}")
    duration = time.monotonic() - started
    for failure in failures:
        console.print(f"FAILED: {failure}")
    if failures:
        data["failures"] = failures
        return Step(name, Status.FAILED, describe_lines(failures), duration, data=data)
    compared = ", ".join(runs)
    detail = (f"the testing {DIST_HALF} export ({data['testSuites']} suite(s), {data['replays']} replay(s)) passed "
              f"--feature-test with every replay at its committed FinalStateHash in {compared}")
    if warnings:
        data["warnings"] = warnings
        return Step(name, Status.WARNING, f"{detail}; {'; '.join(warnings)}", duration, data=data)
    return Step(name, Status.PASSED, f"{detail}; the plain {DIST_HALF} export's smoke test exited 0", duration,
                data=data)


def shutdown_game_editor(client: Any, engine_client: Any) -> list[str]:
    """session.shutdown {}, as failure lines: none when the editor accepts it; when it refuses (unsaved changes in the
    open scene, which no check may leave), a failure, and session.shutdown {force: true} so the editor still exits."""
    try:
        client.call("session.shutdown", {}, timeout=EDITOR_SHUTDOWN_TIMEOUT_SECONDS)
        return []
    except engine_client.EngineError as error:
        client.call("session.shutdown", {"force": True}, timeout=EDITOR_SHUTDOWN_TIMEOUT_SECONDS)
        return [f"session.shutdown refused to exit ({error.detail}): the checks left unsaved changes in the open scene"]


def drive_game_session(project: games.GameProject, config: str, editor_half: bool, dist_half: bool,
                       hashes: dict[str, dict[str, str]], arguments: argparse.Namespace,
                       console: Console) -> tuple[Step | None, Step | None]:
    """One strict rendering editor of `config` on a fresh copy of the project's staged files (materialize_game),
    started through engine_client (GAMES_EDITOR_ARGUMENTS, --user-data-dir and --export-root in a temporary directory):
    with `editor_half` the checks of check_game_in_editor, with `dist_half` those of check_game_export, then
    session.shutdown, after which the editor must exit 0 and, for the editor half, the copy must be unchanged outside
    Library/ (snapshot_project). What fails after the Dist checks (a refused session.shutdown, a broken connection, a
    timeout, the exit code) fails the editor step when an editor half shares the session, and the Dist step otherwise.
    Returns (the "games <Name> Editor <config>" step or None, the "games <Name> Dist" step or None)."""
    editor = editor_executable(config)
    shown = paths.display_path(project.directory)
    editor_name = f"games {project.name} Editor {config}"
    dist_name = f"games {project.name} Dist"
    editor_data: dict[str, object] = {"suite": "games", "project": shown, "config": config,
                                      "editor": paths.display_path(editor)}
    dist_data: dict[str, object] = {"suite": "games", "project": shown, "config": DIST_HALF,
                                    "editor": paths.display_path(editor)}
    started = time.monotonic()

    def failed(lines: list[str], status: Status = Status.FAILED,
               done: Step | None = None) -> tuple[Step | None, Step | None]:
        """Both requested steps failed with `lines`. A Dist step already `done` keeps its result when the editor half
        shares the session, whose step reports the lines; without an editor half the lines fail it (failed_with_lines),
        so a Dist-only run never drops a failure that came after its checks."""
        duration = time.monotonic() - started
        detail = describe_lines(lines)
        editor_step = Step(editor_name, status, detail, duration, data=editor_data) if editor_half else None
        if done is not None:
            return editor_step, done if editor_half else failed_with_lines(done, lines, status)
        return editor_step, Step(dist_name, status, detail, duration, data=dist_data) if dist_half else None

    engine_client = import_engine_client()
    with tempfile.TemporaryDirectory(prefix=f"Test-Games-{config}-") as directory:
        work = Path(directory)
        try:
            copy_directory = materialize_game(project, work / "Index")
            project_file = copy_directory / project.project_file.name
            testing = games.read_testing(project_file)
            matched = expand_replay_globs(copy_directory, list(testing.replays))
            copy = GameCopy(copy_directory, project_file, testing, matched,
                            games.committed_hashes(copy_directory, matched),
                            games.replay_event_counts(copy_directory, matched))
        except (RuntimeError, ValueError, OSError) as error:
            return failed([f"the staged project cannot be prepared: {error}"])
        editor_data["replays"] = copy.committed
        before = snapshot_project(copy.directory)
        command = [*GAMES_EDITOR_ARGUMENTS, "--export-root", str(work / "Exports"), "--project", str(project_file)]
        console.heading(f"games {project.name} ({config} editor on the staged files): "
                        f"{format_command([paths.display_path(editor), *command])}")
        try:
            process = engine_client.launch_editor(editor, command, work / "UserData", paths.workspace_name(),
                                                  environment=child_environment())
        except (RuntimeError, TimeoutError, ValueError, OSError) as error:
            return failed([f"the {config} editor could not start: {error}"])
        editor_failures: list[str] = []
        dist_step: Step | None = None
        try:
            client = process.connect("games", call_log=engine_client.CallLog(None))
            with client:
                if editor_half:
                    editor_failures += check_game_in_editor(client, engine_client, project, config, copy, hashes,
                                                            editor_data, arguments, console)
                if dist_half:
                    dist_step = check_game_export(client, engine_client, project, copy, hashes, work, dist_data,
                                                  arguments, console)
                editor_failures += shutdown_game_editor(client, engine_client)
            exit_code = process.wait(EDITOR_SHUTDOWN_TIMEOUT_SECONDS)
        except TimeoutError as error:
            return failed([*editor_failures, f"the {config} editor did not answer in time: {error}"], Status.TIMEOUT,
                          dist_step)
        except (engine_client.EngineError, engine_client.ConnectionClosed, engine_client.ProtocolError,
                OSError) as error:
            console.print(process.output())
            return failed([*editor_failures, f"the {config} editor failed: {error}"], Status.FAILED, dist_step)
        finally:
            process.kill()
        exit_failure = (f"the {config} editor exited with code {describe_exit_code(exit_code)} after session.shutdown "
                        f"(--expect-no-gpu-errors fails it on any GPU error or warning)") if exit_code != 0 else ""
        if not editor_half:
            # The Dist step is this session's only step: a refused session.shutdown and the exit code fail it.
            lines = [*editor_failures, *([exit_failure] if exit_failure else [])]
            return None, failed_with_lines(dist_step, lines) if dist_step is not None else None
        editor_failures += [exit_failure] if exit_failure else []
        for change in compare_snapshots(before, snapshot_project(copy.directory)):
            kind, path = change.split(": ", 1)
            editor_failures.append(f"{path}: {kind} by the {config} editor's run, which must leave the staged files "
                                   f"as committed")
    # A shared session's Dist half counts in its own step.
    duration = time.monotonic() - started - (dist_step.duration if dist_step is not None else 0.0)
    for failure in editor_failures:
        console.print(f"FAILED: {failure}")
    if editor_failures:
        editor_data["failures"] = editor_failures
        return Step(editor_name, Status.FAILED, describe_lines(editor_failures), duration, data=editor_data), dist_step
    checked = sum(1 for count in copy.events.values() if count > 0)
    return (Step(editor_name, Status.PASSED, f"script.check, project.validate and the upgrade dry run clean; test.run "
                                             f"passed with {len(copy.committed)} replay(s) at their committed "
                                             f"FinalStateHash; {checked} replay(s) with input events fail without it; "
                                             f"the editor exited 0 and left the staged files unchanged",
                 duration, data=editor_data), dist_step)


def missing_game_builds(halves: list[str], exported: bool, arguments: argparse.Namespace) -> dict[str, str]:
    """{half: why it cannot run} for each requested half whose build is missing: an editor half's editor, and for the
    Dist half the Release editor that exports and the Dist Runtime the export packages (decision 8)."""
    missing: dict[str, str] = {}
    for config in halves:
        editor = editor_executable(config)
        if not editor.is_file():
            missing[config] = f"{paths.display_path(editor)} not found: {build_hint(config, 'Editor', arguments)}"
    if exported:
        problems: list[str] = []
        editor = editor_executable(exports.EXPORTING_EDITOR_CONFIGURATION)
        if not editor.is_file():
            problems.append(f"{paths.display_path(editor)} not found: "
                            f"{build_hint(exports.EXPORTING_EDITOR_CONFIGURATION, 'Editor', arguments)}")
        runtime = runtime_executable(DIST_HALF)
        if not runtime.is_file():
            problems.append(f"{paths.display_path(runtime)} not found: {build_hint(DIST_HALF, 'Runtime', arguments)}")
        if problems:
            missing[DIST_HALF] = "; ".join(problems)
    return missing


def run_game_halves(project: games.GameProject, halves: list[str], exported: bool, missing: dict[str, str],
                    no_device: str | None, arguments: argparse.Namespace, console: Console) -> list[tuple[str, Step]]:
    """The editor halves (`halves`, Debug before Release) and with `exported` the Dist half of one project, as
    (configuration, step) pairs: a half whose build is `missing` fails with exit code 3; without a device
    (`no_device`, the probe's reason) the others end as warnings, or fail under --require-gpu, and nothing is copied
    or started; otherwise each editor half runs its own editor, and the Dist half shares the Release half's session,
    or runs a Release editor of its own when the Release half is not requested."""
    names = {config: f"games {project.name} Editor {config}" for config in halves}
    if exported:
        names[DIST_HALF] = f"games {project.name} Dist"
    shown = paths.display_path(project.directory)
    steps: dict[str, Step] = {half: Step(names[half], Status.FAILED, detail, exit_code=EXIT_INIT_FAILED,
                                         data={"suite": "games", "project": shown, "config": half})
                              for half, detail in missing.items() if half in names}
    runnable = [half for half in names if half not in steps]
    if no_device is not None:
        for half in runnable:
            data = {"suite": "games", "project": shown, "config": half, "noDevice": no_device}
            what = "exported half" if half == DIST_HALF else "editor half"
            if arguments.require_gpu:
                steps[half] = Step(names[half], Status.FAILED, f"no usable Vulkan device (--require-gpu): {no_device}",
                                   data=data)
            else:
                steps[half] = Step(names[half], Status.WARNING, f"no usable Vulkan device, so the {what} passed "
                                                                f"without running ({no_device}); --require-gpu fails "
                                                                f"instead", data=data)
        return [(half, steps[half]) for half in names]
    hashes: dict[str, dict[str, str]] = {}
    shares_session = DIST_HALF in runnable and exports.EXPORTING_EDITOR_CONFIGURATION in runnable
    for config in [half for half in runnable if half != DIST_HALF]:
        with_dist = shares_session and config == exports.EXPORTING_EDITOR_CONFIGURATION
        editor_step, dist_step = drive_game_session(project, config, True, with_dist, hashes, arguments, console)
        if editor_step is not None:
            steps[config] = editor_step
        if dist_step is not None:
            steps[DIST_HALF] = dist_step
    if DIST_HALF in runnable and DIST_HALF not in steps:
        _, dist_step = drive_game_session(project, exports.EXPORTING_EDITOR_CONFIGURATION, False, True, hashes,
                                          arguments, console)
        if dist_step is not None:
            steps[DIST_HALF] = dist_step
    return [(half, steps[half]) for half in names]


def run_games(configs: list[str], arguments: argparse.Namespace, console: Console) -> list[Step]:
    """The games suite (ADR 0026 decision 8) over the requested configurations: Debug and Release select those editors'
    halves, Dist the exported half the Release editor drives. Every staged game project (games.discover_projects in
    index mode) gets "games <Name> audit", then its halves; a project found on disk but not staged gets one passed step,
    "games <Name>", and nothing else. The builds the halves need are looked up after the audits, then the device is
    probed once, and only then are the staged files copied. With --junit each requested configuration's steps go to
    games.junit_path(<Config>), the audit and suite-level steps into the first requested configuration's file."""
    configs = [config for config in paths.CONFIGURATIONS if config in configs]
    first = configs[0]
    assigned: list[tuple[str, Step]] = []  # (the configuration whose JUnit file holds the step, the step)
    try:
        staged = games.discover_projects(paths.REPOSITORY_ROOT, "index")
        on_disk = games.discover_projects(paths.REPOSITORY_ROOT, "disk")
    except ValueError as error:
        assigned.append((first, Step(GAMES_STEP_NAME, Status.FAILED, str(error), data={"suite": "games"})))
    except RuntimeError as error:
        assigned.append((first, Step(GAMES_STEP_NAME, Status.FAILED, f"cannot list the staged game projects: {error}",
                                     exit_code=EXIT_INIT_FAILED, data={"suite": "games"})))
    else:
        directories = {project.directory for project in staged}
        unstaged = [project for project in on_disk if project.directory not in directories]
        if not staged and not unstaged:
            assigned.append((first, Step(GAMES_STEP_NAME, Status.PASSED, NO_GAME_PROJECT_NOTE,
                                         data={"suite": "games"})))
        # The audits need neither a build nor a device.
        audits = [games_audit_step(project, console) for project in staged]
        halves = [config for config in configs if config in games.EDITOR_CONFIGURATIONS]
        exported = DIST_HALF in configs
        missing = missing_game_builds(halves, exported, arguments) if staged else {}
        runnable = [half for half in [*halves, *([DIST_HALF] if exported else [])] if half not in missing]
        no_device: str | None = None
        if staged and runnable:
            # The probe runs an editor that exists: the first runnable half's (the Dist half's is the Release editor).
            probe = runnable[0] if runnable[0] != DIST_HALF else exports.EXPORTING_EDITOR_CONFIGURATION
            no_device = probe_rendering_editor(editor_executable(probe))
            if no_device is not None:
                console.print(f"{NO_DEVICE_TEXT}games: {no_device}")
        for project, audit in zip(staged, audits):
            assigned.append((first, audit))
            assigned += run_game_halves(project, halves, exported, missing, no_device, arguments, console)
        for project in unstaged:
            assigned.append((first, Step(f"games {project.name}", Status.PASSED,
                                         f"{paths.display_path(project.directory)} is not staged, so it is not part "
                                         f"of the commit: neither audited nor run (git add it to include it)",
                                         data={"suite": "games", "project": paths.display_path(project.directory)})))
    if arguments.junit:
        for config in configs:
            report = games.junit_path(config)
            selected = [step for owner, step in assigned if owner == config]
            write_junit(report, f"Games.{config}", selected)
            for step in selected:
                step.data["junit"] = paths.display_path(report)
    return [step for _, step in assigned]


def unreadable_report_failures(reports: list[coverage.FeatureReport | None]) -> list[str]:
    """The `report` rule's lines (coverage.merge_coverage) over the positional reports when some could not be read
    (None): a position whose report is missing, unreadable, names another mode or duplicates a mode fails for its
    mode."""
    failures: list[str] = []
    for index, mode in enumerate(coverage.MODES):
        count = sum(1 for report in reports if report is not None and report.mode == mode)
        report = reports[index] if index < len(reports) else None
        if report is None or report.mode != mode or count != 1:
            failures.append(f"merge: report: {mode}")
    return failures


def run_merge(arguments: argparse.Namespace, console: Console) -> int:
    """--merge-coverage: the cross-mode union merge of three reports (ADR 0022 decision 6). A report that does not exist
    or cannot be read is a mode without a report: it fails the `report` rule for its position's mode (exit 1, like every
    other merge failure); exit 2 stays argparse's usage error."""
    reports: list[coverage.FeatureReport | None] = []
    errors: list[str] = []
    for mode, path in zip(coverage.MODES, arguments.merge_coverage):
        try:
            reports.append(coverage.read_feature_report(Path(path)))
        except ValueError as error:
            console.print(f"Test.py: the {mode} report cannot be used: {error}")
            errors.append(str(error))
            reports.append(None)
    readable = [report for report in reports if report is not None]
    failures = unreadable_report_failures(reports) if errors else coverage.merge_coverage(readable)
    for failure in failures:
        console.print(failure)
    console.print(f"Coverage merge over {', '.join(report.mode for report in readable) or 'no readable report'}: "
                  f"{'passed' if not failures else f'{len(failures)} failure(s)'}")
    if arguments.json:
        emit_json({"success": not failures, "failures": failures, "errors": errors})
    return EXIT_SUCCESS if not failures else EXIT_FAILED


def run_suite(name: str, config: str, arguments: argparse.Namespace, console: Console) -> Iterator[Step]:
    """The steps of one suite in one configuration: the automation suite's Python run, the feature suite's editor run,
    or the Tests runs of a doctest suite (one per API cap for the gpu suite)."""
    if name == "automation":
        yield run_automation(config, arguments, console)
        return
    if name == "feature":
        yield run_feature(config, arguments, console)
        return
    for run in plan_runs(name, config, arguments):
        yield run_tests(run, arguments, console)


def suite_configurations(suite: Suite, configs: list[str] | None) -> tuple[list[str], list[str]]:
    """The configurations `suite` runs in for --config `configs` (None: its §15.8 matrix), and the requested ones it
    does not take (its `allowed` configurations), in canonical order."""
    if configs is None:
        return list(suite.configurations), []
    return ([config for config in configs if config in suite.allowed],
            [config for config in configs if config not in suite.allowed])


def skipped_configuration_step(name: str, config: str) -> Step:
    """The step of a configuration that --config requested and the suite `name` does not take (--suite all only)."""
    return Step(f"{name} {config}", Status.SKIPPED,
                f"not run in {config}: the {name} suite takes {', '.join(SUITES[name].allowed)}",
                data={"suite": name, "config": config})


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run test suites in the configurations of the Architecture §15.8 matrix.",
        epilog="Exit codes: 0 every requested suite that exists passed, 1 a suite failed or none of the requested "
               "suites exists yet, 2 usage error, 3 a test executable or the MCP virtual environment is missing, or "
               "--no-wait found a machine lock held, 5 timeout (of a run, or of the wait for a machine lock).",
    )
    parser.add_argument("--suite", default="all",
                        help=f"comma-separated suites: {', '.join(SUITES)} or all (default: all)")
    parser.add_argument("--config", help="comma-separated configurations, overriding the matrix (Debug, Release; "
                                         "test executables do not exist in Dist; the export suite takes Release and "
                                         "Dist, the determinism and games suites any of the three); with --suite all, "
                                         "a suite skips the ones it does not take")
    parser.add_argument("--junit", action="store_true", help="write JUnit XML into bin/TestResults/")
    parser.add_argument("--require-gpu", action="store_true",
                        help="gpu, golden, feature, automation, export and games suites: a test case (or a Python test "
                             "that starts a rendering editor, the FeatureTest run or export, or a game's editor or "
                             "Dist half) without a usable Vulkan device fails instead of passing without running, and "
                             "so does a game's plain Dist export whose smoke test did not run; determinism suite: a "
                             "missing or stale exported report fails (exit code 3) instead of leaving that run out "
                             "(CI.py and PreCommit.py pass it unless --gpu-optional, §15.2)")
    parser.add_argument("--editor-report", type=Path,
                        help="export suite: the Editor-mode FeatureTest report its coverage merge reads (default: "
                             f"{paths.display_path(DEFAULT_EDITOR_REPORT)}, which Test.py --suite feature --config "
                             "Release --junit writes)")
    parser.add_argument("--vulkan-api", choices=VULKAN_API_CAPS,
                        help="gpu suite: run under this API cap only (default: once under each, §8.1)")
    parser.add_argument("--update-golden", action="store_true",
                        help="golden suite: write each image as the golden candidate of this machine's device class "
                             "into Tests/Golden/<DeviceClass>/ instead of comparing; review the diff before committing")
    parser.add_argument("--filter", help="run only test cases matching this pattern (unit, gpu, golden: a doctest "
                                         "wildcard pattern on test case names; automation: comma-separated unittest "
                                         "-k patterns on test ids)")
    parser.add_argument("--allow-skips", action="store_true",
                        help="contract mode (Roadmap rule 3), for a contract checkpoint's targeted runs on a milestone "
                             "branch (ADR 0028 decision 11; PreCommit.py --contract and CI.py --contract pass it too, "
                             "but no gate run of the backlog program uses them): report test cases skipped outside the "
                             f"{CHILD_TARGET_SUITE} suite (unit, gpu, golden) and skipped Python tests (automation) "
                             "instead of failing the run")
    parser.add_argument("--timeout", type=float, default=SUITE_TIMEOUT_SECONDS,
                        help=f"seconds per Tests, automation or FeatureTest run, per export of the export suite, and "
                             f"per editor call and exported run of the games suite (default: "
                             f"{SUITE_TIMEOUT_SECONDS:.0f})")
    parser.add_argument("--merge-coverage", nargs=3, metavar=("EDITOR_JSON", "RELEASE_JSON", "DIST_JSON"),
                        help="run only the cross-mode coverage merge of three FeatureTest reports (§15.6) and print "
                             "one 'merge: <rule>: <subject>' line per failure")
    parser.add_argument("--build-root", type=Path, default=paths.BIN_ROOT,
                        help="unit, gpu, golden and feature suites: the bin directory whose <OutputDir> holds the "
                             "executables (default: the repository's bin/; the AddressSanitizer workspace builds into "
                             "bin-int/Asan/bin)")
    parser.add_argument("--sanitizer", choices=tuple(SANITIZERS),
                        help="unit, gpu, golden and feature suites: give every process the suite starts the "
                             "sanitizer's environment (address: ASAN_OPTIONS and MSVC's runtime first on PATH); the "
                             "step names gain ' (ASan)' and the reports the suffix -Asan")
    parser.add_argument("--debug-heap", action="store_true",
                        help="unit, gpu and golden suites (Windows only): start each Tests process on the Windows "
                             "debug heap (Scripts/Lib/debugheap.py: under a debugger that detaches at the loader's "
                             "initial breakpoint, so every heap of the process, the graphics driver's included, checks "
                             "block tails, fills freed blocks and validates parameters, and its crash handler still "
                             "reports a crash); the step names gain ' (debug heap)' and the reports the suffix "
                             "-DebugHeap (the hardening stage's debug-heap GPU step)")
    parser.add_argument("--test-timeout-scale", type=float,
                        help="unit, gpu and golden suites: pass --test-timeout-scale=<s> (s >= 1) to the Tests binary, "
                             "which multiplies every per-test-case limit")
    parser.add_argument("--fuzz-scale", type=float,
                        help="unit suite: pass --fuzz-scale=<s> (0 < s <= 1) to the Tests binary, so each fuzz case "
                             "runs round(count * s) iterations (at least its minimum); default 0.1 for Debug, none "
                             "(every count) for Release")
    parser.add_argument("--exclude-test-suites", metavar="NAMES",
                        help="unit suite: comma-separated doctest test suites to leave out besides GPU and Golden (the "
                             "hardening stage's sanitized unit run leaves out Fuzz, which its fuzz step runs as a "
                             "superset)")
    parser.add_argument("--json", action="store_true", help="print a machine-readable result on stdout")
    locks.add_wait_arguments(parser)
    arguments = parser.parse_args(argv)
    if arguments.merge_coverage:
        return arguments
    locks.check_wait_arguments(parser, arguments)
    arguments.build_root = arguments.build_root.resolve()
    try:
        arguments.suites = paths.parse_name_list(arguments.suite, tuple(SUITES), "suite")
        arguments.configs = paths.parse_configurations(arguments.config) if arguments.config else None
    except ValueError as error:
        parser.error(str(error))
    # With the default --suite all, a suite skips the requested configurations it does not take (main reports them);
    # a suite named explicitly must take every one.
    arguments.all_suites = [item.strip().lower() for item in arguments.suite.split(",") if item.strip()] == ["all"]
    for name in arguments.suites:
        suite = SUITES[name]
        _, refused = suite_configurations(suite, arguments.configs)
        if suite.available and refused and not arguments.all_suites:
            parser.error(f"the {name} suite does not run in {', '.join(refused)} (it takes {', '.join(suite.allowed)})")
    if arguments.editor_report is not None and "export" not in arguments.suites:
        parser.error("--editor-report needs the export suite (--suite export)")
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    if arguments.update_golden and "golden" not in arguments.suites:
        parser.error("--update-golden needs the golden suite (--suite golden)")
    sanitized_suites = ("unit", "gpu", "golden", "feature")
    custom_root = arguments.build_root != paths.BIN_ROOT.resolve()
    if (arguments.sanitizer or custom_root) and not set(arguments.suites) <= set(sanitized_suites):
        parser.error(f"--sanitizer and --build-root apply to the {', '.join(sanitized_suites)} suites only")
    if arguments.debug_heap and not set(arguments.suites) <= set(DEBUG_HEAP_SUITES):
        parser.error(f"--debug-heap applies to the {', '.join(DEBUG_HEAP_SUITES)} suites only")
    if arguments.debug_heap and not debugheap.is_available():
        parser.error("--debug-heap is Windows only: the Windows debug heap does not exist elsewhere")
    if arguments.test_timeout_scale is not None and not 1.0 <= arguments.test_timeout_scale < float("inf"):
        parser.error("--test-timeout-scale must be a finite number of at least 1")
    if arguments.fuzz_scale is not None and not 0.0 < arguments.fuzz_scale <= 1.0:
        parser.error("--fuzz-scale must be in (0, 1]")
    if arguments.exclude_test_suites is not None:
        names = [name.strip() for name in arguments.exclude_test_suites.split(",")]
        if not all(names):
            parser.error("--exclude-test-suites needs comma-separated doctest test suite names")
        if "unit" not in arguments.suites:
            parser.error("--exclude-test-suites applies to the unit suite only")
        arguments.exclude_test_suites = names
    return arguments


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    console = Console(arguments.json)
    if arguments.merge_coverage:
        return run_merge(arguments, console)
    try:
        paths.host()
    except paths.UnsupportedHostError as error:
        console.print(f"Test.py: error: {error}")
        return EXIT_USAGE

    try:
        hold = take_machine_locks(arguments, console)
    except locks.LockUnavailable as error:
        step = locks.unavailable_step(error)
        console.result(step)
        console.summary("Test summary", [step])
        if arguments.json:
            emit_json({"success": False, "steps": [step.to_json()]})
        return error.exit_code
    try:
        steps = run_suites(arguments, console)
    finally:
        if hold is not None:
            hold.release()

    ran = [step for step in steps if step.status != Status.NOT_AVAILABLE]
    exit_code = overall_exit_code(steps)
    if not ran:
        console.print("Test.py: none of the requested suites exists yet, so nothing was tested")
        exit_code = EXIT_FAILED
    console.summary("Test summary", steps)
    if arguments.json:
        emit_json({"success": exit_code == EXIT_SUCCESS, "steps": [step.to_json() for step in steps]})
    return exit_code


def uses_gpu(arguments: argparse.Namespace) -> bool:
    """Whether the run includes a GPU-using suite that exists (GPU_SUITES; --update-golden is a golden run)."""
    return any(name in GPU_SUITES and SUITES[name].available for name in arguments.suites)


def take_machine_locks(arguments: argparse.Namespace, console: Console) -> locks.Hold | None:
    """The machine-wide GPU lock for a run that uses the GPU, held to its end; any other run only waits until no gate
    runs. Raises locks.LockUnavailable."""
    if uses_gpu(arguments):
        return locks.acquire_gpu(locks.wait_seconds(arguments), console.print)
    locks.wait_for_gate(locks.wait_seconds(arguments), console.print)
    return None


def run_suites(arguments: argparse.Namespace, console: Console) -> list[Step]:
    """Every requested suite in its configurations, each step printed as it finishes."""
    steps: list[Step] = []
    for name in arguments.suites:
        suite = SUITES[name]
        if not suite.available:
            step = Step(name, Status.NOT_AVAILABLE, f"the {name} suite does not exist yet; planned for {suite.planned}",
                        data={"suite": name})
            steps.append(step)
            console.result(step)
            continue
        configs, refused = suite_configurations(suite, arguments.configs)
        for config in refused:  # only with --suite all; parse_arguments refuses them for a named suite
            step = skipped_configuration_step(name, config)
            steps.append(step)
            console.result(step)
        if not configs:
            continue
        if name == "determinism":
            # One step over every configuration, which compares their hashes.
            step = run_determinism(configs, arguments, console)
            steps.append(step)
            console.result(step)
            continue
        if name == "export":
            # One editor exports every configuration, and the merge needs both.
            for step in run_export(configs, arguments, console):
                steps.append(step)
                console.result(step)
            continue
        if name == "games":
            # The Dist half compares its hashes with every editor half of the run, and may share the Release editor.
            for step in run_games(configs, arguments, console):
                steps.append(step)
                console.result(step)
            continue
        for config in configs:
            for step in run_suite(name, config, arguments, console):
                steps.append(step)
                console.result(step)
    return steps


if __name__ == "__main__":
    sys.exit(main())
