# 0026 — M16 Breakout dry run contract

- **Status:** draft for the contract commit (lead-owned; drafted by the contract agent from the lead's plan). It is accepted with the contract commit, which runs `PreCommit.py --contract` once (ADR 0004, ADR 0023). Decision 1 records the product owner's decision of 2026-10-14. The friction list, the time box and the record of the operational rules are filled in during the dry run; the milestone commit closes them.
- **Date:** 2026-10-14.
- **Base:** `fd4edd1` (M15 complete; ADR 0024, ADR 0025).
- **Authority:** Roadmap "Rules for every milestone" (rule 3, the contract task) and M16; Architecture §2.3, §6, §13.4, §13.8, §13.12, §15.1, §15.8 and §15.9; ADR 0008 decisions 10, 11 and 16, ADR 0011, ADR 0023, ADR 0024 decisions 1 and 20, and ADR 0025 decisions 7, 10 and 15. The lead's plan (`bin/Reviews/M16-Plan.md`) and the analysis behind it (`bin/Reviews/M16-Map.json`: five area maps and a completeness critic) are kept under the ignored `bin/Reviews/`.

## Context

M16 is the time-boxed dry run of the agent workflow before the required demos: an agent builds `Tests/Projects/Breakout` through the MCP bridge only, and the milestone also delivers `Scripts/AuditProvenance.py` and the `games` suite and stage (ADR 0025 decision 10), so that the dry run's acceptance, and M17's and M18's, is mechanical. Three implementation streams precede the dry run:

- **A:** the provenance audit: `AuditProvenance.py`, `Lib/provenance.py`, `Lib/xxh64.py`, its fixtures and its static-check steps.
- **B:** the games suite and stage: `Lib/games.py`, `Test.py`'s suite, `CI.py`'s stage and PreCommit's Debug run.
- **C:** the editor, bridge and repository gaps the demo audits would otherwise fail on (G1 to G8 and three smaller items).

Then the builder's sessions run in the main checkout, with friction fixes between them. The facts this contract relies on were checked again in the code:

- Both writers the audit needs exist. `ProvenanceRecorder` and `EditorContext`'s write observer keep `Automation/Provenance.json` (one canonical entry per path for the root `.eproj` and `Assets/**`, PascalCase keys, ADR 0008 decision 10); the bridge appends `Automation/BuildLog.jsonl` (`Tools/MCP/engine_mcp/transcript.py`, ADR 0008 decision 11) and sends each request's line as `params._meta.transcriptLine`; `Editor --upgrade` appends its own `cli` lines (`TranscriptLog`, `BatchRunner`). A read-only port of `autosave.py`'s XXH64 matched all 361 entries of FeatureTest's provenance.
- `AuditProvenance.py`, a library the games suite can import, fixtures and tests do not exist. `Test.py` and `CI.py` register `games` as not available (Release only); PreCommit runs five suites in Debug.
- **G1, deferred import writes are "ui".** FeatureTest's provenance, written only by `Editor --batch`, has 361 entries: 355 `batch` and 6 `ui`, all six a glTF source's own `.meta` (`Assets/Formats/Gltf/{Box.glb, Box.gltf, DataUri.gltf, Textured.gltf}.meta`, `Assets/Models/Golden/{NormalMapped, Textured}.gltf.meta`). `asset.import` starts its import with `ReimportAsync`; the job posts its publication (`FinishAsyncImport`, then `ApplyImport`: the dependency metas of its reads and the source's `.meta` rewritten with its sub-assets) to the main-thread queue, and when a frame's `MainThreadQueue::Drain` runs it before the server next polls the pending operation (whose `WaitIdle` would run it inside the request), it runs outside any request's attribution (`AutomationServer::EnterInvocation`/`LeaveInvocation`). Dependency metas written inside the request are attributed correctly (`Textured.bin.meta` is `asset.import`, request 3).
- **G2, opening with `--project` writes as "ui".** `EditorContext::OpenProject` scans the assets and writes `.meta` files for new sources with the current attribution, which is `ui` when `--project` opens the project at startup; the bridge opens every existing project that way (`launcher.py`), and so does `--upgrade` before its `project.upgrade` request. `TranscriptLog` writes only through `project://`, which does not exist in the launcher state, so `--upgrade` cannot open the project through a transcribed `project.open`.
- **G3:** `project.upgrade` rewrites only the `.eproj`, `.scene` and `.prefab` files (`ListUpgradeFiles`, `MakeCanonicalFile`), although §13.12 names a `.meta` schema change as its job.
- **G4:** the project templates' `.gitignore` holds only `Library/`; the root `.gitignore` covers only `/Projects/*/Library/` and `/Tests/Projects/*/Library/`; `project://` keeps a `.bak` beside every replaced file, and `project.export` writes `<Project>/Build/`.
- **G5:** the root `.luaurc` is read by the type checker and written by `project.create`, but never recorded.
- **G6:** `ProvenanceRecorder.h`, `WriteAttribution`'s comment and the generated `Provenance.schema.json` say `RequestId` is null for `cli` writes; `BatchRunner` sends id 1 with its transcript line (`EditorAppTests` asserts `"TranscriptLine": 1`).
- **G7:** `.gitattributes` (`* text=auto eol=lf`, `.gltf` not binary) normalizes CR LF text on commit, so a fresh clone can hold other bytes than provenance hashed.
- **G8:** `Lint.py`'s strict JSONL check reads every `.jsonl` outside `Tests/Data/**`, and the bridge completes a torn transcript line in place by design; `TranscriptLog` refuses a torn transcript and tells the user to edit it by hand.
- The canonical-JSON cases of `AuthoredFilesTests.cpp` walk `Projects/` and `Tests/Data/` but not `Tests/Projects/`, and their `Tests/Data` walk would require the audit's byte-hash fixtures to be canonical JSON. The bridge's `engine_call` times out after 60 s whatever the method; the DocExamples cases scan `Projects/*/AGENTS.md` but not `Tests/Projects/*/AGENTS.md`.
- The Release and Dist binaries in `bin/` predate the M14 commit (the contract task rebuilt Debug); Debug, Release and Dist are rebuilt on the integrated tree before the first builder session.

## Decisions

### 1. The Linux and macOS catch-up is deferred (the product owner, 2026-10-14)

The product owner said on 2026-10-14: "mac and linux are not a priority".

- The catch-up that Roadmap M16's scope, ADR 0024 decision 20 and ADR 0025 decision 15 scheduled before M16 starts is deferred until the product owner asks for it: the GitHub Actions failures of the M15 milestone commit, a macOS run of an exported bundle (signature, loader lookup), ADR 0025 decision 15's M29 and M30, and ADR 0013 decision 25's third limit. M16 starts without it.
- Remote CI stays informational and non-blocking (ADR 0011). Linux and macOS are not reported as verified while their jobs are red; a platform counts as verified only once its CI job is green (unchanged).
- First-party code stays portable. AGENTS.md's rule that Linux and macOS code must really compile with GCC 14, Clang 19+ and Apple Clang still describes the target, and the local portability stage (generation checks, the clang-cl builds) still gates every milestone; Actions failures no longer have to be fixed before the demo games. A failure that points to a design problem is still raised at once.
- The M16 map's catch-up designs stay in `bin/Reviews/M16-Map.json` for whenever the catch-up happens: the bundle probe in `test_export.py`, the POSIX port of the Runtime's automation connection (M29), the ELF symbol reader beside the GNU map (M30), and the triage without `gh`, which is not installed here.
- **Amended in this contract:** Roadmap M16's scope sentence; AGENTS.md's remote-CI paragraph; Architecture §15.8's GitHub Actions sentence, §17's Linux/macOS risk row and §18's M16 row; a superseded note on ADR 0025 decision 15, on ADR 0024 decision 20's catch-up bullet and on ADR 0011's batching bullet.

### 2. What the contract freezes, and what it implements

The contract freezes the script CLI of decision 3, the Python APIs of decisions 4, 5 and 7, the fixture format of decision 6, the games suite and stage of decision 8 (described here, implemented by stream B), the static-check steps of decision 9, the C++ header changes of decisions 10 and 11 and the documented behaviour of decision 12.

- **Python stubs** (ADR 0008 decision 16, ADR 0024 decision 1): every function body raises `NotImplementedError("contract stub: <area> (ADR 0026 decision <n>, stream <X>)")`; dataclasses and constants are complete. Their tests carry `unittest.skip` with the same kind of message: `Tests/Automation/test_audit_provenance.py` (31 tests) and `Tests/Automation/test_games_suite.py` (29 tests) name every behaviour streams A and B implement.
- **`AuditProvenance.py` exists** with its whole CLI (`--help`, `--json`, usage errors with exit code 2, the documented exit codes, `sys.exit(main())`); its behaviour runs through the library stubs, so anything but `--help` and a usage error ends in the contract stub's `NotImplementedError`. Unlike ADR 0024's new scripts, which were frozen as CLIs only, it can exist in the contract because nothing runs it: the static checks gain its steps only with stream A (decision 9).
- **C++ stubs** (ADR 0024 decision 1): the first statement of each new function is `ENGINE_CONTRACT_STUB();`; a stub returning `Result` returns `Unsupported`, any other a neutral value (`SetWriteContext` ignores its context, `IsAssetScanDeferred` returns false). The new members (`AssetWriteEvent::WriteContextId`, `AssetWriteContext`, `EditorContextSpecification::DeferAssetScan`) are inert until stream C implements them.
- **Acceptance tests:** five `doctest::skip(true)` cases with their final names (decisions 10 to 12). Each body fails when forced (verified with `--no-skip`): against the stubs no import holds its write context and every publication's writes carry 0, a deferred `.meta` is still `ui`, the scan still runs at open (and fails there on the case's unreadable `.luaurc`), `--upgrade` writes two transcript lines, and `project.upgrade` changes no asset file.
- **Unchanged:** `Test.py`'s `Suite("games", ("Release",), False, "M16-M18 (demo games)")` and `CI.py`'s `games` stage stay registered as not available; `PreCommit.py`, `Lib/scripts.py`, `ModuleRules.json`, `.gitignore`, `.gitattributes`, the templates and the bridge do not change. No existing behaviour changes, and no generated reference changes (no method, schema or registry type changed).

### 3. `Scripts/AuditProvenance.py` (stream A)

`python Scripts/AuditProvenance.py (--project P [--project P ...] | --demos | --self-test) [--files index|disk] [--json]`

- **`--project P`:** a project directory, or its `.eproj` file (its parent is the project); repeatable; made absolute against the working directory. Whether it exists is the audit's question (`project-missing`).
- **`--demos`:** every project `games.discover_projects(<repository root>, <files>)` returns (decision 7), in the audit's own mode: in index mode only the projects whose `.eproj` is staged, and each project found on disk but not staged gets a passed step, `provenance <path>`, saying it is not part of the commit and was not audited (so a half-built demo outside the commit never fails an ordinary commit and is never skipped silently); in disk mode every project on disk. With none, it passes with one step, "provenance demos", saying so. A directory holding several `.eproj` files fails "provenance demos" naming it (exit code 1, a repository defect rather than a usage error, so `--json` still prints its document); git failing in index-mode discovery fails it with exit code 3.
- **`--self-test`:** the fixtures of decision 6 in disk mode against their `Expected.json`; it takes no `--files` (usage error).
- **`--files`:** `index` (the default, and the acceptance mode "from a fresh clone") or `disk` (decision 4).
- **Output:** one step per project, `provenance <path>`. Passed: `<mode>: <n> engine-consumed file(s), <n> entries, <n> transcript line(s) (<n> requests), <n> warning(s)`. Failed: `<mode>: <n> finding(s): <code> <count>, ...`, after printing up to 20 findings per code (`  <code>: <path>: <detail>`) and every warning. `--json` prints `{"success", "exitCode", "projects": [provenance.audit_to_json(audit), ...], "steps": [...]}`.
- **Exit codes:** 0 passed (warnings allowed); 1 findings, or a self-test mismatch; 2 usage error; 3 a required file is missing (`project-missing`, `provenance-missing`, `transcript-missing`) or git is missing or fails in index mode. With several projects, the first failing project's code (`Lib/report.overall_exit_code`).

### 4. `Scripts/Lib/provenance.py`: scope, rules and finding codes (stream A)

The module's docstring is the specification; in short:

- **Engine-consumed files:** the project root's `*.eproj` and `.luaurc` and every file below `Assets/` (`.meta` included). `AGENTS.md`, `README.md`, `.gitignore` and `Automation/**` are exempt, nothing else is audited, and nothing below `Library/` is ever read.
- **File sources:** `index` materializes the project's staged blobs into a temporary directory (`git -C <repo> ls-files -z -- <project>` piped to `git -C <repo> checkout-index -z --stdin -f --prefix=<tmp>/`), so `.gitattributes`' conversions apply and untracked and ignored files are absent; a committed `*.bak` is audited. Every file is read from that copy, `Automation/Provenance.json` and `Automation/BuildLog.jsonl` included, so an unstaged change to them is ignored like an unstaged change to an asset. A project directory that does not exist (checked before git runs), or one with no staged file or no staged root `.eproj`, is `project-missing` (exit code 3); a directory outside any git work tree is a git failure (exit code 3). `disk` audits the working tree and leaves `*.bak` out.
- **Readers:** `load_provenance` is as strict as `ProvenanceRecorder::FromText` (duplicate keys, NaN, unknown or missing members, a boolean `RequestId`, an `XXH64` that is not 16 hex digits, a `TranscriptLine` below 1, unsorted or duplicate paths: `ValueError` with a JSON pointer). `load_transcript` splits on `b"\n"` only, never `str.splitlines` (raw U+2028, U+2029 and U+0085 occur inside lines); a non-empty tail without a newline is an unterminated last line; a line that is not a JSON object is torn.
- **Rule 1:** an entry per file (`entry-missing`) whose XXH64 equals the file's hash, compared case-insensitively (`hash-mismatch`).
- **Rule 2:** Method and Client are not `ui` (`written-by-ui`); the `TranscriptLine` names a line of the transcript (`transcript-line-missing` for a null line or one past the end) that is a request line with the entry's `RequestId` (compared type-strictly), Method and Client (`transcript-mismatch`, naming the member). §13.12's exception: a `project.upgrade` entry of client `cli` without a line passes when the transcript holds a `cli` `project.upgrade` request. The response line is not required: a rolled-back `edit.batch` writes files under a request whose response is not ok, and a killed bridge leaves no response.
- **Rule 3:** an entry whose path is not among the audited files, compared as exact case-sensitive strings (never `Path.exists`, which ignores case on Windows and macOS), is `entry-dangling`, unless a later request line is an ok `asset.delete` or `asset.move` (top level or an `edit.batch` op) whose `asset` or `path` param names the file, its source or a directory above it. The editor forgets entries when it deletes or moves files, so this is only the fallback §13.12 states, matched by path.
- **The files themselves:** `project-missing`, `provenance-missing`, `provenance-invalid` (no rule runs then), `transcript-missing`, `transcript-invalid` (a JSON-object line that is neither a well-formed request nor a well-formed response line, or a `session.hello` request, whose token must never be committed).
- **Warnings:** `crlf` (an engine-consumed text file holds CR LF; a fresh clone keeps it only under `-text`, decision 13's G7) and `torn-line` (a torn line no entry references; one an entry references is a `torn-line` finding).
- **Beyond the plan, decided here:** the `project-missing` code (the plan's exit code 3 for a missing directory or `.eproj` needed one); `audit_exit_code` and `audit_to_json`; `enumerate_engine_files`' `files` parameter (`*.bak` left out in disk mode only). Not adopted: an allow-list of clients (§13.12 asks for "an automation method", the transcript match already proves the request was transcribed, and decision 11 adds `cli` `project.refreshAssets` entries); a byte-canonical check of `Provenance.json` (structural validation only); a check that the transcript only grew since `HEAD`; a check of absolute paths in committed request params (ADR 0008 decision 11 records params as sent). Each can return as a friction item.

### 5. `Scripts/Lib/xxh64.py` (stream A)

A byte-identical port of `Tools/MCP/engine_mcp/autosave.py`'s `xxh64`, because `Scripts/` cannot import the MCP package. `xxh64(data) -> int` and `xxh64_hex(data) -> str` (16 lowercase hex digits, as `Provenance.json` writes them). Its test runs the reference vectors of `HashTests.cpp`, the same as `test_autosave_probe.py`, so the two copies cannot drift apart unnoticed.

### 6. Fixtures and the self-test (stream A)

- **Generator:** `Tests/Data/Generate/MakeProvenanceFixtures.py` (standard library, `--check`, `--json`, exit codes 0/1/2), which owns `Tests/Data/Provenance/` and is deterministic (fixed times and ids; hashes through `Scripts/Lib/xxh64.py`). The static checks run its `--check` like every generator's.
- **Layout:** `Tests/Data/Provenance/<Case>/Project/` (a small project: `<Name>.eproj`, `.luaurc`, `.gitignore`, `AGENTS.md`, a scene, a script and a material with their `.meta` files, `Automation/Provenance.json`, `Automation/BuildLog.jsonl`) and `Tests/Data/Provenance/<Case>/Expected.json`: `{"Format": "ProvenanceAuditFixture", "Version": 1, "ExitCode": 0|1|3, "Findings": [{"Code", "Path"}], "Warnings": [{"Code", "Path"}]}`.
- **Cases:** `Clean`, a simulated bridge build (`project.create` at line 1, requests each with its response, a reconnect whose ids restart, an `asset.delete` inside an `edit.batch`, a `cli` `project.upgrade` with its line), and at least one case for every finding and warning code except `crlf`, which `eol=lf` would normalize in a committed fixture (`test_crlf_text_is_a_warning` builds it in a temporary copy). Every passing behaviour of decision 4 is folded into `Clean`, so an audit that refused one fails it: a request without a response line (a killed bridge), a failed `edit.batch` whose writes are recorded (a rolled-back batch), raw U+2028 and U+0085 inside a line, and a `cli` `project.upgrade` entry without a line beside the transcript's `cli` `project.upgrade` request (§13.12's exception); `test_the_clean_fixture_passes_with_its_counts` checks that `Clean` holds each. The map's list is a starting point for the failing cases (`HashMismatch`, `UiWrite`, `BatchWrite`, `LineOutOfRange`, `LineNotRequest`, `MethodMismatch`, `IdMismatch`, `ClientMismatch`, `TornReferencedLine`, `TornUnreferencedLine`, `DanglingEntry`, `DeletedThroughTranscript`, `UpgradeWithoutTranscript`, `TokenInTranscript`, `MissingProvenance`, `MissingTranscript`, `MissingProject`); its `UnicodeLineSeparators` and `UpgradeWithoutLine` are passing behaviours, folded into `Clean`.
- **Self-test:** `self_test(fixtures=PROVENANCE_FIXTURES)`: one step per case, `provenance self-test <Case>`, failing when the case's exit code or its sets of (code, path) pairs differ from `Expected.json` or when a case other than `Clean` has neither a finding nor a warning; then `provenance self-test coverage`, failing naming each finding code no case expects. A test proves it fails, over a temporary copy of the fixtures with one `Expected.json` changed and one code's only case removed.
- **Exemption:** the fixtures are byte-hash inputs, never engine input, so "AuthoredFiles: every authored file under Tests/Data is canonical JSON" exempts `Tests/Data/Provenance/` as it exempts `Lint/` (stream A's hunk of `AuthoredFilesTests.cpp`); `ModuleRules.json`'s `Json.Exclude` already covers `Tests/Data/**`.

### 7. `Scripts/Lib/games.py` (stream B)

- **Discovery:** `GAME_PROJECT_ROOTS = ("Projects", "Tests/Projects")`, `EXCLUDED_PROJECTS = ("Projects/FeatureTest",)` (FeatureTest is covered by the feature, export and determinism suites and never audited, ADR 0025 decision 7). `discover_projects(root, files="disk")` returns every directory directly below a root whose root holds exactly one `*.eproj`, sorted by root-relative path, and raises `ValueError` for a directory with several. With `files="index"` it reads the staged files (`git ls-files`), so a project counts only when its `.eproj` is staged: the index-mode audit and the games suite, which audit and run the staged blobs, discover the same way, and an untracked half-built demo (Tetris while M17's engine fixes are committed) is not part of an ordinary commit's gate. Both callers name each project found on disk but not staged in a passed step (decisions 3 and 8), so none is skipped silently; the disk-mode static check (decision 9) still audits it. The audit (`--demos`) and the games suite share it; `hardening.soak_projects` keeps its own list (every `Projects/*` with a `.eproj`, FeatureTest included; the dry run is not soaked, ADR 0025), and a test pins that the soak does not depend on this module.
- **Testing settings:** `read_testing(project_file) -> TestingSettings` (each suite's Script, Scene and Modes, default all three; the Replays globs as written).
- **Evaluation:** `evaluate_run(document, mode, testing, committed)` checks a `TestRunResult` without coverage gates (games leave `Testing.CoverageGates` off): the mode, passed and not cancelled, no skipped case row, at least one suite, a suite row for every entry whose modes include the run's mode, a passing `replay:<path>` row for every committed replay, and each row's hash equal to the committed `FinalStateHash`. `check_script_check`, `check_validate` (warnings included: a demo's `project.validate` must be clean), `check_upgrade_dry_run` (each changed file, naming `Editor --headless --project <dir> --upgrade`) and `check_smoke` (`smokeTestRan` false is a warning, a failure with `require_gpu`) return failure lines.
- **Beyond the plan, decided here:** `committed_hashes(project_directory, replays)` (the suite needs each matched replay's committed `FinalStateHash`; the glob expansion stays `Test.py`'s `expand_replay_globs`); `check_replays_matched(project_directory, matched)` (a `.replay` below `Assets/` that `Testing.Replays` does not match) and `compare_game_hashes(committed, runs)` (the Dist half's `exports.compare_determinism_runs` over the committed hashes, the editor halves and the exported run, each line naming its replay), so both rules are unit-tested without an editor; the report-path helpers `editor_report_path`, `dist_report_path` and `junit_path`, so the names are frozen in one place. `REPLAY_SUITE_PREFIX`, `EDITOR_CONFIGURATIONS` and `EXPORTED_CONFIGURATION` are aliases of `Lib/exports.py`'s constants, so the export, determinism and games suites cannot drift apart.

### 8. The games suite and stage (stream B)

- **Registration:** `Suite("games", ("Debug", "Release", "Dist"), True, allowed=paths.CONFIGURATIONS)`. `main` calls `run_games(configs, arguments, console) -> list[Step]` once with every requested configuration. Debug and Release select those editors' halves; Dist selects the exported half, which the Release editor drives (`exports.EXPORTING_EDITOR_CONFIGURATION`).
- **No game project:** one passed step, `games`, "no game project below Projects/ or Tests/Projects/ (FeatureTest excluded)", so the contract and the integration commit pass before Breakout exists.
- **Discovery:** `games.discover_projects(<repository root>, "index")` (decision 7). A project found on disk but not staged gets one passed step, `games <Name>`, saying it is not staged and not part of the commit, and nothing else runs for it. A directory holding several `.eproj` files fails one step, `games`, naming it (exit code 1).
- **Per discovered project, in order:**
  1. **`games <Name> audit`**, once per run: `provenance.audit_project(<dir>, "index")`; no device needed; a failure names each finding (`<code> <path>`).
  2. **`games <Name> Editor <Config>`** for Debug and Release: a `materialize_index` copy in a temporary directory (never `Library/` or `Build/`), snapshotted; the strict headless editor of that configuration, `--headless --renderer vulkan --audio-device none --automation --strict --expect-no-gpu-errors --user-data-dir <tmp> --export-root <tmp>/Exports --project <copy>/<Name>.eproj`; then `script.check` (clean), `project.validate` (clean), `project.upgrade {dryRun: true}` (no changed file), `test.run {}` read through `load_offloaded` and evaluated with `evaluate_run(document, "Editor", ...)`; a `.replay` below `Assets/` that `Testing.Replays` does not match fails the step (`check_replays_matched`: an export would never run it); `session.shutdown` exits 0; the copy is unchanged outside `Library/` (`snapshot_project`, `compare_snapshots`). With `--junit` the run is written to `Games-<Name>-Editor-<Config>.json`.
  3. **`games <Name> Dist`:** the Release editor (on its own index copy, or the Release half's session when Release runs too) makes `project.export {config: "Dist", testing: true}`, installed to `bin/Exports/Dist/<Name>/`; `<package executable> --headless --feature-test` runs from a temporary working directory, so the export stage's `FeatureTest-Dist` reports are never overwritten, and its reports are copied to `Games-<Name>-Dist.{json,xml}`. It checks the exit code (printing the log tail), the testing user data, `evaluate_run(document, "Dist", ...)`, and `compare_game_hashes` over the committed hashes, every editor half of the run and the exported run, so the replay's hash is identical in the editors and the Dist export. Then a plain `project.export {config: "Dist", smokeTest: true}` into the temporary export root, checked with `check_smoke`.
- **Editors:** the editor and Dist halves start their editors through `import_engine_client()` (`launch_editor`, `connect`, `call`, `load_offloaded`, `wait`), as the export and determinism suites do, so the suite's tests fake the editor there.
- **Builds, then the device, then the copies:** after the audit, which needs neither, the builds the requested halves need are looked up first, because the device probe runs an editor: each requested editor half's editor (`editor_executable(config)`), and for the Dist half the Release editor and the Dist Runtime (`runtime_executable("Dist")`). A missing build fails that half's step with exit code 3 and the `Build.py` hint, whatever the device. Then the device is probed once (`probe_rendering_editor`) with an editor that exists; without one, the editor and Dist steps end as warnings naming the reason, or fail under `--require-gpu`, and no index copy is made and no editor is started. Only then are the copies materialized.
- **Outputs:** JUnit `bin/TestResults/Games-<Config>.xml` per requested configuration (the audit steps go into the first requested configuration's file, in Debug, Release, Dist order), `Games-<Name>-Editor-<Config>.json` and `Games-<Name>-Dist.{json,xml}`; packages in `bin/Exports/Dist/<Name>/`.
- **Refactors:** `install_package(output, config, name=exports.FEATURE_TEST_NAME)`; `runtime_executable(config, build_root=paths.BIN_ROOT) -> Path`, the export suite's inline Runtime lookup, shared; the replay verification of an open session split out of `verify_replays`; `copy_project` skips `Build/` as well as `Library/`. Every name `test_coverage_gates.py` and `test_testing_export.py` use is kept.
- **`CI.py`:** `Stage("games", ("Debug", "Release", "Dist"), True)`; `Runner.games(configs)` runs one `Test.py --suite games --config <configs> --junit`, with `--require-gpu` unless `--gpu-optional` and `--allow-skips` in contract mode, as the step `games <configs>`; `TIMEOUTS["games"] = 3600.0`. It is part of plain `CI.py` and of `--milestone`, and its Debug and Release editor halves verify the demos' replays in both editor configurations (Roadmap rule 1). `.github/workflows/ci.yml` does not run it (like export, it needs a device and the Dist build).
- **PreCommit:** `--suite unit,gpu,golden,feature,automation,games --config Debug`: the audit and the Debug editor half; no Dist half, since PreCommit builds no Dist. This answers ADR 0025 decision 10's question: engine fixes in M17 and M18 land as ordinary commits, which are the ones that break a demo's canonical form or replay hashes (§13.12). Estimated cost: 30 to 90 s for Breakout.
- **Tests:** `test_games_suite.py` (frozen names), and the updates of `test_testing_export.py` (games is no longer not available; the `--config Dist` skip count) and `test_ci_script.py`. "AuthoredFiles: every authored file under Projects is canonical JSON" and "AuthoredFiles: every committed replay records a run mode as its Config" also walk `Tests/Projects/`, with the same skips (stream B's hunk of `AuthoredFilesTests.cpp`; the 100-file floor stays for `Projects/` only).

### 9. Static checks (stream A)

- `Lib/scripts.py`'s `run_static_checks` gains, after "runtime map self-test", **"provenance audit self-test"** (`AuditProvenance.py --self-test`), which proves every seeded fixture still yields exactly its findings. It runs in PreCommit and in `CI.py`'s lint stage (no device, no git).
- The demos themselves are audited once per gate, by the games suite's audit step in index mode (the acceptance mode): PreCommit runs the games suite in Debug and `CI.py` runs the games stage. A second, disk-mode audit in the static checks would repeat that work (ADR 0023), so `--demos --files disk` stays a command for local use and is not a static check (lead decision on open question 2).
- The lead amends Architecture §2.3, §15.8's lint row and §15.9's list, AGENTS.md's commit-gate list and the `commit-review` and `build-and-test` skills at integration.

### 10. G1: deferred import writes carry the scheduling request's attribution (stream C)

**Frozen header changes** (the M6 `AssetWriter.h` and `EditorAssetManager.h`, reviewed in this contract):

- `AssetWriteEvent::WriteContextId` (`uint64_t`, 0 by default): the Id of the write context of the import whose publication made the write. The `AssetWriter` itself always reports 0; the `EditorAssetManager` sets it on the events it forwards to its write observer.
- `struct AssetWriteContext { uint64_t Id = 0; }`: an immutable token the editor makes current, one per attribution. An import takes the context current when it starts, and the manager holds it with the import until the import is published, superseded or dropped; the editor keeps what an Id means for as long as the token is current or an import holds it, through a weak reference to it, so no release protocol exists and no attribution outlives the imports that need it.
- `void EditorAssetManager::SetWriteContext(Ref<const AssetWriteContext> context)` (null clears it; main thread): whenever the manager begins an import's ticket (`Load`, `LoadAsync`, `Refresh` and a poll's batch, `Reimport`, `ReimportAsync`, a reimport its writer's events schedule, the end of a deferral of reloads, where a held reimport starts), the import keeps the context current then. A ticket begun while a publication runs keeps that publication's context instead (none when it has none), and the manager holds the publishing context for the whole publication, nested publications included: a dependent that the publication's changed artifacts reimport (`ApplyImport`'s `scheduleDependents`, `InvalidateSource`, `StartAsyncImport`), a main-thread importer (`SceneImporter`) that runs inline, a load from the reload listener, a reimport the publication's own writes schedule. So the work an import causes is attributed to whatever started that import: a dependent reimported by a publication that runs at a frame's `Drain` is the scheduling request's, not `ui`. Every write of an import's publication (`WriteDependencyMetas`, the sub-asset rewrite of the source's `.meta`) reaches the observer with `WriteContextId` set, wherever the publication runs: inside the call, in a `WaitIdle`, or at a later frame's `Drain` outside any request. No context gives 0. The manager never calls back into the editor for it.

**EditorContext** (private state only, no public API): `SetWriteAttribution` (the UI's reset included, so a UI-scheduled import's writes stay `ui`) makes one token per attribution it is given and hands it to the manager with `SetWriteContext`; the editor keeps `Id -> WriteAttribution` for the tokens still alive, and its write observer records a write with `WriteContextId` set under that attribution and every other write under the current one. So `asset.import` and `asset.reimport` attribute the `.meta` rewrites their imports publish after the request returned to themselves, with their request id and transcript line (§13.12 rule 2).

**Considered and not chosen:** making the import methods resolve only after every import they scheduled was published inside `Poll` (the map's alternative): the frame's `Drain` runs before the next `Poll`, so the publication would still run outside the request; a plain `uint64_t` tag with an `Id -> attribution` map would need a release signal from the manager; moving `WriteAttribution` down to the asset pipeline would change the M4 provenance header for no gain; a provider the manager calls whenever it begins a ticket (the first draft of this contract): the editor makes one token per attribution anyway, so a pull callback only added a call from inside `BeginTicket` (during `ApplyImport` too) back into `EditorContext` and an obligation to clear it before `~EditorContext`, whose members the manager must not call back into.

**Tests (skipped, final names):** "EditorAssetManager: the writes an import's publication makes carry the write context taken when the import started" (the import holds its context until its publication and releases it then, a dependent reimported by a publication run at `Drain` inherits that publication's context, and a superseded import's context is released with its dropped completion) and "EditorContext: a glTF's .meta rewritten with its sub-assets after asset.import returned is attributed to asset.import" (the critic's nuance: the source `.meta`'s sub-asset rewrite itself is asserted, with the dependency metas). The end-to-end regression through the bridge is `test_audit_provenance.py`'s `BridgeAuditTests` (decision 4 with this one and decision 11).

**FeatureTest:** with G1 a regeneration attributes its six glTF `.meta` entries to `asset.import`, and `Scaffold.py --check` compares `Automation/Provenance.json` byte for byte, so stream C regenerates FeatureTest (`Scaffold.py --config Release`, then `--check`) in the change that lands G1, G4 and G5 (decision 13).

### 11. G2: opening an existing project is attributed and transcribed (stream C)

- **The bridge (G2a):** `launcher.py` always starts a launcher-state editor and opens an existing project with `project.open {path}` (as it already does for recovery), so the open-time writes are attributed to `project.open` with its transcript line; the transcript's pending mode already handles `project.open` (`connection.py`). Test: `Tools/MCP/tests/test_launcher.py`, an existing project opened through `project.open`, transcribed at its line.
- **`Editor --upgrade` (G2b), frozen header changes** (the M4 `EditorContext.h`, reviewed in this contract): `EditorContextSpecification::DeferAssetScan` (`bool`, false by default): `OpenProject` mounts `project://` and `cache://`, loads the provenance and stops before the asset scan, so opening writes nothing; the script services, creating `Assets/`, `EditorAssetManager::OpenProject` (the scan and its `.meta` writes), the `--strict` document check and the recent-list entry wait for `OpenDeferredAssets`, whose errors theirs become (`OpenProject`'s own comment says so). `[[nodiscard]] bool EditorContext::IsAssetScanDeferred() const` and `[[nodiscard]] Result<AssetRefreshReport> EditorContext::OpenDeferredAssets()`: it runs that part with the current attribution and returns the scan's report; errors: `InvalidState` without a project or when no scan is deferred, then `OpenProject`'s errors for that part (a `.luaurc` the type checker cannot read, a scan or `.meta` write failure, `--strict`'s `Validation` naming each file). A failure undoes only that part (the asset manager's project, the script services) and leaves the project open, with `project://` mounted and `IsAssetScanDeferred()` still true: `BatchRunner` transcribes a response through `project://` after the request returns, so closing the project inside `project.refreshAssets` would replace the request's own error with a transcript error and leave a request line without its response. The request fails with its own error and its response line, and `EditorApp` exits non-zero after the failed batch and closes the project as usual.
- **The CLI:** `EditorApp` sets `DeferAssetScan` for `--upgrade` only and runs the `cli` batch `[project.refreshAssets, project.upgrade]`, both transcribed (`BatchRunOptions{ .ClientName = "cli", .WriteTranscript = true }`): `project.refreshAssets` runs `OpenDeferredAssets` before its refresh while the scan is deferred and reports both, so the scan's `.meta` writes are attributed to it (`cli`, request id 1, transcript line 1), and `project.upgrade` is the second request (line 3). `refreshAssets` is synchronous, which also keeps the scan's imports out of the `ui` attribution. A deferred editor serves nothing before that request.
- **Tests (skipped, final names):** "EditorContext: a deferred asset scan writes nothing at open and runs under the attribution of OpenDeferredAssets" (with an unreadable `.luaurc` it first fails with that `Validation` error while `HasProject()` and `IsAssetScanDeferred()` stay true, then runs once the file is repaired) and "EditorApp: --upgrade defers the asset scan to a transcribed project.refreshAssets before project.upgrade" (and a failing `project.refreshAssets` ends the run non-zero with its own error, its response line transcribed with `ok` false). The existing "EditorApp: --upgrade appends its transcript lines and records provenance" is updated by stream C to the new line numbers.

### 12. G3: `project.upgrade` rewrites every canonical project file (stream C)

No header changes: `project.upgrade` (`ListUpgradeFiles`, `MakeCanonicalFile` in `ProjectMethods.cpp`) also rewrites every `.meta`, `.material`, `.sfx` and `.replay` file below `Assets/` through its reader (with its migrations) and canonical writer, each through `WriteProjectFile`, so provenance records `project.upgrade`; its dry run reports them in `changedFiles`. The method's description says so (the lead regenerates the references). Old-format fixtures for each type go under `Tests/Data/Formats/<Type>/` where an older version exists. Test (skipped, final name): "ProjectMethods: project.upgrade rewrites .meta, .material, .sfx and .replay files canonically and records them" (each file's document minified by another writer, restored to the editor's bytes, recorded, and a second upgrade changes nothing). The games suite's upgrade dry run (decision 8) is the canonical re-save check of §15.8 for these files too.

### 13. The other repository, editor and bridge gaps (stream C)

Each lands with a regression test; the names below are the required behaviour, and stream C may refine a name in its result.

- **G4:** the templates' `.gitignore` becomes `Library/`, `Build/`, `*.bak`; the root `.gitignore` adds `/Projects/*/Build/` and `/Tests/Projects/*/Build/` beside the `Library/` rules. Test: the template case of `ProjectManagerTests.cpp` checks the new bytes. It lands before the first builder session, because `project.create` copies the template when Breakout is created.
- **G5:** `ProvenanceRecorder::IsRecordedPath` accepts a root `.luaurc`, so `project.create` records it and the audit treats it as engine-consumed. Test: "ProvenanceRecorder: a root .luaurc is a recorded path"; the lead names `.luaurc` in §13.4 and §13.12.
- **G6:** `ProvenanceRecorder.h`'s and `WriteAttribution`'s comments and `ReferenceGenerator`'s schema description: `RequestId` is null for `ui` writes only; a `cli` entry carries its own request's id and transcript line. "ProvenanceRecorder: ui and cli writes have null request ids and transcript lines" becomes "ProvenanceRecorder: ui writes have null request ids and transcript lines". Stream C drafts the note on ADR 0008 decision 10; the lead applies it.
- **G7:** `.gitattributes` gains `-text` for `/Projects/*/Assets/**`, `/Projects/*/*.eproj`, `/Projects/*/.luaurc` and the same under `/Tests/Projects/`, so a fresh clone holds the editor-written bytes of every engine-consumed file of a demo. `script.write` does not normalize line endings (the repository-level rule covers every writer, `asset.import`'s `.gltf` copies included); the audit's `crlf` warning reports any CR LF text. Test: `test_provenance.py` checks `git check-attr text` for a demo path, and the audit's fresh-clone tests (decision 4) prove the effect.
- **G8:** `Scripts/ModuleRules.json`'s `Json.Exclude` gains `**/Automation/BuildLog.jsonl`, with a note that `AuditProvenance.py` validates transcripts (stream C is the file's integration owner this milestone). Test: a torn committed transcript is no Lint finding.
- **Torn transcript lines:** `TranscriptLog` completes a torn last line with `"\n"` before appending, like the bridge, so line numbers keep their meaning and no hand edit is suggested. "TranscriptLog: a file that does not end with a newline is rejected" becomes "TranscriptLog: a torn last line is completed and the next line numbered after it".
- **`engine_call` timeouts:** the bridge's `engine_call` uses the called method's own timeout from `rpc.discover` or the catalogue instead of 60 s (a long `project.upgrade` or `test.run` through `engine_call` no longer times out). Test in `Tools/MCP/tests/`.
- **DocExamples:** the Luau fences of `Tests/Projects/*/AGENTS.md` are type-checked like those of `Projects/*/AGENTS.md` (`Tests/Source/Support/DocExamples.cpp`).
- **G9: a test run's outcome must not depend on the import cache.** The contract gate's first Debug feature run failed "Formats: every asset under Assets/Formats loads with its type and path" with "script exceeded 500 ms in script.Test.Case (possible infinite loop)": `D:\Engine`'s FeatureTest `Library/` predated M15, so `Assets.Load` imported the textures synchronously inside the case and the import time counted against the script watchdog; the rerun with the cache warmed passed. A fresh clone, and the games suite's index copies (which have no `Library/`), would fail the same way. Stream C finds the root cause and fixes it in the engine or editor, not in the test: preferably a test run (`--run-tests`, `test.run`, the Runtime's `--feature-test` where it applies) and a play start do not begin until the project's pending imports have finished, and/or time spent in engine work a binding blocks on (a synchronous import) does not count against the script watchdog; stream C decides with evidence and records the choice. Regression test: a FeatureTest-like suite whose case loads uncached textures passes in Debug from a cold cache (a fresh copy without `Library/`), proven to fail without the fix.
- **FeatureTest regeneration** (decision 10): after G1, G4 and G5, `Scaffold.py --config Release` regenerates `Projects/FeatureTest`, and `Scaffold.py --check` reproduces it; no committed replay hash changes (none of these touches simulation).

### 14. Documentation and generated references (lead, at integration)

- Architecture §13.4 and §13.12 spell the provenance keys as the editor writes them (`Path`, `XXH64`, `Method`, `RequestId`, `Client`, `TranscriptLine`) and the provenance path as `<Game>/Automation/Provenance.json`, and §13.12 names the root `.luaurc`.
- §2.3 (the `AuditProvenance.py` and `Test.py` rows, PreCommit's list), §6 (the canonical-JSON test's scope with `Tests/Projects/`), §15.1 T5 (the games suite's gate), §15.8 (the games row, the lint row) and §15.9 (PreCommit's suites and static checks).
- AGENTS.md (a commands-table row for `AuditProvenance.py` and one for the games suite, the commit-gate list), the `build-and-test`, `commit-review` and `game-building` skills (the definition of done names `Test.py --suite games`).
- `Docs/ReviewChecklist.md` gains the §13.12 item: an engine change that alters canonical project output or simulation hashes upgrades every demo through automation, regenerates FeatureTest and re-records the affected replays in the same change, keeping the audit and the games suite green.
- The references are regenerated once after the streams merge (`project.upgrade`'s and `project.refreshAssets`' descriptions, `Provenance.schema.json`'s `RequestId` description).

### 15. The dry run (lead)

- **Builder:** one agent per session, using only the `mcp__engine__*` tools (loaded with ToolSearch) and `docs_get` for documentation. The only file it writes directly is `Tests/Projects/Breakout/AGENTS.md`, which provenance exempts. It never reads engine source. It keeps a friction log (every awkward step, unclear error, missing method, documentation gap and workaround it was tempted to make) and returns it with evidence. No other agent uses the bridge while a builder runs: one bridge process holds one editor connection, and in-process subagents share it.
- **Where:** the builder works in the `D:\Engine` main checkout, because the session's MCP bridge (`.mcp.json`, `Tools/MCP/run.py`) spawns `D:\Engine\bin\<Release|Debug>-windows-x86_64\Editor\Editor.exe`. Stream and fix agents work in isolated worktrees branched from the contract commit; the lead applies their staged diffs to `D:\Engine`.
- **Before session 1:** streams A, B and C are integrated (G4's template before `project.create` makes Breakout; G1 and G2 so the build is auditable), Debug, Release and Dist are rebuilt in `D:\Engine`, and the bridge is reconnected (its Python changed).
- **The game** (Roadmap M16): paddle, ball with physics, a brick grid from a prefab, score text, lives, synthesized sounds, win and lose; one test suite and one replay recorded from tick 0; its committed `Automation/BuildLog.jsonl` and `Automation/Provenance.json`.
- **Between sessions:** the lead triages the friction log into the friction list below; fix agents implement engine, editor, bridge and documentation changes with tests in the owning modules; the lead applies them, rebuilds, and reconnects the bridge when its code changed. A fix that changes canonical output triggers `project.upgrade` of Breakout through the bridge in the next session (decision 16).
- **Predicted friction is not pre-fixed:** the engine-capability and agent-workflow maps predicted friction (the table below). It is not fixed in advance, so the friction list records what an agent actually hits; the lead cross-references it, and every predicted item not hit is still dispositioned (fixed, or an ADR decision with its default) before the milestone closes.
- **Time box:** at most 4 builder sessions, one builder-agent invocation each, with their dates recorded; the milestone ends after the fourth whatever the polish of the game (Roadmap M16). What is left becomes friction items with their dispositions.

### 16. The operational rules

The dry run exercises each operational rule once and records it below with its transcript lines:

- **Attach:** the builder attaches to an editor that is already open on the project (started outside the bridge), as §13.8 describes.
- **`project.upgrade`:** once after an engine change that alters canonical output, through the bridge (or `Editor --upgrade`, which decision 11 makes auditable). If no canonical change arises within the time box, the record says so, and the exercise happens at M17's first canonical change; no change is contrived for it.
- **Re-recording:** one replay is re-recorded through automation (§13.6), with its new `FinalStateHash` committed and the games suite green.

### 17. Acceptance and the milestone gate

- `AuditProvenance.py --project Tests/Projects/Breakout` passes in index mode on the committed tree (a fresh clone; nothing read from `Library/`).
- `Test.py --suite games --require-gpu` passes in Debug, Release and Dist: `script.check` and `project.validate` clean, the upgrade dry run unchanged, the suite and the replay passing, the replay's final hash identical in both editors and the Dist export and equal to the committed one, and the plain Dist export's smoke test run and exiting 0.
- Every friction item is closed (a change with its regression test) or recorded as an ADR decision with the default in force, and every predicted item is dispositioned.
- Then the milestone review and its fixes, one `CI.py --milestone` on the final tree (ADR 0023), the milestone commit, a fast-forward of `main`, the push, and a pause for the product owner.

## Frozen interfaces

| File | Interface | Stream |
|---|---|---|
| `Engine/Source/Engine/AssetPipeline/AssetWriter.h` | `struct AssetWriteEvent { ...; uint64_t WriteContextId = 0; };` | C |
| `Engine/Source/Engine/AssetPipeline/EditorAssetManager.h` (+ `.cpp`) | `struct AssetWriteContext { uint64_t Id = 0; };`; `void EditorAssetManager::SetWriteContext(Ref<const AssetWriteContext> context);` | C |
| `Editor/Source/EditorCore/EditorContext.h` (+ `.cpp`) | `bool EditorContextSpecification::DeferAssetScan = false;`; `[[nodiscard]] bool EditorContext::IsAssetScanDeferred() const;`; `[[nodiscard]] Result<AssetRefreshReport> EditorContext::OpenDeferredAssets();` (G1 adds private state only) | C |
| `Scripts/AuditProvenance.py` | the CLI of decision 3; `resolve_project(argument: str) -> Path`, `audit_step(project: Path, files: str, console: Console) -> tuple[Step, provenance.ProjectAudit \| None]`, `self_test(fixtures: Path = PROVENANCE_FIXTURES) -> list[Step]`, `main(argv: list[str] \| None = None) -> int` | A |
| `Scripts/Lib/provenance.py` | `PROVENANCE_FILE`, `TRANSCRIPT_FILE`, `PROVENANCE_FORMAT`, `PROVENANCE_VERSION`, `EXEMPT_FILES`, `FILE_MODES = ("index", "disk")`, `DEFAULT_FILE_MODE = "index"`, `FINDING_CODES`, `WARNING_CODES`, `MISSING_FILE_CODES`, `EXIT_PASSED`, `EXIT_FINDINGS`, `EXIT_MISSING_FILE`; dataclasses `ProvenanceEntry(path, xxh64, method, request_id, client, transcript_line)`, `TranscriptLine(number, raw, record, terminated)`, `Transcript(lines)`, `Finding(code, path, detail)`, `ProjectAudit(project, files, counts, findings, warnings)`; `load_provenance(path: Path) -> list[ProvenanceEntry]`, `load_transcript(path: Path) -> Transcript`, `materialize_index(repository: Path, project: Path, destination: Path) -> Path`, `enumerate_engine_files(project_copy: Path, files: str = DEFAULT_FILE_MODE) -> list[str]`, `audit_project(project_dir: Path, files: str = DEFAULT_FILE_MODE) -> ProjectAudit`, `audit_exit_code(audit: ProjectAudit) -> int`, `audit_to_json(audit: ProjectAudit) -> dict[str, Any]` | A |
| `Scripts/Lib/xxh64.py` | `xxh64(data: bytes) -> int`, `xxh64_hex(data: bytes) -> str` | A |
| `Scripts/Lib/games.py` | `GAME_PROJECT_ROOTS`, `EXCLUDED_PROJECTS`, `TEST_MODES`, `DEFAULT_SUITE_MODES`, `REPLAY_SUITE_PREFIX`, `EDITOR_CONFIGURATIONS`, `EXPORTED_CONFIGURATION`, `REPORT_PREFIX`, `UPGRADE_COMMAND`; dataclasses `GameProject(directory, project_file, name)`, `SuiteEntry(script, scene, modes)`, `TestingSettings(suites, replays)`; `discover_projects(root: Path, files: str = "disk") -> list[GameProject]`, `read_testing(project_file: Path) -> TestingSettings`, `committed_hashes(project_directory: Path, replays: Iterable[str]) -> dict[str, str]`, `evaluate_run(document: dict[str, Any], mode: str, testing: TestingSettings, committed: dict[str, str]) -> list[str]`, `check_replays_matched(project_directory: Path, matched: Iterable[str]) -> list[str]`, `compare_game_hashes(committed: dict[str, str], runs: dict[str, dict[str, str]]) -> list[str]`, `check_script_check(result: dict[str, Any]) -> list[str]`, `check_validate(result: dict[str, Any]) -> list[str]`, `check_upgrade_dry_run(result: dict[str, Any], project: GameProject) -> list[str]`, `check_smoke(result: dict[str, Any], require_gpu: bool) -> tuple[list[str], list[str]]`, `editor_report_path(project: GameProject, config: str) -> Path`, `dist_report_path(project: GameProject, suffix: str) -> Path`, `junit_path(config: str) -> Path` | B |

The M4 `EditorContext.h` and the M6 `AssetWriter.h` and `EditorAssetManager.h` change here, and each change is reviewed in this contract. Every other frozen header stays as M15 left it.

## Script interfaces (frozen; implemented by the streams)

- `Scripts/AuditProvenance.py` (decision 3; exists in the contract, behaviour stubbed).
- `Scripts/Test.py --suite games [--config Debug,Release,Dist] [--junit] [--require-gpu] [--allow-skips]`, `run_games(configs, arguments, console) -> list[Step]` and `runtime_executable(config, build_root=paths.BIN_ROOT) -> Path` (decision 8).
- `Scripts/CI.py`: the `games` stage and `Runner.games(configs) -> list[Step]` (decision 8).
- `Scripts/PreCommit.py`: `games` in its Debug suite list (decision 8).
- `Scripts/Lib/scripts.py`: the two static-check steps (decision 9).
- `Tests/Data/Generate/MakeProvenanceFixtures.py [--check] [--json]` (decision 6).
- `Editor --headless --project P --upgrade`: the `cli` batch `[project.refreshAssets, project.upgrade]` over a deferred scan (decision 11).

## Data formats

- `Tests/Data/Provenance/<Case>/Project/` and `Tests/Data/Provenance/<Case>/Expected.json` (`{"Format": "ProvenanceAuditFixture", "Version": 1, "ExitCode", "Findings": [{"Code", "Path"}], "Warnings": [{"Code", "Path"}]}`, decision 6).
- `AuditProvenance.py --json` (decision 3) and `provenance.audit_to_json` (`{"project", "files", "passed", "counts": {"files", "entries", "transcriptLines", "requests"}, "findings": [{"code", "path", "detail"}], "warnings": [...]}`).
- Reports: `bin/TestResults/Games-<Config>.xml`, `Games-<Name>-Editor-<Config>.json`, `Games-<Name>-Dist.{json,xml}`; packages `bin/Exports/Dist/<Name>/` (decision 8).
- Unchanged: `Provenance.json` (ADR 0008 decision 10; G6 corrects only the `RequestId` wording) and `BuildLog.jsonl` (ADR 0008 decision 11).

## Ownership

Streams own disjoint files, except the named hunks of a shared file, and nothing else in it. A stream that needs a change in another stream's or the lead's file lists it in its result; a defect found in a file no stream owns goes to the stream that found it.

**Stream A (audit):**
- `Scripts/AuditProvenance.py`, `Scripts/Lib/provenance.py`, `Scripts/Lib/xxh64.py`
- `Tests/Data/Generate/MakeProvenanceFixtures.py`, `Tests/Data/Provenance/**`
- `Scripts/Lib/scripts.py`: the two static-check steps (decision 9)
- `Tests/Source/Engine/Core/Json/AuthoredFilesTests.cpp`: the `Tests/Data/Provenance/` exemption
- `Tests/Automation/test_audit_provenance.py` (its `BridgeAuditTests` pass once stream C's G1, G2a, G2b and G5 land: the Empty template's root `.luaurc`, which `project.create` writes, is engine-consumed and is recorded only with G5; the lead runs them at integration)
- drafts of the AGENTS.md command row and the §2.3 row in its result (the lead applies them)

**Stream B (games suite and stage):**
- `Scripts/Lib/games.py`
- `Scripts/Test.py` (the suite, `run_games` and the refactors of decision 8)
- `Scripts/CI.py`: the games stage (`Stage`, `Runner.games`, `TIMEOUTS`, the docstring and `--gpu-optional` help)
- `Scripts/PreCommit.py`: games in its Debug suites, the docstring and help
- `Tests/Source/Engine/Core/Json/AuthoredFilesTests.cpp`: the `Tests/Projects/` walks
- `Tests/Automation/test_games_suite.py`; the games hunks of `test_testing_export.py` and `test_ci_script.py`

**Stream C (editor, bridge and repository gaps):**
- `Engine/Source/Engine/AssetPipeline/{AssetWriter.h, EditorAssetManager.h, EditorAssetManager.cpp}` (G1)
- `Editor/Source/EditorCore/EditorContext.{h,cpp}` (G1, G2b)
- `Editor/Source/Editor/EditorApp.cpp` (G2b)
- `Editor/Source/EditorCore/Automation/ProjectMethods.cpp` (G2b's `project.refreshAssets`, G3)
- `Editor/Source/EditorCore/Automation/{ProvenanceRecorder.{h,cpp}, TranscriptLog.{h,cpp}}` (G5, G6, torn lines)
- `Editor/Source/EditorCore/Reference/ReferenceGenerator.cpp` (G6's schema description)
- `Resources/Templates/Projects/*/.gitignore`, the root `.gitignore`'s `Build/` rules (G4), `.gitattributes` (G7), `Scripts/ModuleRules.json` (G8; its integration owner this milestone)
- `Tools/MCP/**` (G2a's launcher, `engine_call`'s timeouts, their tests)
- `Tests/Source/Support/DocExamples.cpp` (the `Tests/Projects` scan)
- `Projects/FeatureTest/**`, only through `Scaffold.py` (decision 13)
- `Tests/Data/Formats/**` (G3's old-format fixtures)
- tests: `Tests/Source/Engine/AssetPipeline/EditorAssetManagerTests.cpp`, `Tests/Source/EditorCore/EditorContextTests.cpp`, `Tests/Source/Editor/EditorAppTests.cpp`, `Tests/Source/EditorCore/Automation/{ProjectMethodsTests, ProvenanceRecorderTests, TranscriptLogTests}.cpp`, `Tests/Source/EditorCore/Project/ProjectManagerTests.cpp`, `Tests/Source/EditorCore/Scripting/DocExampleTests.cpp`, `Tests/Automation/test_provenance.py`, `Tools/MCP/tests/**`

**Lead (integration, the dry run, completion):**
- this ADR (its friction list, time box and operational-rule records), `Docs/Architecture.md`, `Docs/Roadmap.md`, `AGENTS.md`, `Docs/CodeStyle.md`, `Docs/ReviewChecklist.md`, `Docs/Decisions/**`, `.claude/skills/**`
- `Docs/Reference/*`, `Tools/MCP/catalog.json`, `Resources/Scripting/Engine.d.luau` (regenerated once after merging)
- the premake files, `BuiltinComponents.h`, `RegisterBindings.cpp`, `RegisterMethods.cpp`, `.github/workflows/ci.yml`, the rest of `CI.py`, `PreCommit.py` and `Scripts/Lib/scripts.py`
- the dry run: the builder sessions (`Tests/Projects/Breakout/**` is written through the bridge only, its `AGENTS.md` by the builder), the triage, and the assignment of each friction fix to the owning module's files

## Approvals

None needed. Git is already a required tool (`Setup.py`); nothing is downloaded or vendored; `gh` is not needed while the catch-up is deferred (decision 1).

## Risks

1. **Friction volume against the time box.** Four sessions are the limit; what is unfinished becomes friction items with dispositions, never an extended box.
2. **One shared bridge.** Only the builder loads the engine tools while it runs; fix agents never use the bridge, and worktrees have no `.mcp.json`.
3. **Stale binaries.** The Release and Dist binaries predate M14; Debug, Release and Dist are rebuilt on the integrated tree before session 1 (the bridge spawns the Release editor, and `GenerateDocs.py --check` uses the Debug editor).
4. **A canonical change during the build** forces, in the same change, `project.upgrade` of Breakout through automation, a FeatureTest regeneration and the affected re-recordings (§13.12, decision 14's checklist item).
5. **Fresh-clone differences.** Index mode, the `-text` rules (G7) and the `crlf` warning keep a clone's bytes equal to what provenance hashed; an untracked file is not audited until it is staged.
6. **Cross-stream test.** The bridge audit test needs A and C (G1, G2a, G2b and G5: without G5 it fails with `entry-missing .luaurc`, which is no G1 or G2 regression); the lead runs it at integration, and stream A verifies the rest against its fixtures.
7. **The deferred scan** exists for `--upgrade` only; an editor with a deferred scan serves nothing before `project.refreshAssets`.
8. **Remote drift grows** (decision 1): Linux and macOS failures accumulate until the catch-up; the clang-cl builds of the portability stage catch the most frequent class locally.

## Verification plan (ADR 0023)

- **Streams never run `PreCommit.py` or `CI.py`.** Each builds what it changed, runs the affected cases, shows each regression test failing against the reverted code by running that case alone, and finally runs once the suites its change can affect:
  - **A:** `test_audit_provenance.py` (the bridge class after C), the generator's `--check`, `AuditProvenance.py --self-test`, `--demos --files disk` run once by hand, `Lint.py --paths` on its files.
  - **B:** `test_games_suite.py`, `test_testing_export.py`, `test_ci_script.py`, and one `Test.py --suite games --config Debug,Release,Dist --require-gpu` over a temporary game: the suite has no project option and audits and runs staged projects only (decisions 7 and 8), so B builds a small game through automation (as `test_games_suite.py`'s end-to-end test does) under `Tests/Projects/<Name>/` in its worktree, stages it (`git add`), runs the suite, then unstages and removes it (`git rm -r --cached`, then deletes the directory) before handing back; its result names the game and the run.
  - **C:** the C++ cases of its tests in Debug and Release, `test_provenance.py`, `Tools/MCP/tests`, `Scaffold.py --check` after the regeneration.
- **The contract:** the lead runs `PreCommit.py --contract` once.
- **Integration:** the lead merges the streams on the contract commit, wires what remains, regenerates the references, rebuilds Debug, Release and Dist, and makes each new stage green with targeted runs before session 1. The milestone review and its fixes come before the single `CI.py --milestone` on the final tree.

## Friction list

Every awkward step, unclear error, missing method, documentation gap or tempted workaround a builder session reports, triaged by the lead. A resolution is either a change with its regression test or an ADR decision with the default in force; the milestone review blocks on an open row.

| Id | Session | Symptom and evidence (BuildLog line, tool error, code) | Predicted | Owning module | Resolution (change and regression test, or ADR decision with the default in force) |
|---|---|---|---|---|---|

## Predicted friction

From the engine-capability and agent-workflow maps (`bin/Reviews/M16-Map.json`, which holds the evidence); not fixed in advance (decision 15). Each row is dispositioned before the milestone closes: hit (its friction id), or not hit and fixed, or an ADR decision with its default.

| Id | Predicted item | Hit as | Disposition |
|---|---|---|---|
| P01 | Bricks merged into one compound body by `PHYSICS_ADJACENT_STATIC_BODIES`' auto-fix and the skill's level-root pattern, so brick callbacks reach only the root | | |
| P02 | A partner's collision callback meets an entity destroyed earlier in the same dispatch | | |
| P03 | Speculative contacts reach scripts, so bricks break without a bounce | | |
| P04 | Ball defaults (restitution, friction, damping) lose speed or curve the path | | |
| P05 | A kinematic paddle passes through the static walls | | |
| P06 | `Scene.Reload` and restart in editor play (the saved scene file, two transitions in one frame) | | |
| P07 | `prefab.instantiate` without per-instance overrides, so a brick grid needs about 100 hand-indexed batch ops | | |
| P08 | Bulky tool text and absolute paths in the committed transcript | | |
| P09 | Relative project paths depend on the bridge's working directory | | |
| P10 | The tool list does not refresh after an engine or bridge change | | |
| P11 | A Dist smoke test that did not run (no device, renderer none) is easy to miss | | |

## Time box record

At most 4 builder sessions (decision 15), one builder-agent invocation each.

| Session | Date | Builder invocation | Reached | Friction ids | Rebuilt or reconnected after |
|---|---|---|---|---|---|
| S1 | | | | | |
| S2 | | | | | |
| S3 | | | | | |
| S4 | | | | | |

## Operational rules record

Decision 16; each with its session and transcript lines (`Tests/Projects/Breakout/Automation/BuildLog.jsonl`).

| Rule | Session | Transcript lines | Evidence |
|---|---|---|---|
| Attach to an editor already open on the project | | | |
| `project.upgrade` after an engine change that alters canonical output (or: none arose within the time box; done at M17's first canonical change) | | | |
| One replay re-recorded through automation | | | |

## Before milestone completion

- [ ] Every `ENGINE_CONTRACT_STUB` and every skipped test of this contract is replaced; one strict `CI.py --milestone` run is green on Windows, the games stage and the clang-cl builds included.
- [ ] `AuditProvenance.py --project Tests/Projects/Breakout` passes in index mode on the committed tree; `--self-test` passes in the static checks; the bridge audit test passes.
- [ ] `Test.py --suite games --require-gpu` passes in Debug, Release and Dist, with the replay's hash identical in both editors and the Dist export and equal to the committed one, and the plain Dist export's smoke test run and exiting 0.
- [ ] G1 to G8, the torn-line completion, `engine_call`'s timeouts and the DocExamples scan landed with their regression tests; FeatureTest is regenerated and `Scaffold.py --check` reproduces it.
- [ ] Every friction item is closed or recorded as an ADR decision with its default, every predicted item is dispositioned, the time box record has at most four sessions with their dates, and each operational rule is recorded (or the `project.upgrade` fallback is).
- [ ] The references are regenerated, and decision 14's documentation (Architecture, AGENTS.md, the skills, the review checklist, ADR 0008 decision 10's note) describes what landed.
- [x] Decision 1's amendments are in the tree (Roadmap M16, AGENTS.md, Architecture §15.8, §17 and §18, the notes on ADR 0011, ADR 0024 decision 20 and ADR 0025 decision 15).

## Architecture and Roadmap amendments

This contract amends Roadmap M16 (the scope sentence, and the parallelization line's pointer to this ADR), AGENTS.md's remote-CI paragraph, Architecture §15.8, §17 and §18, and adds the superseded notes of decision 1. The lead amends the rest at integration (decision 14).

## Lead decisions on the contract's open questions

These answer the questions the contract draft raised; the original wording follows each answer.

- **1:** Roadmap M16's "one test suite and one replay" is a minimum. The builder follows the `game-building` skill; an Editor-only autopilot suite that records the replay counts in addition, and if the skill's rule proves awkward for Breakout the builder reports it as friction.
- **2:** drop the static-check audit of the demos; the games suite's index-mode audit is the one audit per gate (decision 9 as amended above).
- **3:** the replay must include at least one paddle hit, one brick broken and one life lost; if the game restarts through `Scene.Load`, it also includes one restart, so the editor-to-Dist hash comparison covers a scene reload.
- Also: an import the UI scheduled stays attributed to `ui` even when it is published during a request (attribution follows whoever scheduled the work); the stream C items without a frozen interface get their tests in the stream, not as skipped contract tests; the superseded-notes on ADRs 0011, 0024 and 0025 are accepted.

Original questions:

1. **The skill's autopilot rule against Roadmap M16's "one test suite":** the `game-building` skill's definition of done asks every level a physics ball plays for an autopilot suite and a recorded replay, which for Breakout means a second, Editor-only suite. Either the skill is amended (autopilot only where a goal must be reached, as in Rolling Ball) or the dry run counts the autopilot suite separately; until decided, the builder follows the skill and the conflict becomes a friction item.
2. **Two audits per PreCommit** (decision 9): the plan keeps the static check (disk mode) and the games suite's audit step (index mode); confirm, or drop one.
3. **Breakout's replay content:** whether the replay must include a lost life and a restart (`Scene.Load`), so the games suite proves the editor and Dist hashes across a scene reload.

## Review dispositions

None yet.

## Integration

Filled in by the lead when the streams are merged.
