# 0024 — M15 export, hardening and completion contract

- **Status:** accepted for the contract commit. Two contract reviews (a spec lens and a design lens) were applied by the lead's fix pass; their dispositions are in "Review dispositions". The contract gate runs once (`PreCommit.py --contract`, ADR 0004, ADR 0023); every later commit is strict. Decision 14 waits for the product owner (see "Open for the operator").
- **Date:** 2026-10-13.
- **Base:** `16f1c80` (M14 complete; ADR 0022, ADR 0023).
- **Authority:** Roadmap "Rules for every milestone" (rule 3, the contract task) and M15; Architecture §2.2, §2.3, §4.1, §12.1, §13.4, §13.9, §14, §15.1, §15.2, §15.8, §15.9 and §16; ADR 0004, ADR 0011, ADR 0012 decisions 14, 16 and 25, ADR 0019, ADR 0021, ADR 0022 and ADR 0023. Remote CI stays non-blocking (ADR 0011).

## Context

M15 completes §14 (export), runs FeatureTest in its three modes and the determinism stage over all four runs, adds the T6 hardening stage, and ends with a completion review of every Architecture section. Four parallel streams implement it (Roadmap M15 "Parallelization"):

- **A:** the exporter's remaining steps and entry points, packaging, executable resources, the macOS bundle and the notices file.
- **B:** testing exports, the Dist replay player's end-to-end proof, the three-way FeatureTest, the export stage and the exported determinism runs.
- **C:** the hardening stages: AddressSanitizer, the long fuzz and the soak.
- **D:** portability, the Dist map-file check and stripping, the perf capture and the Runtime's F1 overlay.

Four read-only state maps (export, testing exports, hardening, portability and completion) established the starting point. The facts this contract relies on were checked again in the code:

- `ExportSpecification` had no `Testing` member; `Exporter::WriteManifest` hard-coded `manifest.Testing = false`; `CollectSources` had no force-include path; and `project.export` refuses any present `testing` param, `false` included (`ExportMethods.cpp`).
- `ExportSpecification::ExportRoot` exists, and `LocateOutput` honours it, but nothing set it. `Editor --export`, `--export-root` and `Scripts/Export.py` do not exist.
- `ResolveExecutableResources` (M14) validates Export.Icon, Version and Company, and the exporter stores the result without using it. There is no `.rc`, `.ico`, `.icns` or `Info.plist` anywhere, so §14.2's "the engine's default icon stays" described nothing.
- No notices file, generator or exporter step exists. The `Vendor/*/VENDOR.md` header tables are not uniform.
- `Runtime/premake5.lua` emits the map file in every configuration (`/MAP` in the Link `AdditionalOptions` on Windows; there is no `GenerateMapFile` element) and the macOS rpath, but had no Linux `$ORIGIN` RPATH.
- The built Dist `Runtime.map` (MSVC with LTCG) holds no automation, Luau Analysis or AssetPipeline symbol, but about 3,500 ImGui lines, because `Application.cpp` creates `ImGuiLayer` behind a run-time flag (ADR 0012 decision 25). It also links Luau's parser and compiler objects.
- The exporter's glob (`Utils::MatchesExportGlob`), the Runtime's `MatchesGlob` and `Scripts/Test.py`'s `glob_regex` are three copies of one rule.
- The Runtime's `--feature-test` runner, the cooked-replay player and the test-mode semantics (§14.4) are implemented and unit-tested, but no testing export of FeatureTest has ever run in Release or Dist.
- `Test.py` has no export suite, accepts `--config` Debug and Release only, and its determinism suite runs the two editors. `CI.py` lists `export` and `hardening` as unavailable.
- There is no sanitizer option. Premake 5.0.0 supports `sanitize { "Address" }`; generated with the option added by this contract, 15 of the 16 projects (every one but the Shaders utility project) get `<EnableASAN>true</EnableASAN>` in Release only. MSVC 14.51 ships `clang_rt.asan_dynamic-x86_64.dll`.
- Fifteen seeded mutation cases exist, at 1,000 to 10,000 iterations each; about twenty readers of external input have none. The Debug unit suite spends about 175 s of its 746 s in them; the binding fuzz alone takes 115 s in Debug and 7 s in Release.
- No process-memory query exists, in the engine or the scripts.
- The Runtime has no F1 overlay; nothing enables ImGui in it. No engine option `--log-level` exists, although §13.9 says Dist honours one.
- The M7 exporter already cooks a project environment from a bake that a GPU editor stored in `Library/Cache` when a `--renderer none` editor exports it (the contract's GPU-suite case proves it).

## Decisions

### 1. What the contract freezes, and what it implements

The contract freezes the C++ headers under "Frozen interfaces", the data formats under "Data formats", the script CLIs under "Script interfaces" and the Python module APIs of the new `Scripts/Lib` modules.

- **C++ stubs:** every new function's first statement is `ENGINE_CONTRACT_STUB();`. A stub returning `Result` or `Status` returns `Unsupported`; any other returns an empty or neutral value. Two private exporter phases (`PatchResources`, `Sign`) are pass-through stubs that complete at once, so exports keep their M7 behaviour until stream A lands them; the marker still fails the strict gate until then.
- **Acceptance tests:** the Roadmap's verbatim names, marked `doctest::skip(true)`; each body fails when forced (verified with `--no-skip`): it calls a stub or asserts behaviour that does not exist yet.
- **Python:** stubs raise `NotImplementedError("contract stub ...")`; their tests carry `unittest.skip` (ADR 0008 decision 16).
- **Scripts:** nothing is added to `Test.py`, `CI.py`, `Build.py`, `Generate.py`, `CheckBuildConfig.py` or `PreCommit.py`, and `Export.py`, `Hardening.py`, `Perf.py` and `CheckRuntimeMap.py` do not exist yet. A stub there would fake a result or break the static checks, which run in contract mode too; their CLIs, and the `Build.py` helper names the skipped tests call, are frozen here only.
- **Implemented by the contract** (inert unless used, so the streams can build on them from the start, as ADR 0022 did with `--id-seed`):
  - the premake option `--sanitize=address` (decision 12), its `/STACK:2097152` for the sanitized Release executables (risk 12), and the Linux `-Wl,-rpath,'$$ORIGIN'` of the Runtime (decision 18), each checked by generating into a scratch directory; and `version` in the Tests links on Windows (decision 4);
  - `Exporter::GetDefaultOutputDirectory`'s testing variant and `ExportPhaseToString` of the two new phases, with their tests;
  - the testing call sites in `Exporter.cpp` (decision 6): `CollectTestingExportContent` in Validate, the suite scenes' strict load, `IsForcedByTestingExport` in `CollectSources`, `Game.json`'s `Testing` and the report's testing fields; stream B implements only the callees;
  - `project.export` passing `AutomationServerSpecification::ExportRoot` on (`ExportMethods.cpp`), with an in-process test that writes under a root (decision 3);
  - `EditorCore/Export/ExportConfiguration.h`, the enum on its own, so `EditorCommandLine.h` no longer includes the whole exporter;
  - `EditorLaunchOptions::IsOneShot` counts `--export`;
  - the shared exporting-test fixture `Tests/Source/Support/ExportBinaryRoot.{h,cpp}` (decision 2), already used by every exporting test B owns and by every M15 exporter case;
  - `coverage.evaluate_feature_report`'s `expected_mode` (decision 9) and the shared path-glob vectors `Tests/Data/PathGlob/Cases.json` with their Python test against `Test.py`'s `glob_regex` (decision 7), both with passing tests;
  - the `testing` refusal check moves unchanged from `test_export.py` into stream B's `test_testing_export.py`, and the C++ refusal becomes its own case for B to replace (decision 6).
- **No existing behaviour changes**, and no generated reference changes (`GenerateDocs.py --check` passes on the contract tree).

### 2. Exporter phases, report and validation (stream A)

- **Phases**, in order: `Validate`, `Cook`, `WritePaks`, `CopyRuntime`, `PatchResources`, `WriteManifest`, `Sign`, `SmokeTest`, `MoveToOutput`, `Done`. `PatchResources` follows §14.2 step 5. `WriteManifest` writes `Game.json` and `THIRD_PARTY_NOTICES.txt` (step 6). `Sign` comes after everything the macOS signature seals and before the smoke test runs the signed bundle.
- **Report:** `ExportReport` gains `Testing`, `TestSuiteCount`, `ReplayCount` (decision 6), `ResourcesPatched` (decision 4) and `PhaseTimings`: one `ExportPhaseTiming` per phase entered, in phase order from `Validate` to `MoveToOutput`, a phase with nothing to do on the host or for the specification included (§14.2 step 8 "timings"). `Seconds` stays the total.
- **Validation under `--renderer none`** (§14.2 step 1): an editor without a graphics device reports every included Environment source that has no current cooked bake as a Validation issue naming the asset, with `EnvironmentImporter::GpuHint`, instead of failing in Cook with `Unsupported`. With a bake in `Library/Cache` the export cooks from it, which the unskipped GPU-suite case "Exporter: an editor without a GPU exports a project environment from the bake a GPU editor cooked" keeps true.
- **Output directory:** at the end of Validate, an existing output directory must be empty or hold an earlier export (a `Game.json`, or on macOS a `<Name>.app` holding `Contents/Resources/Game.json`); otherwise the export fails with `InvalidArgument` naming it and writes nothing. This holds for every output (project-relative, `ExportRoot` and `OutputPath`), so no export replaces a directory that is not a package.
- **Missing build outputs:** step 4's `NotFound` errors (the Runtime, its shaders, its Redist DLLs, the notices file, the macOS frameworks and ICD manifest), and no other, carry the missing path as `ErrorLocation::File` and the build command; `Exporter::IsMissingBuildOutput(const Error&)` recognizes them (decision 3's exit code 3).
- **Excluded scripts:** a plain export still fails on type errors and Error-level asset diagnostics of excluded content (`Assets/Tests/**` included), because one project must export in both kinds. This is documented, not changed.
- **Notices:** `LocateBinaries` requires `bin/<OutputDir>/Runtime/THIRD_PARTY_NOTICES.txt` like the Redist DLLs (`NotFound` naming `Build.py` when missing); `WriteManifest` copies it into the package (decision 8).
- **Dist symbols** (§2.2 "archived in `bin/Symbols`, never shipped"): `Build.py` copies the Dist Runtime's `.pdb` and `.map` (dSYM and `.map` on macOS, `.map` on Linux) into `bin/Symbols/<OutputDir>/Runtime/` after each Dist build (`Build.archive_dist_symbols`). The exporter never packages them. `/PDBALTPATH:%_PDB%` for the Dist Runtime, so the shipped executable no longer names the build machine's path, is a premake change the lead applies at integration, where the Dist build that `CI.py` runs checks it.
- **The shared fixture** (review finding 1): every test that runs an export past Validate builds its bin directory with `Test::MakeExportBinaryRoot` (`Tests/Source/Support/ExportBinaryRoot.h`): a copy of this build's real Runtime output as `Build.py` leaves it (the executable, `Redist/` on Windows, `Frameworks/` and `Vulkan/` on macOS once A's `Build.py` stages them) and the notices file (optional, for the missing-notices case). So what A makes the exporter require (a real PE for `PatchResources`, the notices file, real dylibs for `Sign`) holds in B's tests from day one, and nothing passes in one stream only to fail at integration. The contract moved `TestingExportTests`, `ExportResourcesTests`, `ExportMethodsTests` and every M15 `ExporterTests` case to it. Stream A moves the M7 to M14 `ExporterTests` cases from their fake executable in the same change that makes `PatchResources` and the notices requirement real, and replaces `test_export.py`'s byte-equality check with the resource probe on Windows and the bundle layout on macOS.

### 3. Export entry points: `Editor --export`, `--export-root`, `OutputPath`, `Export.py` (stream A)

- **`ExportSpecification::OutputPath`:** an absolute package directory that overrides `OutputDirectory` and `ExportRoot`, with the same staging and replacement rules (decision 2's output-directory rule included). `Exporter::Start` refuses it with `InvalidArgument` (review finding M1) when it is relative; when it is a file-system root; and when it equals, or is an ancestor of, the project's root, `BinaryRoot`, the process's working directory or the user-data root (`Paths::GetUserDataRoot`). Until stream A lands it, `Start` refuses any set `OutputPath` with `Unsupported` (`CheckOutputPath`).
- **`ExportRoot` semantics:** it replaces `<Project>/Build/`, so the export goes to `<ExportRoot>/<OutputDirectory without "Build/">`, or to `<ExportRoot>/<Platform>-<Config>[-Testing]/<Name>` when `OutputDirectory` is empty. Today `LocateOutput` ignores `OutputDirectory` under a root, which A corrects.
- **`Editor --export <dir> --config Debug|Release|Dist [--testing] [--smoke-test] [--export-report <file.json>]`**, with `--project`, not `--read-only`: a one-shot run (headless or windowed, polled once per frame) that exports into `<dir>` (`OutputPath`, made absolute against the working directory) from `ENGINE_REPO_ROOT/bin`.
  - Exit codes: 0 exported; 1 the export failed (a project asset's `NotFound` included); 3 the project does not open, its lock is held, or a build output is missing (`Exporter::IsMissingBuildOutput`); 2 usage.
  - `--export-report` is written whatever the outcome, as canonical JSON: `{"success": true, "result": <project.export's result>}` or `{"success": false, "error": {"code", "message", "issues": [{"pointer", "message", "hint"}]}}`.
  - Refused combinations: `--config`, `--testing`, `--smoke-test` or `--export-report` without `--export`; `--export` without `--config` or `--project`, with `--read-only`, with another one-shot mode, with `--automation` or with `--export-root`.
- **`Editor --export-root <dir>`:** for a serving editor, made absolute; `EditorApp` sets `AutomationServerSpecification::ExportRoot`, which `project.export` passes on (wired by the contract). It closes §13.2 and §13.4.
- **`Scripts/Export.py --project P --config Debug|Release|Dist --out D [--testing] [--smoke-test] [--editor-config Debug|Release] [--renderer vulkan|none] [--user-data-dir U] [--timeout S] [--json]`** wraps the CLI.
  - It uses `bin/<editor config>/Editor` (Release by default), passes a temporary `--export-report`, and prints a summary (with `--json`, the report document plus `"exitCode"`).
  - Its exit code is the editor's (0, 1 or 3); 2 is usage, 5 a timeout (the editor is stopped).
- **`--validate` and `--timeout` are not added** (settled, review finding 23): one-shot validation is `Editor --batch` with a `project.validate` line, and timeouts belong to the calling scripts (`Test.py`, `Export.py --timeout`, which exit 5). §12.1's CLI and §4.1's exit code 5 are amended in this contract, so `EditorCommandLine.h` gains no field.
- **Tests:** the exit-code-3 cases (a project that does not open, a held lock) and `--export-root` through `project.export` are frozen in `test_export_cli.py`; the classifier in `ExporterTests` ("Exporter: only a missing build output counts as one"), because `Editor --export` always exports from the checkout's bin directory and a missing build output cannot be staged there.

### 4. Executable resources (stream A)

- **`Platform/ResourcePatcher.h`** (layer 1, Core only): `ExecutableIconImage`, `ExecutableVersionInfo`, `ExecutableResourceSet`, and `ResourcePatcher::{BuildVersionInfo, BuildIconGroup, Patch}`. The header documents the exact `VS_VERSIONINFO` and `GRPICONDIR` layouts, the ids (group 1, icons 1..n, version 1), language 0x0409 and code page 1200.
  - The builders are host-independent (`ResourcePatcher.cpp`) and deterministic, with no timestamps.
  - `Patch` is `Platform/Windows/ResourcePatcherWindows.cpp`: `BeginUpdateResourceW` without deleting existing resources, so `RT_MANIFEST` stays; a failed update is discarded; a sharing violation is retried within a bounded wait of 10 s. On Linux and macOS (`Platform/Posix/ResourcePatcherPosix.cpp`) it is `Unsupported`.
  - The file is named `ResourcePatcherWindows.cpp` after the `<Unit>Windows.cpp` convention; §14.2's path and Roadmap M15's deliverable are amended.
- **`BuildExecutableResourceSet`** (`EditorCore/Export/ExecutableResources.h`) makes one image per `ExecutableIconSizes` entry (16, 24, 32, 48, 64, 128, 256): the decoded Export.Icon reduced with `DownscaleImage` and encoded with `EncodePng`. The version strings are `CompanyName` = Company and `ProductName` = `FileDescription` = `InternalName` = Name, with `OriginalFilename` `<Name>.exe`.
- **Square icons:** `ResolveExecutableResources` adds the rule that Export.Icon is square, because icon images are square and padding would change the author's picture. The M14 case that accepts a 256 × 300 icon moves that icon to its refused list when this lands. Its gate-8 fixture is already square.
- **No default icon:** without Export.Icon no icon group is written, and the executable shows the platform's default application icon (macOS omits `CFBundleIconFile`). §14.2's "the engine's default icon stays" is amended, because no engine icon exists to keep.
- **Oracle:** the acceptance test reads the patched file back through Windows' own parser, `Test::ProbeExecutableResources` in `Support/PlatformProbes` (`GetFileVersionInfoW`, `VerQueryValueW`, `LoadLibraryExW` as a data file). The test then runs the patched executable headless.

### 5. macOS bundle (stream A)

- **Layout:** the output directory holds only `<Name>.app`. `ExportReport::Executable` is `<out>/<Name>.app/Contents/MacOS/<Name>`. Everything §14.1 lists, the notices included, lives in `Contents` (`AppBundle.h` documents the paths).
- **Host-independent code** (review finding 19), unit-tested on every OS, so the local Windows gate builds and runs it (`EditorCore/Export/AppBundle.h`):
  - the pure writers: `MakeBundleIdentifier` (`com.<company>.<name>`, sanitized deterministically), `WriteInfoPlist` (keys and order frozen, `LSMinimumSystemVersion` 14.0), `BuildIcns` (big-endian `icp4`/`icp5`/`icp6`/`ic07`/`ic08`/`ic11`/`ic12`/`ic13` PNG entries) and `RewriteMoltenVkIcd` (`library_path` = `../../../Frameworks/libMoltenVK.dylib`);
  - the assembly steps: `CopyAppBundleBinaries` (`CopyRuntime`'s part: the renamed Runtime, `bin/<OutputDir>/Runtime/Frameworks/{libvulkan.1.dylib, libMoltenVK.dylib}` and the rewritten `Runtime/Vulkan/icd.d/MoltenVK_icd.json`; a missing input is the build output's `NotFound`) and `WriteAppBundleResources` (`PatchResources`' part: `Info.plist` and, with an icon, `AppIcon.icns`).
- **macOS-only:** the exporter calls the assembly steps on macOS, and `Sign` runs `codesign --force --sign -` on the dylibs and then on the bundle, through `Process`. §13.4's "no method executes processes" is already relaxed for the smoke test, and §14.2 now names `codesign` too. These few lines are verified by the macOS CI job (ADR 0011).
- **Loader lookup** (review finding m11): `vk::detail::DynamicLoader` opens a bare leaf name, which dyld does not resolve through `LC_RPATH`, so stream A makes the Runtime load `<executable directory>/../Frameworks/libvulkan.1.dylib` by its explicit path when that file exists, and the leaf name otherwise (`Engine/Graphics/VulkanDispatch.cpp`, A's hunk). The macOS CI job's run of an exported bundle verifies it (completion checklist).
- **`Build.py`** stages the SDK's files on macOS (`Build.stage_macos_frameworks(root, config, sdk)`) only from the pinned SDK: it compares the SDK's header version with `Scripts/Lib/Toolchain.json` (the check `Setup.py` makes) and refuses another, and its `--json` report records each staged file's SHA-256 (review finding 10). The exporter reports `NotFound` naming `Build.py` when the files are missing.
- **Testing runs:** the bundle's testing user data goes beside the `.app` (`GetTestingUserDataRoot`, decision 6), so a test run never modifies the signed bundle.

### 6. Testing exports (stream B)

- **Content** (§7.6): a testing export packages, whatever `Export.Exclude` says:
  - every registered main asset below `Assets/Tests/`;
  - every `Testing.Suites` Script and non-empty Scene;
  - every registered `.replay` that a `Testing.Replays` glob matches.

  Nothing else is pulled in automatically: a packaged asset's reference to an excluded asset fails `CheckReferences` with the `Export.Exclude` hint, as it does today. `TestingExport.h` (`CollectTestingExportContent`, `IsForcedByTestingExport`) holds the rule, and the contract wires its calls in `Exporter.cpp`.
- **Validation** (§14.2 step 1):
  - each `TEST_SUITE_INVALID` diagnostic becomes an issue at `/Testing/Suites/<i>/Script` or `/Scene`, its message starting with `TEST_SUITE_INVALID: `;
  - a wildcard-free `Testing.Replays` entry that names no registered replay is an issue at `/Testing/Replays/<i>`;
  - every matched replay is loaded through the editor's asset manager, so `ReplayImporter` compiles each expectation. A failed import is an issue at `/Testing/Replays/<i>` with the located compile error;
  - every suite scene loads strictly with the build scenes (review finding n1; wired by the contract), because the export packages it and the Runtime loads it.

  A glob that matches nothing is not an issue, as at run time.
- **One suite rule:** `ProjectValidator::CheckTestSuites` holds the M13 loop, and `Validate` calls it too. It tightens "the scene file exists" to "the scene is a registered Scene asset", because a testing export packages the scene.
- **Manifest:** `Game.json` says `"Testing": true` exactly for a testing export.
- **TOC:** every export's Game.pak metadata keeps the full `ProjectSettings`, `Testing` included (§14.1). Only `Game.json` admits `--feature-test`, so a plain export's TOC is not reset.
- **Default directory:** `Build/<Platform>-<Config>-Testing/<Name>`, so a testing export never replaces a plain one (implemented by the contract; §14.1 amended).
- **Report and method:** `Testing`, `TestSuiteCount` (every entry, whatever its Modes) and `ReplayCount`.
  - `project.export` accepts `testing` true and false. Stream B replaces the C++ case "ExportMethods: project.export refuses testing exports until M15 stream B lands them" and the Python `test_project_export_testing_param` with that acceptance (review finding 1).
  - Its result gains `testing`, `testSuiteCount`, `replayCount`, `resourcesPatched` and `phaseTimings` (`[{phase, seconds}]`), each with a description (gate 7); the skipped case "ExportMethods: project.export reports the testing, resource and phase-timing fields" freezes them.
  - Stream B owns `ExportMethods.*`; the lead regenerates the references once.
- **User data:** `GetTestingUserDataRoot(manifestPath)` (`Engine/Project/GameManifest.h`) is `<manifest dir>/bin/TestUserData`, or the `.app`'s directory's `bin/TestUserData` for a bundle's manifest. `RuntimeApp` uses it instead of its inline path (§14.3).
- **Dist replay player and test-mode semantics:** the implementation exists. M15 proves it in the shipping binary: the export stage's Dist run of FeatureTest (the QuitApp, Api.Debug and Api.Application suites, and both replays from precompiled expectations), and the map check's required symbols (decision 11).
- **Per-mode gaps:** if the exported runs show Release or Dist gaps, stream B moves the coverage into All-mode suites through the scaffolds (`Scaffold.py`), re-records replays through `Replays.jsonl` if their hashes change, and records each changed hash with its reason in this ADR's integration section. A hash is never stored per configuration (Roadmap rule 1).

### 7. One path glob (stream B)

- `MatchesPathGlob` (`Engine/Core/PathGlob.h`) is the single C++ matcher of `Export.Exclude` and `Testing.Replays`, with the existing vocabulary. Stream B moves the algorithm there, makes `Utils::MatchesExportGlob` forward to it, and replaces the Runtime's private `MatchesGlob`.
- `Scripts/Test.py`'s `glob_regex` (the determinism suite's replay expansion, `expand_replay_globs`) cannot call it, so both run the shared vectors `Tests/Data/PathGlob/Cases.json` (review finding 11): the C++ case "PathGlob: the shared cases that Test.py's glob also runs" (skipped until B) and the passing Python case `test_replay_globs_agree_with_the_shared_path_glob_cases`. A new pattern rule lands in the vectors first.
- Patterns and paths carry no scheme: every caller strips the leading `project://` a `Testing.Replays` entry may spell (the editor's expansion, the Runtime's TOC expansion, testing exports and `Test.py`).

### 8. Notices (stream A)

- **Sources:** each `Vendor/<Lib>/VENDOR.md` ends with a uniform `## Notices` table (Name, Upstream, Version, License as SPDX, Shipped `yes`/`no`/`macOS`, Projects, License files), and `Resources/LICENSES.md` ends with one table row per Engine.pak resource and per macOS-bundled SDK component. `VENDOR.md` files are first-party, so the rule against editing upstream sources does not apply to them.
- **License texts:** the Inter OFL text may move to a file `LICENSES.md` references. The Vulkan loader's and MoltenVK's Apache-2.0 texts are committed under `Resources/Licenses/` (both ship only in macOS bundles).
- **Generation:** `Scripts/Lib/notices.py` (frozen API, format in its docstring) renders deterministic, LF text for the host's exports. `Build.py` writes it into `bin/<OutputDir>/Runtime/THIRD_PARTY_NOTICES.txt` after every build that builds the Runtime, directly or through a dependent such as Tests (`Build.stage_runtime_notices`), as it copies the CRT, so nothing generated is committed and exporter tests need no `engine://` mount. A missing or malformed section fails the build, naming the file.
- **Completeness is mechanical** (review findings 9 and m10): every vendored library whose directory the generated Runtime or Engine project references (`notices.referenced_vendor_libraries` over their project files) must be Shipped `yes`; `test_notices.py` derives that set from a fresh generation, never from a hand list. The rule over-approximates on purpose: attributing code the linker drops (MikkTSpace, cgltf) is harmless, omitting shipped code is not. One notices text serves every configuration, so Dear ImGui is `yes` although Dist strips it. A `VENDOR.md` that bundles another project's code lists that code's license among its License files (NVRHI's Vulkan-Headers; stb's `VENDOR.md` covers `stb_vorbis`, which miniaudio builds). doctest is `no`, and the Inter font is `yes`.
- The MSVC CRT DLLs need no notice and are not listed.

### 9. The export suite and stage (stream B)

- **Command:** `Test.py --suite export [--config Release,Dist] [--junit] [--require-gpu] [--editor-report <file.json>] [--timeout S]`. `Suite("export", ("Release", "Dist"), True)`, and `--config` accepts `Dist` for the export and determinism suites.
- **Steps:**
  1. Probe the Release editor's device, as the feature suite does. Without one the suite reports why and passes without running (a warning), unless `--require-gpu`, which fails it: the FeatureTest export bakes `Gradient.hdr`, the Dist Runtime needs a device, and the exported runs render the `Test.Screenshot` cases.
  2. Copy `Projects/FeatureTest`, with `Library/`, into a temporary directory, and start the Release editor on the copy (`--headless --renderer vulkan --audio-device none --automation --user-data-dir <tmp>`). For each configuration, call `project.export {config, testing: true}`, then move the package to `bin/Exports/<Config>/FeatureTest/` (`Lib/exports.py`), replacing an earlier one. The checked-in project is never written, and stream B does not depend on stream A's CLI.
  3. Run `<package executable> --headless --feature-test` (Release adds `--expect-no-gpu-errors`; Dist takes nothing more), never `--expect-no-errors` (the Errors suite logs errors) and never `--renderer none`. With `--junit` the working directory is the repository root, so the reports are `bin/TestResults/FeatureTest-<Config>.{json,xml}`; without it, a temporary directory. A non-zero exit fails the step and prints the tail of `<package>/bin/TestUserData/FeatureTest/Logs/FeatureTest.log` (Dist has no console log).
  4. Check user data: the run wrote `testing_user_data(package)/Logs/FeatureTest.log`, and no folder named after the product exists beside it. This is the Dist half of "Export: user data goes to the manifest Name folder".
  5. Evaluate each report with `coverage.evaluate_feature_report(report, expected_mode=<Config>)` (implemented by the contract, with its test over the Release and Dist seeded-gap fixtures, review finding 20): a report of another mode fails first, then `UNCOVERED gate <g> (<Mode>): <id>` lines. A skipped case row fails.
  6. With both configurations, merge `coverage.merge_coverage([editor, release, dist])` and print the `merge:` lines. The Editor report is `--editor-report`, by default `bin/TestResults/Feature-Release.json`. A report that `exports.stale_report_reason(report, exports.report_inputs("Release", "editor"))` refuses (missing, or older than the Release editor or a FeatureTest file outside `Library/`) fails with exit 3 naming `Test.py --suite feature --config Release --junit`; the editor is not rerun (ADR 0023). With one configuration the merge step is reported as skipped, with the reason.
- **JUnit:** `bin/TestResults/Export.xml`, one test case per configuration (`export <Config>`, export plus run plus evaluation) and `merge coverage`; the Runtime writes its own reports beside it.
- **CI.py `export` stage:** available, after automation. It runs `Test.py --suite export --config Release,Dist --junit`, with `--require-gpu` unless `--gpu-optional` and `--allow-skips` in contract mode. Timeout 3600 s. It stays local: `.github/workflows/ci.yml` does not run it.
- **PreCommit:** unchanged. The exported modes run in `CI.py` (milestone commits); §15.1 T3's "every commit" is amended to that.

### 10. The determinism stage's exported runs (stream B)

- **Command:** `Test.py --suite determinism [--config Debug,Release,Dist] [--require-gpu] [--junit]`. Debug selects the Editor Debug run; Release the Editor Release and Exported Release runs; Dist the Exported Dist run. The run labels and their order are `exports.DETERMINISM_RUNS`.
- **Editor runs:** unchanged (M14; CPU only).
- **Exported runs:** the hashes come from the `replay:<path>` suite rows of `bin/TestResults/FeatureTest-<Config>.json` (`exports.read_replay_hashes`: every row with a non-empty `finalStateHash`, passed or failed, so a drifted hash is reported as the hash it is; review finding m4), which the export suite of the same tree wrote; they are not rerun (ADR 0023).
  - **Current reports only** (review finding 15): a report counts when `exports.stale_report_reason(report, exports.report_inputs(<Config>, "exported"))` accepts it, that is when it is newer than the `<Config>` Runtime executable, the Release editor that exported and every FeatureTest file outside `Library/`. Leftover packages and reports of an earlier tree never pass, including after an export stage that passed without running (`--gpu-optional`) and in `CI.py --stages build,determinism`.
  - A missing or stale report fails with exit 3 naming `Test.py --suite export --junit` under `--require-gpu`. Without `--require-gpu` the exported runs are reported as not compared, with the reason, and the step ends as a warning while the editor runs still decide pass or fail.
- **Failures** (`exports.compare_determinism_runs`): for each replay, a run without a hash, a hash that differs from the committed `FinalStateHash`, and runs that disagree. The committed hashes are the replay files' own: `Determinism.replay` `2a75dca847e98e59` and `Input.replay` `ff0e3f70e3200f86` at the base. JUnit `bin/TestResults/Determinism.xml`, one test case per replay; a failure lists each run label and its hash.
- **CI.py:** `determinism` runs Debug, Release and Dist after export, with `--require-gpu` unless `--gpu-optional`. The hosted Windows job keeps it with `--gpu-optional`, so its exported half warns (ADR 0022 decision 7's "CPU only" holds for the editor half only and is amended).

### 11. Dist stripping and the map check (stream D)

- **ImGui leaves Dist:** `Application.{h,cpp}` compiles `EnableImGui`, `ImGuiIniPath`, `OnImGuiRender`, `GetImGuiLayer`, `m_ImGuiLayer` and the layer's creation under `#if !defined(ENGINE_DIST)`. The Release Runtime keeps ImGui for the F1 overlay (decision 17).
- **The check:** `Scripts/CheckRuntimeMap.py [--map <path>] [--config Dist] [--self-test] [--json]` and `Scripts/Lib/runtime_map.py` (frozen API).
  - Formats (review finding m7): `msvc` (link.exe), `lld` (ld.lld `-Map`, the Linux Clang job, and lld-link `/MAP` when a toolset links with it), `gnu` (GNU ld, the Linux GCC job) and `ld64`.
  - Families (review finding 14), derived from the headers so a new type or function is covered without editing the module:
    - automation: every type and free function declared in `Engine/Automation/**.h` (`ParseRpcRequest` among them);
    - Analysis: `Luau::Frontend` and any symbol whose origin is the `LuauAnalysis` library;
    - ImGui: the `ImGui` namespace, any symbol whose origin is the ImGui library, and every type and free function declared in `Engine/ImGui/**.h` (`ImGuiScreenshot` and `StatsOverlay` among them);
    - AssetPipeline: every type and free function declared in `Engine/AssetPipeline/**.h`.
  - Matching: symbol names only through namespace or type components of decorated, mangled or demangled names, never raw substrings; origins only through their library component, never their object-file stem. So string literals and object stems never count, while a library's objects do. `runtime_map.FamilyPatterns` holds each family's name and library patterns.
  - `REQUIRED_SYMBOLS` (`FeatureTestRunner`, `ReplayPlayer`) must be present.
  - Exit codes: 0 clean, 1 findings, 3 the map is missing.
- **Fixtures:** `Tests/Data/MapFiles/<format>/{Clean, automation, Analysis, ImGui, AssetPipeline, MissingRequired}.map`, with `Expected.json` per format. They are hand-written first-party fixtures, and MSVC's `Clean.map` contains an "ImGui" string literal and a `Watchdog1.obj` stem. `--self-test` checks them.
- **Wiring (lead):** a `CI.py` build-stage step right after `build Dist`, named verbatim **"Dist: map file contains no automation, Analysis, ImGui or AssetPipeline symbols"**, because PreCommit builds no Dist (precedent: the shader acceptance step). The step also runs on the Linux and macOS jobs' Dist builds, where it is non-blocking (ADR 0011). `CheckRuntimeMap.py --self-test` joins the static checks.
- **Luau compiler in Dist** (§14.4 "no Luau compiler"): the Dist map links Luau's `Compiler`, `Parser` and `Ast` objects, probably through `ScriptApiRegistry`'s `Luau::Parser::parseType` and the require library's configuration path. Stream D removes it from Dist if that needs no vendored-source edit and no behaviour change, and then, in the same change, appends a `LuauCompiler` family (`Luau::Compiler`, `Luau::Parser`, `luau_compile` and the `Luau.Compiler` and `Luau.Ast` library origins) to `FORBIDDEN_FAMILIES` with its fixtures, an extension of the frozen API this decision approves, so the compiler cannot return unnoticed. Otherwise the completion review records the deviation with the evidence that no game input reaches it (`ScriptCompiler` is `Unsupported` in Dist).

### 12. AddressSanitizer build and runs (stream C)

- **Premake** (implemented): `--sanitize=address` adds `sanitize { "Address" }` at workspace scope for Release only, so every project, vendored ones included, is instrumented alike. MSVC's STL marks translation units that use its container annotations, so mixing instrumented and uninstrumented C++ fails or misreports. Debug (`/ZI`) and Dist (LTO) are never sanitized. The sanitized Release executables also get `/STACK:2097152`, as Debug has, because ASan's stack redzones enlarge the frames that Luau Analysis recursion stacks up (`Dependencies.lua`, risk 12). No vendor script changes, so no `VENDOR.md` changes.
- **Workspace:** `Generate.py --sanitize address --to bin-int/Asan --no-compile-commands`, then `Build.py --workspace-dir bin-int/Asan --config Release`. The binaries live in `bin-int/Asan/bin/Release-windows-x86_64/<Project>/` and never replace the plain Release build; the SPIR-V stays shared (`ShaderOutputDirectory`). A sanitizer warning in vendored code may use `--allow-vendor-warnings`, as the clang-cl build does; one in first-party code is fixed.
- **Runtime:** `Lib/hardening.py stage_asan_runtime` copies `clang_rt.asan_dynamic-x86_64.dll` and its `.pdb` from the MSVC tools' `bin/Hostx64/x64` next to Tests, Editor and Runtime in the ASan tree, never into `Runtime/Redist`, which exports copy. A missing runtime is exit 3, naming Visual Studio's "C++ AddressSanitizer" component, and `Setup.py` warns about it. The runtime is never redistributed: an ASan editor exports the plain Runtime, because its `ExportBinaryRoot` is the repository's `bin`.
- **CheckBuildConfig.py:**
  - a target `windows-asan` (vs2026 with `--sanitize=address`) in which every non-Utility project has ASan in Release and in no other configuration;
  - every plain target has no project with ASan in any configuration;
  - `_DISABLE_VECTOR_ANNOTATION` and `_DISABLE_STRING_ANNOTATION` are banned in premake and in source;
  - the fixture `Tests/Data/BuildConfig/PartialAddressSanitizer/` (one project with `removesanitize`) must fail. The lead adds it to the fixture list in `Scripts/Lib/scripts.py` at integration, once the fixture exists.
- **Runs:** `Test.py --build-root <dir>` (the `bin` directory whose `<OutputDir>` holds the executables; default the repository's) and `--sanitizer address`.
  - `--sanitizer address` gives every process the suite starts `hardening.asan_environment(os.environ)` (review findings 2 and M2): `ASAN_OPTIONS` (`hardening.asan_options()`: at least `halt_on_error=1`, `allocator_may_return_null=0` and a bounded `quarantine_size_mb`, plus the handler settings the crash-handler tests need, decided by stream C's first spike and recorded at integration) and the runtime's directory first on `PATH`. Children inherit both, so the instrumented Runtime copies that the resource and user-data acceptance tests export into temporary packages load the runtime too. Report stems get the suffix `-Asan`.
  - Steps: `unit Release (ASan)` (`Unit-Release-Asan.xml`) and `feature Release (ASan)` (`Feature-Release-Asan.{xml,json}`, `--require-gpu` unless `--gpu-optional`). `ERROR: AddressSanitizer:` lines go into the step's detail.
  - Acceptance: both are clean.

### 13. Long fuzz: readers, counts and seeds (stream C)

- **Support** (`Tests/Source/Support/Fuzz.h`, frozen): the `Fuzz` doctest suite, `GetFuzzRange` (acceptance count, `--fuzz-scale`, `--fuzz-iterations`, `--fuzz-shard`, `--fuzz-index`), `GetFuzzIterationSeed` (SplitMix64 of `caseSeed + (index + 1) * 0x9E3779B97F4A7C15`), and `ReportFuzzRun`'s single Info line, whose pattern is `hardening.FUZZ_REPORT_PATTERN`. `TestOptions` gains `FuzzIterations`, `FuzzScale`, `FuzzShardIndex`, `FuzzShardCount`, `FuzzIndex` and `TimeoutScale`; `--test-timeout-scale` scales decorated and default limits alike (`TestTimeout`).
- **Iteration independence:** each iteration's input comes from its index alone. A case whose reader keeps state across calls resets, before every iteration, all the state its checks observe, so the outcome also depends only on the index; what else accumulates (a session's tick count, its error history) is no input of any check. A failure reproduces alone with `Tests -tc=<case> --fuzz-iterations=<total> --fuzz-index=<i>` (review finding m1).
- **The binding fuzz mapping** (review findings 6 and m1), "Bindings: 10,000 seeded play-session calls reject invalid inputs before physics":
  - iteration `i` is a corpus call when `i % 10 < 7` and a generated call otherwise (7,000 and 3,000 of the acceptance count, as today);
  - the corpus calls, numbered in index order, form rounds of 2 × the corpus size, each a permutation of every case over both transports, seeded from `(caseSeed, round)`;
  - every call's arguments come from `Random(GetFuzzIterationSeed(caseSeed, i))`;
  - one play session per process, reset before every call as today (the generated phase's `reset()` included); the three fixed generated statements run once per process;
  - the thresholds are ratios of the calls the process ran: accepted > 10 % and rejected > 50 % of its corpus calls, and the same of its generated calls; every corpus case is visited by construction in each complete round;
  - the minimum is the smallest count whose corpus calls complete the first round.
- **Targets:** `Tests/Data/Fuzz/Targets.json` (`{"Format": "FuzzTargets", "Version": 1, "Targets": [{"Reader", "Case", "Iterations"}]}`) lists every reader of external input; `Iterations` is 1,000,000 for every target without exception (`read_fuzz_targets` refuses any other value, review findings 13 and m6). The required readers:
  - **existing cases, converted to `Fuzz.h`:** BinaryReader, JsonReader, CookedFormat (re-sealing the payload hash so mutations reach the payload), PakReader, PakToc (a direct TOC fuzz with a re-sealed TOC hash), MeshData, TextureData, FontData, AudioClipData, ScriptData, ReplayData (text and re-sealed cooked), SceneSerializer, ShaderReflection, DecodePng, Framing, and Bindings (the play-session binding fuzz, §15.2, at 1,000,000 calls);
  - **new cases:** EnvironmentData, Prefab (text and cooked), ProjectSerializer, AssetMetadata, MaterialData (text and cooked), SoundEffect, GameManifest, BuiltinAssetCatalog, JsonRpc (`ParseRpcRequest` and `ParseHelloRequest`), PlayInputEventCodec, EditorPreferences (`ReadEditorPreferences`, `user://Editor.json`), AutosaveRecovery (`ReadAutosaveRecovery`), ProjectLock (`ProjectLock::ReadHolderPid`), TextureImporter (PNG, JPEG, TGA, BMP through stb), EnvironmentImporter (`.hdr` through `stbi_loadf`), FontImporter (`ValidateTrueTypeTables` plus `TrueTypeFont::Open` and a few glyphs, the stb_truetype guard), GltfImporter (cgltf), AudioDecoder (WAV, FLAC, MP3, Vorbis) and ScriptCompiler (Luau source; cooked bytecode stays trusted after its hash, §6.8). The automation session file is read only by the Python clients (`engine_client.py`, the MCP bridge), whose tests cover its parsing (review finding 21).
  - **seed corpora:** `Tests/Data/Assets/**`, `Tests/Data/Project/AllSettings.eproj`, `Tests/Data/Scenes/**` and FeatureTest's metas, replays and `.sfx` files.
  - **in memory:** every iteration reads from memory and writes no file; importers are fuzzed through their in-memory entry points (review finding m2).
  - **unit counts:** the unit suite runs each existing case's current count; a new case runs 1,000 iterations, 200 for the third-party decoders (TextureImporter, EnvironmentImporter, FontImporter, GltfImporter, AudioDecoder) and ScriptCompiler, and its name states its count. Stream C reports the time the new cases add to the Debug unit suite, which stays under 60 s; hardening's 1,000,000 is their acceptance.
- **The stage** (`Hardening.py`): the ASan Tests binary with `--test-suite=Fuzz --fuzz-iterations=1000000 --fuzz-shard=<k>/<n> --test-timeout-scale=20` in n = `hardening.fuzz_process_count(logical CPUs, available memory, measured peak per process)` processes (at most 32), each with its own `--user-data-dir` and `asan_environment`.
  - The report lines are read from each process's captured standard error (the console sink writes every level there), never from `Tests.log`, whose rotation could drop them (review finding m9). Fuzz case names contain no apostrophe (n4).
  - `hardening.evaluate_fuzz_logs` fails a target whose shards do not cover `[0, 1,000,000)` exactly once, and any process failure.
  - JUnit `Fuzz-Release-Asan.xml`.
  - Wall-time budget: 45 minutes on this machine (24 logical CPUs); stream C's first spike records each target's cost per iteration and the peak memory per process, at integration.
- **Upstream defects:** a crash in a vendored decoder is fixed by a vendor update (downloads are pre-approved by ADR 0001; the lead owns `Vendor/`) or, failing that, by an engine-side guard; never by excluding the target.
- **The physics edit fuzz** stays in the unit suite (24 seeds); it reads no external input.

### 14. Debug fuzz prefix: pending the product owner (ADR 0023 follow-up; stream C)

ADR 0023 left "moving the long Debug fuzz iterations to the hardening stage" to its own decision. The proposal: the Debug unit run passes `--fuzz-scale=0.1`, so every fuzz case runs the first tenth of its own sequence (at least its minimum), the Release unit run keeps every count and hardening runs 1,000,000. It would save about 150 s per Debug unit run, and determinism would hold (decision 13's index independence).

It changes what every PreCommit runs of the verbatim "10,000 …" acceptance cases, which AGENTS.md's "no weakened tests" protects, so it needs the product owner's agreement (review finding M5). Until it is recorded here:

- Test.py passes no `--fuzz-scale`; the Debug unit run executes every case's full count.
- The mechanism stays (`TestOptions::FuzzScale`, `GetFuzzRange`'s scale and minimum, their tests): it is what hardening's absolute counts and shards build on, and the approved change is then one line in stream C's `Test.py` hunk.
- New cases keep the small unit counts of decision 13, so the Debug unit suite does not grow by more than 60 s either way.

### 15. Soak (stream C)

- **Coverage** (review findings 4 and 18): every `.scene` below `Assets/Scenes/` of every project `hardening.soak_projects` names (`Projects/FeatureTest` from M15, the golden scenes included, and every demo project from its milestone, so §15.1 T6's "per demo" holds), plus the Runtime: the exported Release FeatureTest that the export stage leaves in `bin/Exports/Release/FeatureTest/` runs its start scene for 10,000 frames headless, so the Runtime-only paths (the pak mounts, the Runtime's asset manager and frame loop) are soaked too. A missing or stale package (decision 10's rule) is exit 3 naming `Test.py --suite export --config Release --junit`.
- **Editor driver:** one plain Release editor per scene, never ASan (its quarantine grows by design): `--headless --renderer vulkan --audio-device none --automation --user-data-dir <tmp>`, on a temporary copy of the project with `Library/`.
  - It calls `scene.open`, `play.start {lockstep: true}` and then 40 calls of `play.step {ticks: 250, render: "Every"}`, which is 10,000 rendered frames. A lockstep driver keeps its session through `Application.Quit` (§12.4); a step that fails is that scene's soak failure, naming the error.
  - After each step it samples `process.query_process_memory(pid)` (frozen in `Lib/process.py`; Windows `PrivateUsage`) and `stats.get` (Entities, Bodies, Voices, ScriptHeapBytes, MemoryAllocationCount). No engine interface changes.
- **Runtime driver:** `<package executable> --headless --frames 10000`, sampled from outside every 0.5 s with `query_process_memory`; a sample's position is its elapsed time over the run's.
- **Rules** (`hardening.evaluate_soak`, review findings 18 and M3), with windows as fractions of the soak (the editor's position is tick / 10,000): the baseline window is 0.50 to 0.55, the final window 0.95 to 1.00.
  - private memory: the final window's maximum grows less than 1 % over the baseline window's median;
  - script heap, a garbage-collected sawtooth: the final window's minimum does not exceed the baseline window's maximum;
  - entities, bodies, voices and the device's memory allocation count, small integers: the final window's maximum does not exceed the baseline window's maximum;
  - a rule whose field is absent (the Runtime soak's stats) is not applied; a window without a sample, or a zero baseline, is a failure naming it, and no rule divides by zero.
- **Noise first:** stream C's first spike soaks two or three scenes, records each rule's observed margin at integration, and brings any rule whose noise reaches half its limit to the lead before the thresholds are relied on; no threshold changes silently.
- **Report:** `bin/TestResults/Soak.json` (`{"Format": "SoakReport", "Version": 1, "Frames": 10000, "Subjects": [{"Subject", "Samples": [...], "Growth", "Passed", "Failures"}]}`) and `Soak-Release.xml`.
- **Without a device:** the soak passes without running, as a warning, under `--gpu-optional`; otherwise it fails.
- **Budget:** 40 minutes (the golden scenes add about 10).

### 16. Perf capture (stream D)

- **Command:** `Scripts/Perf.py [--config Release] [--output <file>] [--gpu-optional] [--json]` writes `bin/TestResults/Perf.json` (`Lib/perf.py`; format in its docstring, aligned with this decision, review finding 8).
- **Metrics:**
  - `playCopy/1000Entities`: the median of 5 `play.start`/`play.stop` cycles of a generated 1,000-entity scene, against the 30 ms budget §5.6 handed to this capture (ADR 0012);
  - per scene: `scene/<Name>/frameMs` (`play.step` time per tick with `render: "Every"`) and `scene/<Name>/simulationMs` (with `render: "None"`), with `stats.get`'s GPU pass times and the process memory (`privateStart`, `privateEnd`, `privatePeak`, `residentPeak`);
  - `replay/Determinism/tickMs` (the 600-tick replay in a `--renderer none` editor).
- **Warning-only:** an over-budget metric or a fallback without a GPU makes the step a warning. Only a capture that cannot run (a missing build, a crash, a protocol error, or `Perf.json` not written) fails it, because no outcome may depend on how long anything takes (ADR 0008 decision 15).
- **Baselines:** none committed (they churn per machine).
- **Budget:** 10 minutes.
- **Profiler:** the GPU merge into the profiler timeline (§4.13) has no consumer. The completion review decides between wiring `GpuProfiler` into `Profiler::SubmitGpuZones` and amending §4.13.

### 17. F1 stats overlay and `--log-level` (stream D)

- **Overlay:** `Engine/ImGui/StatsOverlay.h`: `StatsOverlayData`, the pure `FormatStatsOverlay`, `DrawStatsOverlay`, and the pure toggle `StatsOverlayToggle` (hidden at start; each F1 press that is not a repeat toggles it; review finding 17). A windowed Debug or Release Runtime enables ImGui and draws the overlay while it is visible. A headless run and every Dist build have neither ImGui nor the overlay, so test runs and the shipping binary are unaffected.
- **Input:** the overlay never consumes input. The overlay window takes no input, and the Runtime ignores ImGui's `WantCapture` flags, so F1 and every other event reach the game in every configuration and Release and Dist games see identical input.
- **Tests:** the toggle's case is frozen (skipped); stream D adds a case that draws the overlay in an ImGui context without a device (review finding n6); the lead adopts the call site in `RuntimeApp.cpp` (stream B's file) after both streams merge and adds "RuntimeApp: F1 toggles the stats overlay in a windowed run and the game still receives F1" to `RuntimeAppTests.cpp` (a windowed child with `Test::ProbeGpuForProcess`).
- **`--log-level <trace|info|warn|error|critical>`:** `ApplicationSpecification::MinimumLogLevel`, honoured in Dist (§13.9), which closes that gap. It sets the minimum level of the output sinks (console and log file) only, like `LogSpecification::ConsoleLevel`, so `--expect-no-errors`, the ring buffer `log.read` serves, log listeners and expected-log capture still see every entry; in Dist, where Trace is compiled out, `trace` acts as `info` (review findings 16 and m3). Stream D owns `Application.{h,cpp}` and adds "Application: --log-level filters the console and file sinks only, so --expect-no-errors still counts errors".

### 18. Portability and the Linux RPATH (stream D, lead)

- **RPATH** (implemented): `Runtime/premake5.lua` adds `-Wl,-rpath,'$$ORIGIN'` on Linux. Generated into a scratch directory, gmake's `ALL_LDFLAGS` and ninja's `ldflags_Runtime_<Config>` both carry it verbatim in every configuration, and make and ninja turn `$$` into `$` inside the shell's single quotes.
- **Checks:** `Scripts/Lib/portability.py check_runtime_link_settings` (frozen API) checks the generated settings for every configuration:
  - gmake and ninja: the RPATH and the `-Wl,-Map=` path;
  - xcode4: the Frameworks rpath, `-Wl,-map,`, `MACOSX_DEPLOYMENT_TARGET` 14.0 and the tool product type, since the exporter copies a plain executable;
  - vs2026: `/MAP` in the Link `AdditionalOptions` of every configuration (review finding 5; premake emits no `GenerateMapFile`), with subsystem Windows in Dist and Console otherwise.

  The lead calls it from `CI.py`'s portability stage.
- **clang-cl:** the milestone gate requires the clang-cl builds: `CI.py --milestone` implies `--require-clang-cl` (decision 19).

### 19. The hardening stage, `--milestone` and time budgets (lead)

- **Availability:** `hardening` becomes available in `CI.py`, last after portability. It is not part of `--stages all`, because §15.8 runs hardening at milestone gates only. `CI.py --milestone` selects every available stage plus `hardening` and implies `--require-clang-cl`; the commit that completes a milestone runs it. Plain `CI.py` reports hardening in its summary as not run ("milestone gates only: CI.py --milestone"), never silently.
- **Combinations** (review finding 28): `--milestone` together with `--stages`, `--skip-stages`, `--configs`, `--toolset`, `--no-clang-cl`, `--gpu-optional` or `--contract` is a usage error (exit 2): a milestone gate is the whole matrix, on a machine with a device, in strict mode. `--json` and `--summary-junit` combine freely.
- **Standing rules amended** (review findings 3 and M4), in this contract: Roadmap rule 1 and M15's acceptance, AGENTS.md (milestone workflow rule 1, the commit gate, the commands table, the remote CI paragraph), Architecture §2.3 and §15.9, `Docs/ReviewChecklist.md`, and the `commit-review` and `build-and-test` skills now name `CI.py --milestone` for the commit that completes a milestone. ADR 0023 decision 1's "a milestone commit runs `CI.py`" is read with this: plain `CI.py` for any other commit that needs full CI.
- **Steps**, failing fast:
  1. `perf capture (Release)`: `Perf.py`, warning-only;
  2. `soak Release`;
  3. `asan generate`;
  4. `asan build`;
  5. `unit Release (ASan)`;
  6. `feature Release (ASan)`;
  7. `fuzz Release (ASan)`.

  Steps 2 to 7 run through `Scripts/Hardening.py --steps soak,asan-generate,asan-build,asan-unit,asan-feature,fuzz [--gpu-optional] [--json]`, stream C's entry point, which reports one step each. `--gpu-optional` has its usual meaning for perf, soak and the ASan feature suite when `Hardening.py` runs alone.
- **Time budget** on this machine: the first ASan build 20 minutes (incremental afterwards), ASan unit 6, ASan feature 6, fuzz 45, soak 40, perf 10; about 2 hours 10 minutes the first time. `TIMEOUTS`: perf 1200 s, hardening steps 3600 s each, the ASan build 7200 s.
- **Gate cost:**
  - PreCommit is unchanged.
  - `CI.py --milestone` grows by the export stage (about 15 minutes; the determinism stage reuses its reports) and the hardening stage.
  - One milestone commit, one gate run (review finding 27): the streams' integration, the milestone review and the completion review land in the single commit that completes M15, gated by one `CI.py --milestone` on its final tree (ADR 0023 decisions 1 and 2). No separate integration commit with its own `CI.py` run.
- Streams C and D report their measured step times. A step over its budget is raised with the lead before any count is cut.

### 20. Remote CI (lead)

- **Windows job:** keeps its stage list, so the determinism stage's exported half warns under `--gpu-optional`. Its comments are updated.
- **Local only:** export and hardening stay with the local gate. Hosted perf numbers mean nothing, and ASan is MSVC's.
- **Map check:** runs in every job's build stage after the Dist build, non-blocking on Linux and macOS (ADR 0011).
- **Catch-up:** Linux and macOS have not been checked remotely since ADR 0011 (M6). The lead checks the Actions runs of the M15 milestone commit and fixes failures in one batch before M16 starts (ADR 0011: "always before the demo games"; the product owner's standing instruction is to finish a milestone on the local Windows gate and not wait for Actions). The review's suggestion to fold the catch-up into M15 before its milestone commit is declined for that reason; its concern (macOS code no local check compiles) is met by decision 5's host-independent assembly, which leaves only `codesign` macOS-only. Anything that points to a design problem is raised at once.

### 21. Completion review (lead)

After the streams are merged and before the single `CI.py --milestone` run (ADR 0023 decision 2), the lead reviews every Architecture section (§1 to §18 and appendices A to D) against the code, in `Docs/Decisions/0025-m15-completion-review.md`.

- **One row per subsection:** conforms; fixed in M15 (test); deviation accepted (decision); or deferred after v1 (an Appendix C row).
- **Amendments:** Architecture is amended in the same change.
- **Items it must dispose of** (from the state maps, verified):
  - §6's `--strict`;
  - §4.7's per-frame `FrameArena`;
  - §4.13's GPU zones in the profiler timeline (decision 16);
  - §8.12, §7.5 and §14.4's shader hot reload, never scheduled by any milestone;
  - §9.6's ground-velocity carry of the character;
  - §14.4's Luau compiler in Dist (decision 11);
  - §2.2's MikkTSpace omission;
  - stale texts: §1.2 "OGG import", §7.4, §17 and Appendix C on MikkTSpace, §4.12's DetMath attribution;
  - ADR 0005 decision 24's duplicated helpers;
  - ADR 0007's `GenerateDetMathReference.py --check` outside the static checks;
  - ADR 0013 decision 25's known limits;
  - ADR 0015 decision 27's GPU-suite driver crash;
  - ADR 0022 review fix M11 (`Projects/FeatureTest/Automation/Provenance.json` lacks the 24 M9 golden files);
  - ADR 0023's vendor-library cache follow-up.

  `--validate` and `--timeout` (§12.1, §4.1) are settled by decision 3 and amended in this contract.

## Frozen interfaces

| File | Interface | Implementing stream |
|---|---|---|
| `Engine/Source/Engine/Platform/ResourcePatcher.h` (+ `.cpp`, `Windows/ResourcePatcherWindows.cpp`, `Posix/ResourcePatcherPosix.cpp`) | `ExecutableIconImage`, `ExecutableVersionInfo`, `ExecutableResourceSet`, `ResourcePatcher::{IconGroupId, FirstIconId, VersionId, LanguageId, CodePage, BuildVersionInfo, BuildIconGroup, Patch}` | A |
| `Editor/Source/EditorCore/Export/ExecutableResources.h` (+ `.cpp`) | `ExecutableIconSizes`, `BuildExecutableResourceSet`; the square-icon rule of `ResolveExecutableResources` | A |
| `Editor/Source/EditorCore/Export/AppBundle.h` (+ `.cpp`) | the bundle path constants, `AppBundleInfo`, `MakeBundleIdentifier`, `WriteInfoPlist`, `BuildIcns`, `RewriteMoltenVkIcd`, `CopyAppBundleBinaries`, `WriteAppBundleResources` | A |
| `Editor/Source/EditorCore/Export/ExportConfiguration.h` | `ExportConfiguration`, `ExportConfigurationToString` (moved from `Exporter.h`, implemented) | A |
| `Editor/Source/EditorCore/Export/Exporter.h` (+ `.cpp`) | `ExportPhase::{PatchResources, Sign}`, `ExportPhaseTiming`, `ExportSpecification::{Testing, OutputPath}` with its refusal rules, the `ExportRoot` semantics, the output-directory rule, `ExportReport::{Testing, TestSuiteCount, ReplayCount, ResourcesPatched, PhaseTimings}`, `Exporter::IsMissingBuildOutput`, `GetDefaultOutputDirectory(…, testing)` (implemented) | A (B's call sites wired by the contract) |
| `Editor/Source/EditorCore/Export/TestingExport.h` (+ `.cpp`) | `TestingExportForcedDirectory`, `TestingExportContent`, `CollectTestingExportContent`, `IsForcedByTestingExport` | B |
| `Editor/Source/EditorCore/Project/ProjectValidator.h` (+ `.cpp`) | `ProjectValidator::CheckTestSuites` | B |
| `Engine/Source/Engine/Core/PathGlob.h` (+ `.cpp`) | `MatchesPathGlob`; `Tests/Data/PathGlob/Cases.json` | B |
| `Engine/Source/Engine/Project/GameManifest.h` (+ `.cpp`) | `GetTestingUserDataRoot` | B |
| `Editor/Source/EditorCore/EditorCommandLine.h` (+ `.cpp`) | `EditorLaunchOptions::{ExportDirectory, ExportConfig, ExportTesting, ExportSmokeTest, ExportReportPath, ExportRoot}`; `IsOneShot` (implemented) | A |
| `Editor/Source/EditorCore/Automation/AutomationServer.h` | `AutomationServerSpecification::ExportRoot` (read by `project.export`: implemented) | A sets it |
| `Engine/Source/Engine/App/Application.h` | `ApplicationSpecification::MinimumLogLevel` (output sinks only), `--log-level`; ImGui members compiled out of Dist | D |
| `Engine/Source/Engine/ImGui/StatsOverlay.h` (+ `.cpp`) | `StatsOverlayData`, `FormatStatsOverlay`, `DrawStatsOverlay`, `StatsOverlayToggle` | D |
| `Tests/Source/Support/Fuzz.h` (+ `.cpp`) | `FuzzSuite`, `FuzzRange`, `GetFuzzRange`, `GetFuzzIterationSeed`, `FuzzCounts`, `ReportFuzzRun` | C |
| `Tests/Source/Support/TestOptions.h` | `FuzzIterations`, `FuzzScale`, `FuzzShardIndex`, `FuzzShardCount`, `FuzzIndex`, `TimeoutScale` (parsed by C) | C |
| `Tests/Source/Support/PlatformProbes.h` (+ `.cpp`) | `ExecutableResourceProbe`, `ProbeExecutableResources` (Windows) | A |
| `Tests/Source/Support/ExportBinaryRoot.h` (+ `.cpp`) | `DefaultExportNotices`, `ExportBinaryRootOptions`, `MakeExportBinaryRoot` (implemented) | A owns it afterwards |
| `Scripts/Lib/notices.py` | `NOTICES_FILE_NAME`, `NOTICES_SECTION`, `NoticeEntry`, `read_notice_entries`, `collect_notice_entries`, `referenced_vendor_libraries`, `render_notices`, `generate_notices` | A |
| `Scripts/Lib/exports.py` | `EXPORTS_ROOT`, `FEATURE_TEST_NAME`, `DETERMINISM_RUNS`, `REPLAY_SUITE_PREFIX`, `package_directory`, `package_executable`, `testing_user_data`, `read_replay_hashes`, `report_inputs`, `stale_report_reason`, `compare_determinism_runs` | B |
| `Scripts/Lib/coverage.py` | `evaluate_feature_report(report, expected_mode=None)` (implemented) | B |
| `Scripts/Lib/hardening.py` | the ASan, fuzz and soak constants and `FUZZ_REPORT_PATTERN`, `FuzzTarget`, `SoakSample`, `SoakResult`, `asan_options`, `asan_runtime_directory`, `asan_environment`, `stage_asan_runtime`, `read_fuzz_targets`, `fuzz_process_count`, `evaluate_fuzz_logs`, `soak_projects`, `soak_scenes`, `evaluate_soak` | C |
| `Scripts/Lib/process.py` | `ProcessMemory`, `query_process_memory` | C |
| `Scripts/Lib/runtime_map.py` | `FORBIDDEN_FAMILIES` (and decision 11's `LuauCompiler` extension), `REQUIRED_SYMBOLS`, `MAP_FORMATS`, `MapSymbol`, `FamilyPatterns`, `MapFile`, `parse_map`, `family_patterns`, `check_map` | D |
| `Scripts/Lib/portability.py` | `check_runtime_link_settings` | D |
| `Scripts/Lib/perf.py` | `PERF_FORMAT`, `PERF_VERSION`, `PERF_REPORT_NAME`, `BUDGETS`, `MetricSummary`, `summarize`, `metric_to_json`, `write_perf_report`, `read_perf_report` | D |

The headers of the frozen M7 `Exporter.h`, the M13 `ProjectValidator.h`, the M2 `Application.h`, the M4 `AutomationServer.h`, the M7 `GameManifest.h`, the M14 `EditorCommandLine.h` and `ExecutableResources.h`, and the test-support headers `TestOptions.h` and `PlatformProbes.h` change here, and each change is reviewed in this contract.

## Script interfaces (frozen CLIs, no stubs)

- `Editor --export <dir> --config C [--testing] [--smoke-test] [--export-report <file>]` and `Editor --export-root <dir>` (decision 3).
- `Scripts/Export.py` (decision 3).
- `Scripts/Build.py`: on every OS, writes `Runtime/THIRD_PARTY_NOTICES.txt` after every build that builds the Runtime (`stage_runtime_notices(root, config, system)`, decision 8); on macOS, stages `Runtime/Frameworks/` and `Runtime/Vulkan/icd.d/` from the pinned SDK only (`stage_macos_frameworks(root, config, sdk)`, decision 5); archives the Dist Runtime's symbols into `bin/Symbols/` (`archive_dist_symbols(root)`, decision 2). Each returns a dictionary that its `--json` report carries, like `crtRedist`; `test_build_script.py` freezes the three names.
- `Scripts/Generate.py --sanitize address` (decision 12).
- `Scripts/CheckBuildConfig.py`: target `windows-asan` (decision 12).
- `Scripts/Test.py`:
  - `--suite export`, `--config Dist` and `--editor-report` (decision 9);
  - the determinism configurations (decision 10);
  - `--build-root`, `--sanitizer address` and `--test-timeout-scale` (decision 12); `--fuzz-scale 0.1` on Debug unit runs only once decision 14 is approved.
- `Scripts/Hardening.py --steps … [--gpu-optional] [--json]` (decisions 12 to 15).
- `Scripts/CheckRuntimeMap.py` (decision 11).
- `Scripts/Perf.py` (decision 16).
- `Scripts/CI.py`:
  - the export stage (decision 9);
  - the determinism configurations (decision 10);
  - the map-check build step (decision 11);
  - the portability link-settings step (decision 18);
  - `hardening`, `--milestone` and its refused combinations (decision 19).

## Data formats

- **Package** (§14.1): `THIRD_PARTY_NOTICES.txt` in every package (on macOS in `Contents/Resources`); the macOS output directory holds only `<Name>.app` (decision 5); `Game.json` `Testing` (decision 6).
- **Export report file** (`--export-report`): decision 3.
- **Exports of the export stage:** `bin/Exports/<Config>/FeatureTest/`; test user data `<package>/bin/TestUserData/FeatureTest/`.
- **Reports:**
  - `bin/TestResults/FeatureTest-{Release,Dist}.{json,xml}` (Runtime) and `Export.xml`;
  - `Determinism.xml`;
  - `Unit-Release-Asan.xml`, `Feature-Release-Asan.{xml,json}` and `Fuzz-Release-Asan.xml`;
  - `Soak.json` and `Soak-Release.xml`;
  - `Perf.json`.
- **`Tests/Data/Fuzz/Targets.json`:** decision 13.
- **`Tests/Data/MapFiles/<format>/`:** decision 11.
- **`Tests/Data/PathGlob/Cases.json`:** decision 7 (`{"Format": "PathGlobCases", "Version": 1, "Cases": [{"Pattern", "Path", "Matches"}]}`).
- **`## Notices` sections and the notices text:** decision 8 and `Lib/notices.py`.
- **Dist symbols:** `bin/Symbols/<OutputDir>/Runtime/`.

## Ownership

Streams own disjoint files, except where a stream owns named hunks of a file and nothing else in it, as listed. A stream that needs a change in another stream's or the lead's file lists it in its result. A defect a stream finds in a file it does not own goes to that file's owner, or, for files no stream owns, to the stream that found it (C, for ASan and fuzz findings).

**Stream A:**
- `Editor/Source/EditorCore/Export/{Exporter.*, ExecutableResources.*, AppBundle.*, ExportConfiguration.h}`. The testing call sites in `Exporter.cpp` stay as the contract wired them.
- `Engine/Source/Engine/Platform/ResourcePatcher.*`, `Platform/Windows/ResourcePatcherWindows.cpp`, `Platform/Posix/ResourcePatcherPosix.cpp`
- `Engine/Source/Engine/Graphics/VulkanDispatch.cpp`: the macOS bundled-loader hunk only (decision 5)
- `Editor/Source/EditorCore/EditorCommandLine.*`; `Editor/Source/Editor/EditorApp.{h,cpp}` (the export and export-root one-shots and options)
- `Scripts/Build.py`, `Scripts/Export.py`, `Scripts/Lib/notices.py`
- the `## Notices` sections of `Vendor/*/VENDOR.md` and `Resources/LICENSES.md`; `Resources/Licenses/**`
- `Tests/Source/Support/PlatformProbes.*` (the resource probe) and `Tests/Source/Support/ExportBinaryRoot.*`
- tests:
  - `Tests/Source/Engine/Platform/ResourcePatcherTests.cpp`
  - `Tests/Source/EditorCore/Export/{ExporterTests, ExecutableResourcesTests, AppBundleTests, ExportResourcesTests}.cpp`
  - `Tests/Source/EditorCore/EditorCommandLineTests.cpp`
  - `Tests/Source/Editor/EditorAppTests.cpp` (the export one-shot cases)
  - `Tests/Automation/{test_export, test_export_cli, test_notices, test_runtime, test_build_script}.py` and `tiny_game.py`

**Stream B:**
- `Editor/Source/EditorCore/Export/{TestingExport.*, Private/ExportPaths.*}`, `Editor/Source/EditorCore/Automation/ExportMethods.*`
- `Editor/Source/EditorCore/Project/ProjectValidator.*` (`CheckTestSuites` and Validate's call)
- `Editor/Source/EditorCore/Play/EditorFeatureTestHost.*` (the glob)
- `Engine/Source/Engine/Core/PathGlob.*`, `Tests/Data/PathGlob/**`
- `Engine/Source/Engine/Project/GameManifest.*` (`GetTestingUserDataRoot`, the comment)
- `Engine/Source/Engine/Testing/**`, `Engine/Source/Engine/Session/ReplayPlayer.*` (fixes the exported runs reveal)
- `Runtime/Source/Runtime/RuntimeApp.*` (test paths, the glob, the testing user-data root). The F1 hook is the lead's adoption.
- `Scripts/Test.py`, except stream C's hunks; `Scripts/Lib/exports.py`, `Scripts/Lib/coverage.py`
- `Tests/Automation/harness.py` and `fake_editor.py` (the other streams send their needs; review finding 22)
- `Projects/FeatureTest/**` (per-mode gap fixes through the scaffolds, `Provenance.json` and the re-recorded replays)
- tests:
  - `Tests/Source/EditorCore/Export/TestingExportTests.cpp`
  - `Tests/Source/Engine/Core/PathGlobTests.cpp`
  - `Tests/Source/Engine/Project/GameManifestTests.cpp` (unskipping the contract case)
  - `Tests/Source/EditorCore/Project/ProjectValidatorTests.cpp`
  - `Tests/Source/EditorCore/Automation/ExportMethodsTests.cpp`
  - `Tests/Source/Runtime/RuntimeAppTests.cpp`, `Tests/Source/Support/TestGame.*`
  - `Tests/Source/EditorCore/FeatureTest/**`, `Tests/Source/Engine/Testing/**`
  - `Tests/Automation/{test_testing_export, test_script_modes, test_coverage_gates, test_feature_test}.py`

**Stream C:**
- `Scripts/Generate.py`, `Scripts/CheckBuildConfig.py`, `Tests/Data/BuildConfig/PartialAddressSanitizer/**`
- `Scripts/Hardening.py`, `Scripts/Lib/hardening.py`, `Scripts/Lib/process.py` (the memory query), `Scripts/Lib/toolchain.py` (locating the ASan runtime), `Scripts/Setup.py` (the missing-runtime warning)
- `Scripts/Test.py` hunks:
  - `--build-root`, `--sanitizer` (with `asan_environment`), `--test-timeout-scale`;
  - the `-Asan` report stems and the sanitizer report parsing;
  - `--fuzz-scale 0.1` on Debug unit runs, only once decision 14 is approved.
- `Tests/Source/Support/{Fuzz, TestOptions, TestTimeout}.*` and their tests; `Tests/Data/Fuzz/**`
- each reader's fuzz case in its unit's existing `<Unit>Tests.cpp` (decision 13), those cases only. In `GameManifestTests.cpp`, C appends its case and B only unskips its own.

**Stream D:**
- `Engine/Source/Engine/App/Application.{h,cpp}`, `Engine/Source/Engine/ImGui/StatsOverlay.*`
- the Luau-compiler investigation (`Engine/Source/Engine/Scripting/{ScriptApiRegistry,Sandbox}.cpp` if it lands)
- `Scripts/CheckRuntimeMap.py`, `Scripts/Lib/runtime_map.py`, `Tests/Data/MapFiles/**`
- `Scripts/Lib/portability.py`, `Scripts/Perf.py`, `Scripts/Lib/perf.py`
- tests: `Tests/Source/Engine/ImGui/StatsOverlayTests.cpp`, `Tests/Source/Engine/App/ApplicationTests.cpp`, `Tests/Automation/{test_runtime_map, test_portability, test_perf}.py`

**Lead (shared integration files, integration, completion):**
- the premake files (`premake5.lua`, `Dependencies.lua`, `*/premake5.lua`, `Vendor/*/premake5.lua`), `Scripts/ModuleRules.json`, `.gitignore`
- `BuiltinComponents.h`, `RegisterBindings.cpp`, `RegisterMethods.cpp`
- `Docs/Reference/*`, `Tools/MCP/catalog.json`, `Resources/Scripting/Engine.d.luau` (regenerated once after merging)
- `Scripts/CI.py`, `Scripts/PreCommit.py`, `Scripts/Lib/scripts.py`, `.github/workflows/ci.yml`
- `Docs/Architecture.md`, `Docs/Roadmap.md`, `AGENTS.md`, `Docs/CodeStyle.md`, `Docs/ReviewChecklist.md`, `Docs/Decisions/**` (the `0001-approvals.md` row of decision 5 included), `.claude/skills/**`
- call-site adoption: the F1 overlay in `RuntimeApp.cpp` and its `RuntimeAppTests.cpp` case, after B and D merged
- the completion review (ADR 0025) and the Linux and macOS catch-up

## Approvals

No new download approval is needed:

- The ASan runtime is part of the installed Visual Studio toolchain. It is never downloaded or redistributed.
- The macOS bundle redistributes the pinned Vulkan SDK's loader and MoltenVK (Apache-2.0, permissive). Their license texts are committed under `Resources/Licenses/` and listed in the notices. ADR 0001 requires the commit that adds an item to add a table row pointing at its pin, so the lead adds a `0001-approvals.md` row (source: the pinned Vulkan SDK of `Scripts/Lib/Toolchain.json`; license; destination `Runtime/Frameworks` of macOS builds; the per-file SHA-256 in `Build.py`'s report) in the milestone commit that lands A's staging (review finding 10).
- A vendor update that a fuzz finding requires uses ADR 0001's standing download permission (official source, pinned version, checksum and license committed).

Anything else that needs the operator goes to the lead; decision 14 is such an item.

## Risks

1. **Unknown per-mode coverage.** No exported FeatureTest run has happened. Gaps mean scaffold changes and re-recorded replays, whose hashes change (decision 6). Stream B exports FeatureTest with `project.export {testing: true}` on a copy early in its task, before writing the suite, so gaps surface first.
2. **The crash handler under ASan.** ASan intercepts `SetUnhandledExceptionFilter` and handles access violations itself, which affects about 20 crash-path tests. Stream C's first task is a spike of the `handle_segv=0` family of options. If no option set keeps them meaningful, C brings the evidence to the lead; no test is excluded silently.
3. **The Vulkan driver under ASan** (the ASan feature suite) is unverified, and ADR 0015 decision 27 already records an NVIDIA driver crash. It is spiked with the crash handler.
4. **Mixed instrumentation** fails the link (LNK2038) or reports false container overflows. Workspace scope and the `windows-asan` check prevent it.
5. **Time:** a milestone gate takes about 2 hours 10 minutes the first time (decision 19). The budgets are per step, and over-budget steps are raised, never silently trimmed.
6. **Soak false positives** from buffers that fill, GC steady state, fragmentation and driver allocations. The baseline window starts halfway, the heap and counter rules are absolute, and stream C measures the noise before the thresholds are relied on (decision 15).
7. **The Dist strip touches `Application`**, which every application uses. The editor and the Release Runtime keep ImGui; the map check, the editor screenshot goldens and the GPU suite at integration prove both sides.
8. **Resource patching on Windows:** antivirus locks are handled with a bounded retry, and determinism with a test. Every exporting test uses a real build output (decision 2), which costs a copy of the Runtime per export (about 45 MB in Debug).
9. **macOS code is verified only remotely** (ADR 0011). The writers and the assembly are host-independent and unit-tested everywhere; `codesign` and the loader lookup are the macOS-only remainder.
10. **Hunk ownership** in `Test.py` (B and C), and the merges in `GameManifestTests.cpp`, need the lead's care at integration. The hunks are disjoint and named.
11. **Remote drift:** eight milestones of Linux and macOS changes are unverified (decision 20). The catch-up may surface design issues, which are raised at once.
12. **ASan stack depth:** recursion-depth tests may overflow 1 MB with ASan's redzones; the sanitized Release executables get Debug's 2 MB stack (decision 12). If the spike shows more is needed, C brings the measurement to the lead.
13. **Debug unit time without the prefix:** while decision 14 waits, the Debug unit suite keeps its fuzz time and the new cases add at most 60 s.

## Verification plan (ADR 0023)

- **Streams never run `PreCommit.py` or `CI.py`.** Each builds what it changed (Release where Debug adds nothing), runs the affected test cases (`Tests.exe -tc=…`, `Test.py --suite <suite> --filter …`), shows each regression test failing against the reverted code by running that case alone, and finally runs, once, the suites its change can affect:
  - **A:** the EditorCore export cases, the resource and bundle cases, the export Python tests, and one real Windows export with its probe.
  - **B:** the testing-export and Runtime cases, the export suite and the determinism suite (`Test.py --suite export,determinism --config Release,Dist --junit --require-gpu`, after `Test.py --suite feature --config Release --junit`).
  - **C:** the Fuzz-suite cases, the ASan workspace build, the ASan unit and feature suites, one full fuzz run and one soak run through `Hardening.py`, and `CheckBuildConfig.py` with its new fixture.
  - **D:** the map check on the real Dist map, the editor's GPU and golden cases touched by the ImGui strip, the portability check on generated workspaces, and one `Perf.py` run.

  Each stream reports its measured times.
- **The contract:** the lead's fix pass applied both reviews and runs `PreCommit.py --contract` once.
- **Integration:** the lead merges the streams on the contract commit, adopts the call sites, wires `CI.py`, `scripts.py` and the `Test.py` hunks, and regenerates the references. It then makes each new or changed stage green with targeted runs (`CI.py --stages <stage>`, the `Test.py` suites, `Hardening.py --steps …`). Nothing is committed in between.
- **Review before the gate:** the milestone review and the completion review (ADR 0025), with their fixes, come before the single `CI.py --milestone` run on the final tree, hardening included (ADR 0023 decision 2); that run gates the one milestone commit. Documentation-only edits afterwards keep the result (ADR 0023 decision 3).

## Before milestone completion

- [ ] Every `ENGINE_CONTRACT_STUB` and every skipped test of this contract is replaced. One strict `CI.py --milestone` run is green on Windows: every stage, the clang-cl builds and `hardening` included.
- [ ] The testing export of FeatureTest passes `--feature-test` in Release and Dist with the six per-mode gates evaluated and green against their own mode; the merge over Editor, Release and Dist reports no failure; and the 18 per-mode seeded-gap fixtures and the merge cases still fail exactly as expected (M14's tests and this contract's mode-checked case).
- [ ] The determinism stage shows identical hashes for `Determinism.replay` and every FeatureTest replay across Editor Debug, Editor Release, Exported Release and Exported Dist, equal to the committed values, from current reports. Any changed committed hash is recorded here with its reason.
- [ ] "Export: resources carry the project icon, version and company", "Export: user data goes to the manifest Name folder" and the CI step "Dist: map file contains no automation, Analysis, ImGui or AssetPipeline symbols" pass. The Dist map holds the harness's required symbols.
- [ ] The ASan unit and feature suites are clean; the fuzz stage ran 1,000,000 iterations of every reader of `Targets.json`, clean; the soak passes every rule for every scene of every project and for the Runtime; `Perf.json` is written (its warnings are recorded, not gated).
- [ ] Notices are generated on every OS and packaged; `bin/Symbols` holds the Dist symbols; the Linux RPATH and the macOS settings pass the portability check; the `0001-approvals.md` row for the macOS redistribution exists.
- [ ] The references are regenerated (`project.export`'s schema), and the AGENTS.md command table, the build-and-test, commit-review and game-building skills and Architecture §2.3, §12.1, §13.4, §13.9, §14, §15.1, §15.8 and §16 describe what landed.
- [ ] The completion review (ADR 0025) has a disposition for every Architecture subsection and every item of decision 21, and each deviation is recorded.
- [ ] The Linux and macOS catch-up, including a macOS run of an exported bundle (signature, loader lookup), is scheduled before M16 (decision 20).
- [ ] Decision 14 carries the product owner's answer, and `Test.py` matches it.

## Architecture and Roadmap amendments

This contract amends the documents where a decision changes their text:

- Architecture §2.3 (the script table, `CI.py --milestone` and its refused combinations), §4.1 (exit code 5), §12.1 (no `--validate` or `--timeout`; `--export`'s exit codes and refusals), §13.9 (`--log-level` filters output only), §14.1 (the macOS output directory, the testing default directory), §14.2 (the phases, validation's output rule and suite scenes, the host-independent bundle assembly, the default icon, `ResourcePatcherWindows.cpp`, the timings), §14.3 (the bundle's testing user data), §14.4 (where the map check runs, its formats, families and matching), §15.1 (T3's gate for the exported modes, T3d, T6's soak), §15.8 (the export, determinism and hardening rows, `--milestone`) and §15.9 (`CI.py --milestone`).
- Roadmap rule 1 (`CI.py --milestone`), M15's deliverables (`Platform/ResourcePatcher`) and acceptance (`CI.py --milestone`), and the parallelization line (the pointer to this ADR).
- AGENTS.md (milestone workflow rule 1, the commit gate, the commands table, the remote CI paragraph), `Docs/ReviewChecklist.md`, and the `commit-review` and `build-and-test` skills (decision 19).

The completion review amends the rest.

## Review dispositions

Two contract reviews ran read-only on the first contract tree: a spec lens (findings 1 to 28) and a design lens (M1 to M5, m1 to m11, n1 to n6). Where they overlap, one disposition covers both.

| Finding | Disposition |
|---|---|
| 1 (blocker) and 7: A's requirements break B's frozen export tests; macOS staging | Fixed: the shared fixture `Support/ExportBinaryRoot` (implemented), used by every exporting test B owns and every M15 exporter case; A moves the older `ExporterTests` cases with its change; B replaces the split C++ testing-refusal case (decisions 1, 2, 6). |
| 2, M2: the ASan runs cannot start exported Runtime copies | Fixed: `hardening.asan_environment` puts the runtime first on `PATH` for every process (decision 12). |
| 3, M4: `--milestone` leaves the standing rules unamended | Fixed: Roadmap rule 1, AGENTS.md, §2.3, §15.9, the checklist and both skills amended; plain `CI.py` reports hardening as not run (decision 19). |
| 4: T6 loses the per-demo soak; the Roadmap narrowed | Fixed: every scene of every project, golden scenes included, so the Roadmap needs no amendment and T6 keeps "per demo" (decision 15). |
| 5: the portability check against `GenerateMapFile` | Fixed: `/MAP` in the Link `AdditionalOptions` (decision 18). |
| 6, m1: the binding fuzz conversion | Fixed: the index mapping, per-call reset, ratio thresholds and minimum, and `--fuzz-index` for a single iteration (decision 13; `Fuzz.h`, `TestOptions.h`). |
| 8: perf format disagreement | Fixed: `frameMs` and `simulationMs` per scene, one memory object (decision 16, `perf.py`). |
| 9, m10: notices completeness | Fixed: derived from generated projects (`referenced_vendor_libraries`); Dear ImGui `yes` for every configuration (decision 8). |
| 10: approvals for the macOS redistribution | Fixed: the `0001-approvals.md` row is the lead's, in the commit that lands A's staging; `Build.py` stages only the pinned SDK and records SHA-256 (decision 5, "Approvals"). |
| 11: one path glob is not one | Fixed: shared vectors run by C++ and by `Test.py`'s `glob_regex` (passing now); callers strip `project://` (decision 7). |
| 12, m5 (part): no positive `--renderer none` export test | Fixed: the GPU-suite case exists and already passes, so it is unskipped (decision 2). |
| 13, m6: the 1M-iteration loophole | Fixed: no exception; `read_fuzz_targets` refuses other counts; the C++ case checks it (decision 13). |
| 14: map family definitions | Fixed: Automation and ImGui families from all their headers' types and free functions; library origins versus object stems defined; the `LuauCompiler` extension approved (decision 11). |
| 15: weak staleness checks | Fixed: `report_inputs` and `stale_report_reason` against the Runtime, the exporting editor and the project files (decisions 9, 10). |
| 16, m3: `--log-level` interactions | Fixed: output sinks only; `--expect-no-errors` and `log.read` unaffected; Dist `trace` acts as `info` (decision 17, `Application.h`, §13.9). |
| 17: the F1 call site has no owner or test | Fixed: `StatsOverlayToggle` with a frozen case; input never consumed; the lead's call-site case named (decision 17). |
| 18, M3: the soak measures the wrong process; rules fail without a leak | Fixed: a Runtime soak added; absolute heap and counter rules; zero baselines defined; noise measured first (decision 15). |
| 19: macOS assembly compiled by no local check | Fixed: `CopyAppBundleBinaries` and `WriteAppBundleResources` are host-independent and unit-tested on every OS; only `codesign` stays macOS-only (decision 5). |
| 20: per-mode evaluation lacks an exported-mode test | Fixed: `evaluate_feature_report(expected_mode)` with a passing test over the Release and Dist fixtures (decision 9). |
| 21: fuzz reader list incomplete | Fixed: EditorPreferences, AutosaveRecovery and ProjectLock added; the session file is read only by the Python clients (decision 13). |
| 22: ownership gaps | Fixed: `test_build_script.py` to A, `harness.py` and `fake_editor.py` to B, the changed-headers sentence completed ("Ownership", "Frozen interfaces"). |
| 23: settle `--validate` and `--timeout` now | Fixed: not added; §12.1 and §4.1 amended (decision 3). |
| 24: no tests for exit code 3 and `--export-root` | Fixed: frozen in `test_export_cli.py` and `ExporterTests`; the in-process `--export-root` case passes now (decision 3). |
| 25: the `PhaseTimings` comment | Fixed: every phase entered (decision 2, `Exporter.h`). |
| 26: Roadmap names `Platform/Windows/ResourcePatcher` | Fixed (Roadmap M15). |
| 27: two full CI runs | Fixed: one milestone commit gated by one `CI.py --milestone` (decision 19). |
| 28: `--milestone` combinations undefined | Fixed: usage error with every selection and mode option (decision 19). |
| M1: `--export <dir>` can delete any directory | Fixed: `OutputPath` refusal rules and the output-directory rule for every export, with a frozen case (decisions 2, 3). |
| M5: the Debug fuzz prefix weakens acceptance cases | Accepted: decision 14 waits for the product owner; until then Debug runs full counts. |
| m2: fuzz memory and time unmeasured | Fixed: `fuzz_process_count` bounded by memory, `quarantine_size_mb`, in-memory iterations, per-target costs recorded by the spike (decisions 12, 13). |
| m4: the determinism stage drops drifted hashes | Fixed: `read_replay_hashes` returns every non-empty hash (decision 10). |
| m5 (rest): frozen interfaces without failing tests | Fixed: the in-process `ExportRoot` case (passing), the `project.export` fields case, the `Build.py` helper cases (decisions 2, 3, 6). |
| m7: GNU ld and lld lumped together | Fixed: a separate `lld` format (decision 11). |
| m8: ASan stack size | Fixed: risk 12 and the `/STACK` premake hunk (decision 12). |
| m9: fuzz lines lost to log rotation | Fixed: read from each process's standard error (decision 13). |
| m11: the bundle may not find its loader | Fixed: the explicit Frameworks path, A's hunk of `VulkanDispatch.cpp`, verified by the macOS job (decision 5). |
| n1: suite scenes not loaded strictly | Fixed: wired by the contract (decision 6). |
| n2: `EditorCommandLine.h` includes the whole exporter | Fixed: `ExportConfiguration.h` (decision 1). |
| n3: exit 3 from any `NotFound` | Fixed: `Exporter::IsMissingBuildOutput` (decisions 2, 3). |
| n4: apostrophes in fuzz case names | Fixed: names hold none; `ReportFuzzRun` fails such a case (decision 13). |
| n5: §14.1 lacks the testing directory | Fixed (§14.1). |
| n6: the overlay's drawing and toggle untested | Fixed in part: the toggle is pure and frozen; stream D adds the device-less drawing case (decision 17). |
| Open question 5 (both reviews): the Linux and macOS catch-up before the milestone commit | Declined: the product owner's standing instruction (ADR 0011) keeps it after the milestone commit and before M16; finding 19's concern is met by decision 5 (decision 20). |

## Open for the operator

- **Decision 14:** may the Debug unit run execute the first tenth of each fuzz case's sequence (`--fuzz-scale=0.1`; Release keeps the full counts, hardening runs 1,000,000; about 150 s saved on every PreCommit)? Until the product owner answers, Debug runs every case's full count.
