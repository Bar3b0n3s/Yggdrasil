# 0022 — M14 FeatureTest, coverage gates and documentation contract

- **Status:** accepted; the contract was committed as `3f5a3e7` after the spec and design reviews (dispositions under "Contract review"), and the four streams' results were integrated on it (decisions under "Integration"); the milestone review's findings were fixed before the milestone commit ("Integration", "Review fixes"). The contract commit ran `PreCommit.py --contract`; every later commit is strict (ADR 0004).
- **Date:** 2026-10-13.
- **Base:** `c6adfa2` (review fixes for M9, M10 and M13; ADR 0021).
- **Authority:** Roadmap "Rules for every milestone" and M14; Architecture §2.3, §11.8, §11.9, §11.10, §13, §15.1, §15.5, §15.6, §15.8 and §15.9; ADR 0004, ADR 0008 decision 16, ADR 0019 and ADR 0021. Remote CI stays non-blocking (ADR 0011).

## Context

M14 completes `Projects/FeatureTest`, evaluates the coverage gates of §15.6 per run mode, adds the determinism stage over the two editor configurations (§15.1 T3d), generates the remaining reference documentation and completes the agent skills. Four parallel streams implement it (Roadmap M14 "Parallelization"):

- **A:** the gates, per-mode evaluation, the cross-mode merge, the seeded-gap fixtures, the test-run reports, the feature and determinism stages and the scaffold runner.
- **B:** the FeatureTest scenes and suites for rendering, physics, audio and formats.
- **C:** the scenes and suites for the scripting API, callbacks, prefabs, input, timing, errors, `QuitApp`, `SceneLoad` and `Determinism.replay`.
- **D:** the generated reference, gate 7, the skills, the validation fixtures, the settings observation test and the `type` reload error.

Four read-only state maps (gates, project, docs, fixtures) established the starting point at `c6adfa2`. The facts this contract relies on were checked again in the code:

- No gate is evaluated anywhere, and the only coverage producer is `ScriptApiRegistry`. Its kinds collapse in both host adapters, so gate 2 cannot be told apart from gate 4.
- `--run-tests` writes its reports into the automation output directory, and the server deletes the JSON at exit.
- `ProjectSettings` rejects the zero delta that ends §15.5's Timing clock.
- `Editor --batch` seeds ids from the OS CSPRNG, and a batch writes a new asset's `.meta` only when something scans (`scene.save` alone leaves none).
- `.exr` is registered only so that it can be refused.
- Neither the Runtime nor Dist has an importer registry.
- Nothing produces `ScriptErrorKind::Type`.
- `Export.Icon`, `Version` and `Company` have no consumer.
- Validation codes carry no machine-readable documentation.
- No M8 golden scene has a shadow-casting light. Every golden with one (AnnotatedScreenshot, SelectionOutline, ShadowsNear, ShadowsFar, ShadowsOrtho, SpotShadows) renders through `RendererIIGoldenFixture`, which pins `ShadowMapSize` 1024.
- `Test.Fail` and `Test.Skip` outside an active case fail the case lookup and raise a catchable script error, and dispatch has already counted the call.

## Decisions

### 1. What the contract freezes

The contract freezes the public C++ headers listed under "Frozen interfaces", the data formats under "Data formats", the CLIs and the Python module API of `Scripts/Lib/coverage.py`.

- **C++ stubs:** every new C++ function is a stub whose first statement is `ENGINE_CONTRACT_STUB();`. A stub returning `Result` or `Status` returns `Unsupported`; any other stub returns an empty or neutral value.
- **Acceptance tests:** they carry the Roadmap's verbatim acceptance names and are marked `doctest::skip(true)`. Each body fails when forced to run: it calls a stub, reads a fixture that does not exist yet, or ends in an explicit `FAIL` that names what the owning stream must implement.
- **Python:** stubs raise `NotImplementedError("contract stub ...")`, and the Python tests carry `unittest.skip` (ADR 0008 decision 16).
- **Other scripts:** nothing is added to `Test.py`, `CI.py`, `GenerateDocs.py`, `FetchAssets.py` or the fixture generators, and `Scripts/Scaffold.py` does not exist yet. A stub there would fake a result or break the static checks, which run in contract mode too. Those CLIs are frozen here only.
- **Behaviour the contract implements:** no existing behaviour changes, except:
  - the new `Testing.CoverageGates` setting (decision 3). It is registered, `Tests/Data/Project/AllSettings.eproj` sets it to `true` (the gate 8 non-default test requires every field to be non-default), and `Docs/Reference/Methods.json` is regenerated for it. `Projects/FeatureTest/FeatureTest.eproj` gains the default `"CoverageGates": false` through `project.setSettings` in a batch run (so `Automation/Provenance.json` records it), keeping the file canonical;
  - `Editor --id-seed` (decision 8), with its tests, so streams B and C can check their scaffolds from the start;
  - `DumpReferences` (decision 13), which takes `--dump-reference` out of `EditorApp.cpp` unchanged;
  - the regression test of decision 4 for `Test.Fail` and `Test.Skip` outside an active case, which pins existing behaviour.

### 2. Coverage counters: one id and kind scheme for every producer

`TestCoverageCounter` keeps its M13 shape (`Id`, `Kind`, `Modes`, `Count`); `Testing/CoverageGates.h` freezes its vocabulary. Each kind belongs to exactly one gate, and each id starts with that gate's prefix:

| Gate | Kinds | Id |
|---|---|---|
| 1 | `component`, `field` | `component:<Component>`; `field:<Component><pointer>` |
| 1b | `enum-value` | `enum:<Enum>/<Value>` |
| 2 | `function`, `method`, `operator`, `constructor` | `api:<Owner>.<Member>` |
| 2 | `property-read`, `property-write` | `api:<Owner>.<Member>/read`, `/write` |
| 2 | `argument-value` | `api:<Owner>.<Member>/argument<N>/<Enum>/<Value>` |
| 3 | `callback` | `callback:<Owner>.<Member>` |
| 4 | `proxy-read`, `proxy-write` | `proxy:<Component>.<Field>/read`, `/write` |
| 9 | `format` | `format:<Importer>/<extension>` |

- **Field ids:** `<pointer>` is the RFC 6901 JSON pointer of a serialized leaf below the component's JSON, with its leading `/` and the `~0`/`~1` escapes. Every array element and every map value is the single token `*`, so map keys never appear and a literal `*` key cannot collide with a field name. Examples: `field:Camera/Projection`, `field:Prefab/Overrides/*/Kind`.
- **Modes:** the run modes in which the entry exists.
  - Components, enum values and formats exist in all three modes. Fields and proxy fields follow `FieldMeta::Modes`; script members and callbacks follow their `RunModes`.
  - Every entry exists in the Editor: `RunModes` values are `EditorOnly` or `All`, and nothing is registered for Release or Dist alone. The Editor report therefore lists every counter of every mode, which the merge relies on (decision 6).
  - `EvaluateCoverageGates` rejects a counter that breaks either rule.
  - A report lists only the counters available in its own mode, so an `EditorOnly` entry never appears in a Release or Dist report.
- **Reports:** a report lists every counter of its producers, covered or not, sorted by id, with counts saturated to `uint32`.
- **Script API conversion:** the host adapters' duplicated conversion is replaced by `MakeScriptApiCoverageCounters`. Property and proxy-field counters exist only where `RequiresRead`/`RequiresWrite` say, so a read-only field has no write counter. Existing M13 tests that expect the old ids (`Test.ReloadScript`) or kinds (`call`) are updated to the new ones; their assertions keep their strength.
- **`RegistryFingerprint`:** it stays the XXH64 of the generated declarations, but the modes are never compared on it. `ScriptApiRegistry::Freeze` imports every reflected enum, and the three modes register different automation types, so the fingerprint differs by design.

### 3. Opt-in, evaluation and the run's result

- **Opt-in:** coverage is recorded and the gates are evaluated only for a project whose new `Testing.CoverageGates` (bool, default `false`) is `true`. `Projects/FeatureTest` sets it; the demo games do not.
  - A project setting rather than a CLI flag keeps the Roadmap's acceptance command literal (`Editor --headless --project Projects/FeatureTest --run-tests`).
  - The setting travels with the project into M15's testing exports, so `--feature-test` evaluates the gates there without new options.
  - Gate 8 covers the setting: `AllSettings.eproj` sets it, and the settings observation test observes it through the runner.
- **Evaluation:** only an unfiltered (`Filter` empty), non-recording run that completes evaluates the gates. `FeatureTestRunner` evaluates them in `Complete()` with `EvaluateCoverageGates(counters, options.Mode)`. `Coverage.Gates` then holds the six per-mode gates in the order 1, 1b, 2, 3, 4, 9, and `Passed` additionally requires every gate to pass.
- **Evaluation errors:** if a recorder or `EvaluateCoverageGates` fails, the run fails. `Gates` is `MakeUnevaluatedCoverageGates()`, `Passed` is false, and the error is logged at Error through the Engine logger, naming the mode.
- **Unevaluated gates:** a filtered, recording or cancelled run of a coverage project reports `MakeUnevaluatedCoverageGates()` (`Evaluated` and `Passed` false), and its `Passed` ignores them. Without the opt-in, `Gates` is empty and `Counters` holds the script counters only, as today.
- **No vacuous pass:** a gate with no counter available in the mode fails with the single entry `<no counters>`, so a missing producer can never pass.
- **JUnit:** when `Gates` is not empty, `TestRunResultToJUnit` appends a `testsuite name="FeatureTest.coverage"` with one `testcase name="gate <g>"` per gate. A failed gate carries a `<failure>` that lists each missing id on its own line; an unevaluated gate carries `<skipped/>`.

### 4. Producers of gates 1, 1b, 2, 3 and 4

- **Gates 2, 3 and 4 (script API):** both hosts take the baseline `ScriptApiRegistry::GetCoverage(mode)` immediately before `FeatureTestRunner::Begin`.
  - This replaces the Editor's after-Begin and the Runtime's before-Begin snapshots, which disagreed.
  - Counting happens only in test-mode sessions. Replay sessions are not test mode, so replays add nothing to gates 2 to 4. The Input suite covers the input API through `Test.Inject*`, and `Input.replay` proves replay behaviour (§15.5).
  - Terminal calls are covered without failing the run:
    - `Test.Fail` and `Test.Skip` through `pcall` where no case is active (a suite body, which collects the cases). The case lookup fails, so each raises a catchable script error, and dispatch has already counted the call on entry. §11.10 documents the behaviour, and the implemented regression test "TestRunner: Test.Fail and Test.Skip outside an active case raise a catchable error and still count as called" pins it.
    - `Application.Quit` by the `QuitApp` suite.
  - FeatureTest permits no skipped case row (decision 7).
  - Per-value counters exist only for enum parameters of at most 16 values (`ScriptApiRegistry::Freeze`), so `Key` needs none.
- **Gates 1 and 1b (scenes):** `SceneCoverageRecorder` records every scene of a runner-owned suite or replay session after setup, after each scene replacement (a change of `GetSceneGeneration`) and before teardown. The recording-verification session is not recorded.
  - The runner creates the recorder at the first recorded scene from that scene's `Scene::GetTypeRegistry()`, because `IFeatureTestHost` exposes no registry. Every session of one host shares that registry; a scene with another registry fails the run with `InvalidState`.
  - An entity created and destroyed between two recording points is not seen, so coverage entities live for the whole suite.
  - Golden scenes render only in the doctest goldens and never count.
- **Field leaves:** serialized fields (`FieldMeta::Serialized`), recursing into structs and into the elements of arrays and maps of structs. A top-level leaf compares with the component's `MakeDefaultJson`; a leaf inside an array or map element compares with the element struct's `MakeDefaultJson`, since the component's default has no elements. Other values are compared whole. The entity key `Active` is not a reflected field and is not a counter.
- **Gate 1b scope:** every registry enum reachable from a serialized component field, recursively. That includes `PrefabOverrideKind` through `Prefab.Overrides[*].Kind`, so every override kind must occur in a simulated scene. This narrows §15.6's "every serialized enum" to what a scene can hold:
  - Project-settings enums are gate 8's, and automation enums are outside the gates.
  - Asset-file enums (`AlphaMode`, `SoundWave`, the import-setting enums) are deliberately not gated per value in any mode. Their values are covered by the M6 to M12 importer and asset tests (the glTF alpha-mode fixtures and the AlphaModes golden, the `SoundSynth` bit-exactness tests, the importer settings tests), and gate 9 requires a load of every format that carries them. §15.6 records the narrowing.

### 5. Gate 9: source formats and the load observer

- **The format table:** `Engine/Asset/SourceFormats.h` states the editor importers' importer and extension pairs in the Asset layer, because AssetPipeline does not exist in the Runtime or in Dist. A unit test keeps the table equal to `RegisterBuiltinImporters` (ids, extensions and main types).
  - `.exr` is the only row with `Importable = false`; it is claimed only to be refused with a hint, and gate 9 excludes it.
  - `.jpeg` is a separate row and needs its own fixture.
  - This deviates from §15.6's literal "every supported extension".
- **Observing loads:** `AssetManager` gains `SetLoadObserver` and a protected `NotifyLoaded`.
  - `EditorAssetManager` and `RuntimeAssetManager` call it after each successful `Load` return (an asset already loaded included) and each `LoadAsync` publication of a registered asset. They never call it for a built-in, a placeholder or a failure. They call it with no internal lock held and outside any iteration over their own tables.
  - The observer may make const queries (`GetReferencePath`, `GetAssetType`, `GetMetadata`, `GetState`). It must not load, unload, publish, reimport or install another observer.
  - The runner installs the observer through the new `IFeatureTestHost::GetAssetManager()` and removes it whenever the run ends: on completion, on `Cancel`, when `Begin` fails after installing it, and in `~FeatureTestRunner`. This is a reviewed change to the frozen M6 interface. The test fakes compile unchanged, because the API is non-virtual.
- **What counts:** `FormatCoverageRecorder` counts each handle once. A handle seen since the last `Reset` returns after one set lookup, so per-frame loads cost nothing more and counts never depend on how many frames ran. For a new handle it maps `GetReferencePath(handle)` (cut at `#`, ASCII lower-cased extension) to its source format; that works identically from editor paths and from pak TOC paths. A format's count is its number of distinct loaded handles. Engine assets are ignored.
  - Any project path counts. The foreign formats of §15.5 live in `Assets/Formats/` by convention; the native ones (`.material`, `.prefab`, `.scene`, `.luau`, `.replay`) count wherever a suite loads them.
  - `.sfx` belongs to `Formats.scene` as §15.5 lists it.
  - A suite that relies on an asynchronous load waits until it is published, so the set of loaded handles never depends on worker timing.
- **GPU:** an `.hdr` import needs the GPU baker (§13.9), so a feature run needs a Vulkan device (decision 7).

### 6. Mode-independent gates, seeded gaps and the merge

- **Gate 5:** `Scripts/Test.py` (automation suite, unchanged behaviour) uses `coverage.uncovered_methods` and `coverage.read_called_methods`. Its seeded gap is `Tests/Data/CoverageGaps/Gate5/`.
- **Gate 6:** the validation fixtures (decision 11). The fixture set must trigger every `ProjectValidator::GetCodes()` entry. Its seeded gap is `CoverageGaps/Gate6/Expected.json` with `Exclude` (fixture directories left out) and `Missing` (the codes that are then untriggered). Stream D owns the logic and the fixture.
- **Gate 7:** `CollectDescriptions` over the editor's `TypeRegistry`, `ScriptApiRegistry` and `MethodRegistry`, which are the superset of every mode, and `FindMissingDescriptions` (empty or whitespace-only).
  - Both live in `Editor/Source/EditorCore/Reference/DescriptionInventory.h`, owned by stream D. Gate 7 is editor-only, so nothing of it reaches a Dist build, and `GenerateReferences` reuses the same walk to refuse an empty description (§5.4).
  - The registries assert descriptions at registration, so the seeded gap is a neutral inventory, `CoverageGaps/Gate7/Descriptions.json` (a `DescriptionEntry` list with exactly one empty description).
- **Gate 8:** the existing non-default test, plus the observation test (decision 12). Its seeded gap is `CoverageGaps/Gate8/AllSettings.eproj`, canonical with exactly one field at its default. Both tests use the shared walk `Test::CollectDefaultFields` (`Tests/Source/Support/DefaultFields.h`). Stream D owns it.
- **Per-mode seeded gaps:** for each gate of 1, 1b, 2, 3, 4 and 9 and each mode, `CoverageGaps/Gate<g>/<Mode>/FeatureTest-<Mode>.json` (a complete TestRunResult JSON with a small synthetic counter set) and `Expected.json` (`{"Format": "CoverageGap", "Version": 1, "Gate", "Mode", "Missing"}`).
  - Evaluating the report for its mode must fail exactly that gate, with exactly `Missing`, and pass the other five.
  - The report's own `coverage.gates` must equal `EvaluateCoverageGates` of its `coverage.counters`, so the Python feature suite (which reads the gates) and the C++ acceptance test (which re-evaluates the counters) judge the same fixture. The C++ test asserts it.
  - Every Dist fixture includes an entry that only the editor run covered (count 0 in the Dist report). The Editor fixtures of gates 1 (a field), 2, 3 and 4 include an `EditorOnly` entry; components, enum values and formats exist in every mode (decision 2), so gates 1b and 9 have none.
- **The merge** (`coverage.merge_coverage`, run by M15's export stage; M14 proves it on `CoverageGaps/Merge/<Case>/FeatureTest-{Editor,Release,Dist}.json` with `Expected.json {"Format": "CoverageGap", "Version": 1, "Failures": [...]}`). Each failure is one line `merge: <rule>: <subject>`. The rules, in their reporting order:

  | Rule | Fails when | Subject |
  |---|---|---|
  | `report` | a mode of `MODES` has no report or more than one, or a report names another mode | `<mode>` |
  | `passed` | a report did not pass | `<mode>` |
  | `skipped` | a report has a skipped case row | `<mode> <suite>/<case>` |
  | `gate` | a per-mode gate is absent, unevaluated or failed | `<mode> <gate>` |
  | `missing` | an Editor id whose modes contain the mode is absent from that mode's report | `<mode> <id>` |
  | `extra` | a Release or Dist id is absent from the Editor report, or its Editor modes exclude that mode | `<mode> <id>` |
  | `mismatch` | a Release or Dist counter's kind or modes differ from the Editor's | `<mode> <id>` |
  | `uncovered` | an Editor counter has count 0 in every report whose mode is in its modes | `<id>` |

  - Within a rule, lines are ordered by mode (`MODES` order), then by subject byte-wise.
  - The merge never compares fingerprints. The Editor counter set is the registered set (decision 2).
  - The cases are `Covered` (no failure), `Passed`, `SkippedCase`, `Unevaluated` (the `gate` rule), `Missing`, `Extra`, `Mismatch` and `Uncovered`. The `report` rule is proven in code (`test_merge_rejects_a_missing_or_duplicated_mode`).
  - Its CLI is `python Scripts/Test.py --merge-coverage <Editor.json> <Release.json> <Dist.json> [--json]`, with exit 0 or 1, printing the failure lines.

### 7. Reports, exit codes, the feature suite and the determinism stage

- **Editor reports:** `Editor --headless --project <p> --run-tests [--filter F] [--junit <file.xml>]` writes the JUnit report to `--junit` and the JSON report beside it, with the extension replaced by `.json`.
  - The default is `<working directory>/bin/TestResults/FeatureTest-Editor.xml` and `.json`. The name says which runner and mode produced the report, whatever the project; M15's export stage looks for these fixed names.
  - EditorApp writes them from the run's result, independent of the automation server's offload directory and its exit cleanup.
  - `--junit` exists only with `--run-tests` (the §12.1 CLI already names it).
  - Exit codes: 0 when every case, suite and evaluated gate passed; 1 on any failure or preflight error; 3 and 4 as before.
  - **Test hygiene:** every test that starts `--run-tests` passes `--junit` into its temporary directory or runs the editor there (`RunEditor`'s working directory, which the contract adds), and every test that starts `--feature-test` runs the Runtime in a temporary working directory. No test writes the repository's `bin/TestResults`. Stream A moves the existing `--run-tests` cases of `EditorAppTests.cpp` to this rule when it lands the reports.
- **Runtime reports:** `--feature-test` writes `<working directory>/bin/TestResults/FeatureTest-Release.json` and `.xml` (or `-Dist`), replacing `Runtime-<pid>`. Dist takes no new option. `Tests/Automation/test_script_modes.py` follows the rename.
- **Feature suite command:** `Scripts/Test.py --suite feature --config Debug|Release [--require-gpu] [--junit]` runs `Editor --headless --project Projects/FeatureTest --run-tests --audio-device none --expect-no-gpu-errors --junit <report>`.
  - With `--junit`, `<report>` is `bin/TestResults/Feature-<Config>.xml` (the §2.3 `<Suite>-<Config>` name), and the editor's JSON report lands beside it as `Feature-<Config>.json`.
  - Without `--junit`, `<report>` lies in a temporary directory that Test.py removes.
  - Test.py writes no other file.
- **Feature suite checks:** it fails, printing `UNCOVERED gate <g> (Editor): <id>` lines, when:
  - the editor exits non-zero;
  - any of the six gates is absent, unevaluated or failed;
  - any case row is skipped;
  - any file under `Projects/FeatureTest` outside `Library/` changed. It compares a byte snapshot taken before and after the run, so a run that rewrites committed metas fails.
- **Without a device:** without a usable Vulkan device the feature suite reports why and passes without running, ending as a warning; with `--require-gpu` it fails. This matches the automation suite's rule. PreCommit and CI pass `--require-gpu` unless `--gpu-optional`.
- **Determinism stage (§15.1 T3d, "editor Debug vs Release from M14"):** `Scripts/Test.py --suite determinism [--config Debug|Release]` becomes available.
  - For each configuration it starts `Editor --headless --renderer none --audio-device none` on a temporary copy of `Projects/FeatureTest` without `Library/`. It runs `input.replay {path, verify: true, strictHash: true}` for every file `Testing.Replays` matches and reads each `stateHash`.
  - It fails on a failed verification, on a hash that differs from the replay's committed `FinalStateHash`, or, when both configurations ran, on a hash that differs between them. Its JUnit report is `bin/TestResults/Determinism.xml`, with one testcase per replay.
  - It needs no GPU (decision 9's rule for replay scenes), so it runs on every machine.
  - M15 adds the exported Release and Dist runs (Roadmap M15).
- **CI:** CI.py's `feature` stage becomes available in Debug and Release, and its `determinism` stage in "Debug, Release editors". The Test.py and CI.py "M14" labels are therefore correct and stay.
- **Remote CI:** the feature stage stays with the local gate, as `.github/workflows/ci.yml` says; running it on the Linux Lavapipe job joins the ADR 0011 batch. The determinism stage is CPU-only, so it joins the hosted Windows job's stage list.

### 8. Deterministic scaffolds

- **`--id-seed`:** `Editor --id-seed <n>` (decimal, 0 to 2^64 − 1, only with `--batch`) sets `EditorContextSpecification::IdGeneratorState = Random(n).GetState()`. The contract implements it (parser, option table and the `EditorApp.cpp` call site). Its tests are "EditorCommandLine: --id-seed seeds the ids of a batch run" and "EditorApp: --id-seed makes a batch run's entity ids and asset handles reproducible", which proves that two seeded runs write identical scene and `.meta` bytes.
- **Scaffold files:** each M14 scaffold `Projects/FeatureTest/Scaffold/<Name>.jsonl` has no `project.open` line and runs once on the checked-in project with `Editor --headless --renderer <Renderer> --audio-device none --project Projects/FeatureTest --id-seed <seed> --batch Projects/FeatureTest/Scaffold/<Name>.jsonl`.
  - Each scaffold ends with `project.refreshAssets`, so every `.meta`, and with it every asset handle, is written inside the seeded run. A batch otherwise leaves new assets without metas, and the next unseeded open would write them with CSPRNG handles.
- **Run order and seeds:** `Scaffold/Scaffolds.json` (`{"Format": "FeatureTestScaffolds", "Version": 1, "Scaffolds": [{"File", "IdSeed", "Renderer"}]}`, canonical JSON) lists every M14 scaffold in run order with its seed. The lead writes it at integration with these frozen seeds:

  | File | IdSeed | Renderer | Owner |
  |---|---|---|---|
  | `Settings.jsonl` | 1501 | `none` | lead |
  | `Formats.jsonl` | 1502 | `vulkan` (`.hdr` import) | B |
  | `Components.jsonl` | 1503 | `none` | B |
  | `Physics.jsonl` | 1504 | `none` | B |
  | `Audio.jsonl` | 1505 | `none` | B |
  | `Scripting.jsonl` | 1506 | `none` | C |
  | `Callbacks.jsonl` | 1507 | `none` | C |
  | `Prefabs.jsonl` | 1508 | `none` | C |
  | `SceneLoad.jsonl` | 1509 | `none` | C |
  | `Input.jsonl` | 1510 | `none` | C |
  | `Timing.jsonl` | 1511 | `none` | C |
  | `Errors.jsonl` | 1512 | `none` | C |
  | `QuitApp.jsonl` | 1513 | `none` | C |
  | `Determinism.jsonl` | 1514 | `none` | C |
  | `Suites.jsonl` | 1515 | `none` | lead |
  | `Replays.jsonl` | 1516 | `none` | C |

  - The order is `Settings.jsonl`, B's scaffolds, C's scaffolds, `Suites.jsonl` (the whole `Testing.Suites` list and `Replays` globs, restated in one patch because arrays replace), then `Replays.jsonl`, which records the replays last.
  - A stream may reorder its own scaffolds, or need `vulkan` for another of them, and says so in its result. The seeds stay with the files.
  - B's scenes never reference C's assets; C's may reference B's, which run first.
- **`Golden.jsonl`:** it and its content stay untouched. The M9 regression test pins 30 of its lines, and golden UUIDs depend on them.
- **Baseline:** `Projects/FeatureTest/Scaffold/Baseline/FeatureTest.eproj` and `Baseline/Provenance.json` are committed copies of the project's state before any M14 scaffold (the golden content plus `"CoverageGates": false`). Later milestones extend the scaffold list, never the baseline, unless a change to the golden content itself is reviewed.
- **Regeneration:** `Scripts/Scaffold.py [--config Debug|Release] [--check] [--json]` (stream A) regenerates `Projects/FeatureTest`.
  1. Remove every output of the scaffolds: all of `Assets/` except `Scenes/Golden`, `Materials/Golden`, `Models/Golden` and `Textures/Golden`.
  2. Copy the baseline over `FeatureTest.eproj` and `Automation/Provenance.json`.
  3. Run `Scaffolds.json` in order.

  With `--check` it regenerates a temporary copy instead and fails, naming each difference, unless every file outside `Library/` is byte-identical to the repository's. A `vulkan` scaffold without a usable Vulkan device fails and names the reason.
- **Who runs it:** the lead regenerates once at integration with `Scaffold.py`. The lead-owned Python test `Tests/Automation/test_feature_test.py` ("the scaffolds reproduce the project byte for byte") runs `Scaffold.py --check` in the automation suite. It needs every stream's scaffold, so it is unskipped at integration.
- **Streams:** they may change `FeatureTest.eproj` and `Provenance.json` locally while developing (applying decision 9's settings themselves), but integration discards those copies. Before `Scaffold.py` lands, a stream checks its own scaffolds by running them twice from the baseline with their seeds and comparing the bytes.

### 9. The FeatureTest project

**Settings** (non-default where observable at run time; lead-owned, written by `Settings.jsonl`):

| Section | Values |
|---|---|
| `Name`, `StartScene` | `FeatureTest`; `Assets/Scenes/Components.scene` |
| `Window` | Title `FeatureTest`, Width 1280, Height 720; the rest default |
| `Simulation` | FixedHz 64, MaxStepsPerFrame 5, Seed 1414, MaxEntities 8192 |
| `Physics` | Gravity [0, −10, 0]; Layers [`Default`, `Ball`, `Track`, `Trigger`, `Character`, `Debris`] with `Default` first (golden annotations rely on it) |
| `Physics.Collisions` | Default–Default, Default–Ball, Default–Track, Default–Character, Default–Debris, Ball–Ball, Ball–Track, Ball–Trigger, Character–Track, Character–Trigger, Debris–Track, Debris–Debris |
| `Input.Actions` | `Jump` and `Fire` (Button); `MoveX` (Axis: keys plus `Gamepad.LeftX`); `MoveY` (Axis: keys plus `Gamepad.LeftY`, `Invert: true`). Stream C chooses the control names and lists them in its result |
| `Rendering` | ShadowMapSize 1024; `SsaoHalfResolution` stays `false`, because it would change the GtaoOn and GtaoOrtho goldens |
| `Scripting` | MemoryLimitMB 128, CallbackBudgetMs 500, PauseOnError false, BlockPlayOnTypeErrors true |
| `Testing` | `CoverageGates: true`; Replays [`Assets/Tests/Replays/*.replay`]; the limits default |

- **Fixed rate:** 64 Hz keeps the Timing clock's first cycle at 0, 1, 2 and 5 steps (accumulator 0.004, 0.004375, 0.006525, 0.0114, then the 0.25 s clamp), makes the fixed delta exact in binary, and gives 750 audio frames per tick. §15.5's "800 frames per tick" assumed 60 Hz. A non-default rate is required by §15.5 "Settings coverage".
- **MaxStepsPerFrame:** it must stay 5 for the Timing suite.
- **Rendering:** ShadowMapSize 1024 changes no golden. No M8 golden scene has a shadow-casting light (GltfFixture's `CastShadows: true` entries are mesh renderers; its directional light does not cast), and every golden with one renders through `RendererIIGoldenFixture`, which pins 1024. The golden stage at integration confirms it.

**Scenes and suites.** Scenes live in `Assets/Scenes/` (the golden directory keeps its 17 scenes), suites in `Assets/Tests/<Area>/`, and test assets in `Assets/Tests/<Area>/` or `Assets/Formats/`. Suite names are `"<Area>"` or `"<Area>.<Unit>"` (for example `Api.Entity`), unique, as `filter` matches `<suite>/<case>`. Every suite runs in all modes unless listed otherwise.

| Owner | Scene | Suites (`Testing.Suites` entries) |
|---|---|---|
| B | `Components.scene` and `Components/<Variant>.scene` | `Assets/Tests/Components/Components.test.luau` (round trip; renders through `Test.Screenshot`; loads each variant with `Scene.Load`) |
| B | `Physics.scene` | `Assets/Tests/Physics/Physics.test.luau` |
| B | `Audio.scene` | `Assets/Tests/Audio/Audio.test.luau` (`Test.CaptureAudio`) |
| B | `Formats.scene` | `Assets/Tests/Formats/Formats.test.luau` (loads, instantiates or plays every `Assets/Formats/` asset) |
| C | `Scripting.scene` | `Assets/Tests/Api/<Module or Type>.test.luau`, one per API module and userdata type (§15.5 list plus the types gate 4 needs); `Assets/Tests/Api/EditorOnly.test.luau` with `Modes: ["Editor"]` |
| C | `Callbacks.scene` | `Assets/Tests/Callbacks/Callbacks.test.luau`; `Assets/Tests/Callbacks/HotReload.test.luau` with `Modes: ["Editor"]` (`Test.ReloadScript`, `OnHotReload`) |
| C | `Prefabs.scene` | `Assets/Tests/Prefabs/Prefabs.test.luau` (every `PrefabOverrideKind` present) |
| C | `SceneLoadA.scene`, `SceneLoadB.scene` | `Assets/Tests/SceneLoad/SceneLoad.test.luau` with `Parameters` |
| C | `Input.scene` | `Assets/Tests/Input/Input.test.luau` (`Test.Inject*`); replay `Assets/Tests/Replays/Input.replay` |
| C | `Timing.scene` | `Assets/Tests/Timing/Timing.test.luau` with `Clock: [0.004, 0.016, 0.0334, 0.083, 0.5, 0.0]` |
| C | `Errors.scene` | `Assets/Tests/Errors/Errors.test.luau` with `Isolation: "Case"`, `Overrides: {CallbackBudgetMs: 50, MemoryLimitMB: 16}` |
| C | `QuitApp.scene` | `Assets/Tests/QuitApp/QuitApp.test.luau` with `ExpectQuit: 3` |
| C | `Determinism.scene` | replay `Assets/Tests/Replays/Determinism.replay` (600 ticks: a 50-body pile, a ball on a compound track, scripted forces, DetMath trigonometry and a non-trivial `^`) |

- **Variant scenes:** some components are unique per scene; `PostProcess`, for example, needs four scenes for its four `Tonemapper` values. B covers them with `Assets/Scenes/Components/<Variant>.scene`, which the Components suite loads with `Scene.Load`. The recorder records after each replacement (decision 4), and every export cooks every asset under `Assets/`, so this works in all modes.
- **Scenes without the GPU:** the scene of every replay (`Determinism.scene`, `Input.scene`), `Timing.scene` and anything they load must not need the GPU baker: no `.hdr` source and no environment asset from `Assets/Formats/`.
  - They load in GPU-less unit runs of a project copy without a Library cache, and in the determinism stage under `--renderer none`.
  - `EditorFeatureTestHost::Preflight` creates a session for every replay even in a filtered run.
- **The Timing filter:** "FeatureTest: the Timing suite passes with 0, 1, 2 and 5 steps per frame" selects with the case-insensitive substring filter `Timing/` and checks that exactly the suite `Timing` ran. No other suite name may end in `Timing`, and no replay path may contain `timing/`.

**Gate accountability.** Every suite contributes to every gate, but each counter has one accountable stream:

- **Stream B:** gates 1 and 1b for every component, field and enum value except those of C below. `Components.scene` and its variants set every such field to a non-default value somewhere and use every such enum value. Also gate 9 for every format except `.prefab`, `.luau` and `.replay`.
- **Stream C:** gates 2, 3 and 4. Also the gate 1 and 1b ids of the `Prefab`, `PrefabLink` and `Script` components (their fields, and every `PrefabOverrideKind` value) through `Prefabs.scene` and its own scenes, and gate 9's `.prefab`, `.luau` and `.replay`.

**Timing clock.** `ProjectSettings` accepts frame deltas ≥ 0 and rejects a clock whose entries are all 0, at load. Stream C owns this change, with the skipped test "ProjectSettings: a scripted clock may hold zero deltas but not only zeros". This aligns the validator with §11.10 and ADR 0019 decision 10. Suites derive their expectations from `Time.GetFixedDeltaTime()` and the float-stored deltas.

**Determinism replay.** `Determinism.replay` and `Input.replay` are recorded with `input.record` in `Replays.jsonl`, after the final settings and the final scaffold run. The committed `FinalStateHash` is verified with `StrictHash` in every configuration:

- by `Testing.Replays` in the feature run;
- by "FeatureTest: Determinism.replay matches its committed hash in both editor configurations" in the Debug and Release unit suites, without a GPU;
- by the determinism stage (decision 7), which also compares the Debug and Release hashes of every FeatureTest replay; M15 adds the exported runs.

Any change that alters the hash re-records it with a stated reason in the review. The verifier's hash never replaces the recorded one (ADR 0019 decision 6).

**No new script API.** M14 adds no script API: every API §15.5 needs exists, and `RegisterBindings.cpp` stays unchanged. Mouse movement, scroll and text input are exercised through `Input.replay`; their getters are called in the Input suite for gate 2.

### 10. Validation code documentation

`ProjectValidator::GetCodeInfos()` returns one `ValidationCodeInfo {Code, Severity, AutoFixable, Description, Fix}` per code, in `GetCodes()` order. `Severity` is `Error`, `Warning` or `Warning; Error at the limit`. `Validation.md`, the game-debugging skill's error-code table and gate 6 read it. §13.7 gains `ASSET_CONTENT_SKIPPED`, which the validator already reports.

### 11. Validation fixtures

`Tests/Data/Validation/<Name>/` holds `Project/` (a complete mini project: `.eproj`, `.luaurc` when it has scripts, `Assets/…`) and `Expected.json`:

```json
{ "Format": "ValidationFixture", "Version": 1, "Scope": "Project", "Codes": ["<every code the project reports, sorted>"],
  "TestComponents": false, "Expand": [], "NonCanonical": [] }
```

- **Naming:** `<Name>` is a code from `GetCodes()`, whose `Codes` must contain it, or `Clean`, a baseline with a primary camera, lighting and a listener that reports nothing. Fixtures start from that baseline, so incidental codes stay out.
- **`TestComponents`:** when true, the test component types (`ValidatorTarget`, `ValidatorFollower`, moved into `Tests/Source/Support/`) are registered. `COMPONENT_MISSING_REQUIREMENT` needs them, because every built-in `Requires` is `Transform`.
- **`Expand`:** lists `{"Path", "Entity", "Count"}` entries that the harness expands into a scene at test time, so `PHYSICS_LIMIT_EXCEEDED` (more than 16,384 bodies) and `RENDER_LIGHT_LIMIT_EXCEEDED` need no multi-megabyte fixture.
- **`NonCanonical`:** the project-relative paths of deliberately malformed files, which the authored-files test exempts.
- **Generator:** `Tests/Data/Generate/MakeValidationFixtures.py` (stdlib, `--check`, owns the directory and reports extra files) writes everything. It byte-copies the existing invalid scenes, glTF diagnostics fixtures and `Truncated.png`.
- **Placement:** fixtures stay out of `Tests/Data/Scenes/` and `Tests/Data/Assets/`, so the re-save and importer sweeps do not see them.
- **Canonical files:** they are canonical where the format allows. The authored-files test learns the one `NonCanonical` exemption, and is extended to `Projects/**` (excluding `Automation/` and `.jsonl`), as ADR 0006 decision 13 promised.

### 12. Settings observation (gate 8)

- **Observation rule:** the test loads `AllSettings.eproj` and observes every leaf JSON pointer through a production consumer, never by reading the settings struct back. It then compares the observed set with the reflected leaf walk and names each unobserved pointer.
- **Consumers:**
  - the play session getters (`GetFixedDelta`, `GetSeed`, `GetMaxEntities`, `GetViewSize`);
  - `PlayInput`;
  - the session's `PhysicsWorld::GetGravity` and `GetLayers`;
  - the extraction's `Quality`;
  - for `MemoryLimitMB` and `CallbackBudgetMs`, the session `ScriptEngine`'s enforcement (a budget timeout, a memory fault) or its effective limits, never `ScriptEngine::GetSettings`, which is a copy;
  - for `PauseOnError` and `BlockPlayOnTypeErrors`, `EditorPlayController`'s behaviour: it pauses play at a runtime error, refuses Play and refuses a reload (`CheckReloadTypes`);
  - the runner, for `Testing`, `CoverageGates` included;
  - the validator, for `StartScene` and `BuildScenes`;
  - the exporter, for `Exclude`.
- **Fixture project:** the test opens `Tests/Data/SettingsObservation/Project/` copied into its temporary directory together with `Tests/Data/Project/AllSettings.eproj`.
  - That directory holds every file `AllSettings.eproj` names: `Assets/Icon.png` (a PNG of at least 256 × 256), the start, build and suite scenes, the suite script and whatever else a consumer needs.
  - `MakeValidationFixtures.py` writes it as a second output root with `--check`.
  - Stream D owns it and `AllSettings.eproj` after the contract.
- **Game process settings:** `Engine/App/ProjectSettingsApplication.h` freezes `ApplyGameSettings`, which maps Name, Window and Simulation into an `ApplicationSpecification` and makes the Runtime factory's mapping observable without opening a window. It also freezes `ApplyTestSuiteOverrides`, which removes the identical copies in the editor and Runtime test hosts.
- **Export labelling:** `EditorCore/Export/ExecutableResources.h` freezes `ResolveExecutableResources`, the first consumer of `Export.Icon`, `Version` and `Company`. The exporter calls it before writing; it checks `major.minor.patch` with each part at most 65535, and an icon that is a PNG of at least 256 × 256. M15's resource patcher consumes its result.
- **Call sites:** the Runtime factory, the two test hosts and the Exporter adopt these functions at integration (see "Ownership").

### 13. Generated reference

- **File set:** `GenerateReferences` produces exactly the files of `ReferenceGenerator.h`:
  - `catalog.json`, `Methods.json`, `Engine.d.luau` and `ScriptAPI.md`, unchanged in format;
  - `Components.md`, `Automation.md`, `Validation.md` and `FileFormats.md`;
  - `Schemas/<Format>.schema.json` for AssetMeta, BatchRequest, GameManifest, Material, Prefab, Project, Provenance, Replay, Scene and SoundEffect.
- **Determinism:** generation is deterministic and byte-identical across operating systems.
- **Descriptions:** an empty description fails generation through gate 7's walk (`DescriptionInventory.h`).
- **Schemas:** they use only the subset `JsonSchema::Validate` supports. A single-value `enum` replaces `const`, and there is no `oneOf`. They allow unknown components and keys where §6 allows them.
- **Dump:** `Editor --dump-reference <dir>` calls only `DumpReferences` (`ReferenceGenerator.h`). The contract moved the M13 body there unchanged, so stream D completes the dump (`GenerateReferences` plus the built-in `ImporterRegistry`, then `WriteReferences`) without touching `EditorApp.cpp`.
  - The dump keeps the flat names its consumers read (`Methods.json`, `catalog.json`, `Engine.d.luau`, `ScriptAPI.md`) and adds the new files and `Schemas/`.
- **Mapping:** `GenerateDocs.py` maps `Methods.json`, `*.md` and `Schemas/**` to `Docs/Reference/`, `Engine.d.luau` to `Resources/Scripting/` and `catalog.json` to `Tools/MCP/`.
- **Fully generated:** `Docs/Reference/` becomes fully generated. `--check` fails on a missing, stale or extra file, and write mode deletes extras. Exit codes and atomic staging stay as they are.
- **Regeneration during the milestone:** a stream whose change alters the reference regenerates `Docs/Reference/`, `Tools/MCP/catalog.json` and `Resources/Scripting/Engine.d.luau` in its own worktree to keep its gate green. Those files stay lead-owned; integration discards the streams' copies and the lead regenerates once after merging.
- **`docs.get`:** unchanged. It serves `reference/<stem>` for each `.md` and offloads results over 48 KB as today. (Superseded by ADR 0026's session-1 fixes, friction S1-01: `docs.get` pages and sections every topic and never offloads.)
- **Catalogue tests:** the tests of ADR 0008 decision 9 (`EditorAppTests` and `test_rpc_discover_matches_the_dumped_catalogue`) stay.

### 14. Doc-example type checking

- **Scope:** every fenced block whose info string starts with `lua` or `luau`, in:
  - `Docs/*.md` and `Docs/Reference/*.md`;
  - `AGENTS.md` and `Projects/*/AGENTS.md`;
  - `Resources/Templates/**/*.md`;
  - `.claude/skills/*/SKILL.md`.

  `Docs/Decisions/` is excluded as history.
- **Attributes:** an optional `path=<Assets/...>` (default `Assets/Scripts/DocExample.luau`), `prelude=<Name>` and `skip="<reason>"`. The `Engine.d.luau` excerpt in §11.9 is skipped.
- **Stubs and preludes:** stub modules (`Board`, `Game`, `Ball`) and preludes and epilogues live under `Tests/Data/TypeChecker/DocExamples/`. `MakeScriptFixtures.py` generates them and lists them `Clean` in `Expected.json`, so the TypeChecker fixture test also checks them alone.
- **Pass rule:** zero Error findings, and at least `MinimumCheckedBlocks` blocks checked. That constant lives in `Tests/Source/EditorCore/Scripting/DocExampleTests.cpp`; stream D sets it to the count at completion, and it is raised as documents grow and lowered only with a reason in the review. A seeded document (`DocExamples/Seeded/Seeded.md`) must fail at its document line.
- **Where it runs:** in the unit suite, as "DocExamples: every Luau example in the docs type-checks".
- **Edits:** stream D may edit the info strings of Luau fences in any document. Edits to the text of `Architecture.md` go through the lead.
- **§11.9:** widened from "ScriptAPI.md and §11" to this scope.

### 15. Compile and type error fixtures; the `type` kind

`Tests/Data/Scripts/Errors/*.luau` and `Expected.json` (`{File, Kind, Code, Line, Column, Message}`) are generated by `MakeScriptFixtures.py` as a second output root. They cover compile errors (syntax, an unresolved require, an engine call at module top, a top-level runtime error, a load-time budget) and type errors.

- **The `type` kind:** `ScriptErrorKind::Type` gains its producer, `EditorPlayController::CheckReloadTypes` (frozen; stream D owns it).
  - It is the one type gate of a play-mode reload. The safe point's hot reload calls it (D), and `EditorFeatureTestHost::ReloadScript` calls it before reloading (the lead adopts that one-line call at integration). `Test.ReloadScript` therefore takes "the same path" (§11.8).
  - The gate applies to an ordinary session when `Scripting.BlockPlayOnTypeErrors` is set, and to every test session, because type errors always block test runs (§11.9).
  - When it applies and the latest import check of a script in the reload chain reports `SCRIPT_TYPE_ERROR`, it keeps the running code and publishes one `type` ScriptError (`Callback: "Reload"`) at the first finding's location, alongside the existing `compile` path. It returns a Script error naming that location, which `Test.ReloadScript` raises in the case.
- **Without the setting:** an ordinary session's reload proceeds as before.
- **Parity:** the C++ test "EditorPlayController: a reload refused for type errors keeps the running code and reports a type error" and the Python test `test_reload_refused_for_type_errors_reports_a_type_error` (`script.errors`, Roadmap rule 5) cover it.

### 16. Format fixtures

- **Generated:**
  - `Tests/Data/Generate/MakeFormatFixtures.py` (new, stdlib, MakeAudioFixtures' `--json` shape) writes `Tests/Data/Assets/Environments/Gradient.hdr` (32 × 16, flat RGBE scanlines) and `Tests/Data/Assets/Fonts/Glyphs.ttf` (a few TrueType outlines meeting `TrueTypeTables.cpp`'s table requirements).
  - `MakeTextureFixtures.py` adds `Tests/Data/Assets/Textures/Baseline.jpeg`.
  - `Tests --write-format-fixtures` is dropped, because JPEG has been first-party since M6. The Roadmap and §15.5 are amended accordingly.
- **Pinned OTF:** `extras/otf/Inter-Regular.otf` from the already pinned Inter 4.1 archive (same host, archive SHA-256 and OFL text), at `Tests/Data/Assets/Fonts/Inter-Regular.otf`.
  - It is 609,600 bytes with SHA-256 `d4f2b9e148059a15f014cb0f0b8fea8cd11bfa447dd483bedf1b0adc0e2ba799`.
  - It is a static CFF font (`OTTO`, a `CFF ` table, no `CFF2` and no `fvar`), which stb_truetype reads. This was verified at contract time against the pinned archive.
- **`FetchAssets.py fixture [--check]`:** it pins the OTF and the committed `Tone.mp3` and `Tone.ogg` (sha256; one table row per file in `Tests/Data/LICENSES.md`, with path and hash on one line). It joins the static checks as `test fixtures`.
- **Scaffold sources:** `Projects/FeatureTest/Scaffold/Sources/**` is a subset mirror of `Tests/Data/Assets/**`: each file equals its counterpart byte for byte, and the deliberately invalid fixtures (`HttpUri.gltf`, `DracoRequired.gltf`, `ParentEscape.gltf`, `Truncated.png`, …) are never mirrored. The GoldenScene copy test walks the Sources tree and checks each file against its counterpart. `Formats.jsonl` imports an explicit list from there into `Assets/Formats/<same relative path>`.
- **Approvals:** ADR 0001's row is updated with where each pin lives. FLAC is first-party and MP3 landed in M12.

### 17. Skills

`.claude/skills/<name>/SKILL.md`, each with `name` and a single-line `description` frontmatter (the `docs.get` parser):

- **add-component:** the files to touch:
  - the component header;
  - `BuiltinComponents.h`, through its owner;
  - the category registration, with flags, metadata and mandatory descriptions;
  - the consuming systems;
  - migrations with a format golden;
  - validation codes with a `Tests/Data/Validation` fixture.

  It also lists the generated references to refresh and the FeatureTest coverage for gates 1, 1b and 4. It notes that a new serialized field changes state hashes, which means re-recording replays and running `project.upgrade` on the demos.
- **golden-images:** the update policy and the review:
  - `--update-golden` candidates under `bin/TestResults`;
  - review through the diff;
  - record which goldens changed and why;
  - `Golden.jsonl` and its M9 byte check;
  - thresholds;
  - "passing comparisons are not design quality" (ADR 0020).
- **game-building:** the end-to-end MCP workflow and definition of done:
  - `Testing.Suites` setup;
  - the autopilot-and-record workflow and replay re-recording (§13.6);
  - the shared static body per level for tracks;
  - the provenance and transcript rules and the demo audit (§13.12).

  It stays under 48 KB, because §13.11 step 1 fetches it through `docs_get`.
- **game-debugging:** the observability loop (`log.read`, `events.read`, `script.errors`, `script.eval`, `scene.tree`, `scene.diff`, annotated screenshots, `physics.bodyInfo`, `entity.bounds`, `stats.get`, `audio.stats`, `play.state`). It ends with an error-code to fix table that covers the RPC codes (§13.3), `ErrorCode`, the script error kinds, the bridge errors and the validation codes from `GetCodeInfos`.
- **Updated skills:** build-and-test, commit-review, add-automation-method, add-script-api and luau-gameplay lose their "M14 adds" and "once it exists" text, and name the feature suite, the determinism suite, the references, the doc-example test and the concrete FeatureTest paths. luau-gameplay's example becomes checkable, and the skill shows how to type self-method calls.

### 18. Internal performance

`ScriptApiRegistry`'s per-dispatch counter lookup (a linear scan in test mode) is indexed. This is an internal change by stream A, with no header change, so heavy Debug suites stay within their callback budgets.

## Frozen interfaces

| File | Interface | Implementing stream |
|---|---|---|
| `Engine/Source/Engine/Testing/CoverageGates.h` (+ `.cpp` stubs) | `CoverageGate`, `PerModeCoverageGates`, `CoverageKind*`, `CoverageGateName`, `ParseCoverageGateName`, `IsPerModeCoverageGate`, `GetCoverageGateOfKind`, `ToTestModes`, `ToRunModes`, `EvaluateCoverageGates`, `MakeUnevaluatedCoverageGates`, `MakeScriptApiCoverageCounters`, `SceneCoverageRecorder`, `FormatCoverageRecorder` | A |
| `Engine/Source/Engine/Asset/SourceFormats.h` (+ `.cpp`) | `SourceFormat`, `GetSourceFormats`, `FindSourceFormat` | A |
| `Engine/Source/Engine/Asset/AssetManager.h` (+ `.cpp`) | `LoadObserver`, `SetLoadObserver`, protected `NotifyLoaded` | A |
| `Engine/Source/Engine/Testing/FeatureTestRunner.h` | `IFeatureTestHost::GetAssetManager()`; documented coverage behaviour, observer lifetime, evaluation errors and the `GetCoverage` baseline rule | A |
| `Engine/Source/Engine/Testing/TestResults.h` | Documented id and kind vocabulary, gate results and the JUnit coverage suite (no struct change) | A |
| `Engine/Source/Engine/Project/ProjectSettings.h` (+ registration) | `TestingSettings::CoverageGates` (implemented) | contract |
| `Editor/Source/EditorCore/EditorCommandLine.h` (+ `.cpp`) | `EditorLaunchOptions::TestJunitPath` (A parses it); `IdSeed` (implemented) | A / contract |
| `Engine/Source/Engine/App/ProjectSettingsApplication.h` (+ `.cpp`) | `ApplyGameSettings`, `ApplyTestSuiteOverrides` | D |
| `Editor/Source/EditorCore/Reference/ReferenceGenerator.h` (+ `.cpp`) | `ReferenceSources`, `GeneratedReferenceFile`, `GenerateReferences`, `WriteReferences`; `DumpReferences` (implemented with the M13 behaviour) | D |
| `Editor/Source/EditorCore/Reference/DescriptionInventory.h` (+ `.cpp`) | `DescriptionEntry`, `CollectDescriptions`, `FindMissingDescriptions` | D |
| `Editor/Source/EditorCore/Project/ProjectValidator.h` | `ValidationCodeInfo`, `ProjectValidator::GetCodeInfos` | D |
| `Editor/Source/EditorCore/Export/ExecutableResources.h` (+ `.cpp`) | `ExecutableResources`, `ResolveExecutableResources` | D |
| `Editor/Source/EditorCore/Play/EditorPlayController.h` (+ `.cpp`) | `EditorPlayController::CheckReloadTypes` | D |
| `Scripts/Lib/coverage.py` | `MODES`, `PER_MODE_GATES`, `MERGE_RULES`, `CoverageCounter`, `GateResult`, `FeatureReport`, `read_feature_report`, `evaluate_feature_report`, `merge_coverage`, `read_called_methods`, `uncovered_methods` | A |

Stub implementers added so the tree builds: `EditorFeatureTestHost::GetAssetManager` and `RuntimeApp::State::GetAssetManager` (stubs, A); the unit-test `RunnerHost` returns null.

## Data formats

- **FeatureTest report:** the TestRunResult JSON of `TestRunResultToJson` (decision 2 and 3 vocabulary): `mode`, `passed`, `cancelled`, `cases`, `suites`, and `coverage {registryFingerprint, counters[{id, kind, modes, count}], gates[{gate, evaluated, passed, missing}]}`, plus `jsonPath`, `junitPath` and `recordingPath`. Files: `FeatureTest-Editor.json` (the editor's default), `Feature-<Config>.json` (the feature suite with `--junit`), `FeatureTest-Release.json` and `FeatureTest-Dist.json`.
- **Seeded gaps:** `Tests/Data/CoverageGaps/` holds:
  - `Gate{1,1b,2,3,4,9}/{Editor,Release,Dist}/{FeatureTest-<Mode>.json, Expected.json}` (A);
  - `Gate5/{Methods.json, CalledMethods.txt, Expected.json}` (A);
  - `Gate6/Expected.json` (D);
  - `Gate7/{Descriptions.json, Expected.json}` (D);
  - `Gate8/{AllSettings.eproj, Expected.json}` (D);
  - `Merge/<Case>/{FeatureTest-{Editor,Release,Dist}.json, Expected.json}` (A), with the cases of decision 6.

  Every `Expected.json` has `"Format": "CoverageGap", "Version": 1`.
- **Validation fixtures:** decision 11. **Settings observation project:** decision 12.
- **Scaffold manifest, seeds and baseline:** decision 8.
- **Script error fixtures:** decision 15.
- **Doc-example fences:** decision 14.
- **Reference files and schemas:** decision 13.

## Ownership

Streams own disjoint files. A stream that needs a change in another stream's or the lead's file lists it in its result.

**Stream A:**
- `Engine/Source/Engine/Testing/**`
- `Engine/Source/Engine/Asset/SourceFormats.*`, `AssetManager.*` and `RuntimeAssetManager.cpp`
- `Engine/Source/Engine/AssetPipeline/EditorAssetManager.cpp`
- `Engine/Source/Engine/Scripting/ScriptApiRegistry.cpp` (decision 18 only)
- `Editor/Source/EditorCore/EditorCommandLine.*`
- `Editor/Source/EditorCore/Play/EditorFeatureTestHost.*`
- `Editor/Source/EditorCore/Automation/TestMethods.*`
- `Editor/Source/Editor/EditorApp.cpp` (the `--run-tests` reports; the `--id-seed` and `--dump-reference` call sites are final)
- `Runtime/Source/Runtime/RuntimeApp.cpp` (test paths and reports)
- `Scripts/Test.py`, `Scripts/CI.py`, `Scripts/PreCommit.py`, `Scripts/Scaffold.py`, `Scripts/Lib/coverage.py`
- `.github/workflows/ci.yml`
- `Tools/Automation/engine_client.py`
- `Tests/Automation/test_coverage_gates.py` and `test_script_modes.py`
- `Tests/Data/CoverageGaps/**`, except `Gate6/`, `Gate7/` and `Gate8/`
- the tests of these units: `Tests/Source/Engine/Testing/**`, `Tests/Source/Engine/Asset/{AssetManager,SourceFormats,RuntimeAssetManager}Tests.cpp`, `Tests/Source/Engine/AssetPipeline/EditorAssetManagerTests.cpp`, `Tests/Source/EditorCore/EditorCommandLineTests.cpp`, `Tests/Source/EditorCore/Automation/TestMethodsTests.cpp`, `Tests/Source/Editor/EditorAppTests.cpp`, `Tests/Source/Runtime/RuntimeAppTests.cpp` and `Tests/Source/Support/InMemoryAssetManager.*`

**Stream B:**
- `Projects/FeatureTest/Scaffold/{Components,Physics,Audio,Formats}.jsonl`
- `Projects/FeatureTest/Scaffold/Sources/**`
- `Projects/FeatureTest/Assets/{Formats/**, Tests/{Components,Physics,Audio,Formats}/**, Scenes/Components/**}` and their scenes in `Assets/Scenes/`
- `Tests/Data/Generate/{MakeFormatFixtures,MakeTextureFixtures}.py`
- `Tests/Data/Assets/{Environments,Fonts}/**` and `Tests/Data/Assets/Textures/Baseline.jpeg`
- `Scripts/FetchAssets.py`
- the `test fixtures` step in `Scripts/Lib/scripts.py`
- `Tests/Data/LICENSES.md`. D's fixtures are first-party and fall under its opening paragraph, so they need no section; D lists any row it does need in its result.
- `Tests/Source/Engine/AssetPipeline/FormatFixturesTests.cpp` and the importer tests it extends
- `Tests/Source/Support/GoldenSceneTests.cpp`

**Stream C:**
- `Projects/FeatureTest/Scaffold/{Scripting,Callbacks,Prefabs,SceneLoad,Input,Timing,Errors,QuitApp,Determinism,Replays}.jsonl`
- `Projects/FeatureTest/Assets/{Tests/{Api,Callbacks,Prefabs,SceneLoad,Input,Timing,Errors,QuitApp,Replays}/**, Scripts/**, Prefabs/**}` and their scenes in `Assets/Scenes/`
- `Engine/Source/Engine/Project/ProjectSettings.cpp` (the clock rule)
- `Tests/Source/Engine/Project/ProjectSettingsTests.cpp`
- `Tests/Source/EditorCore/FeatureTest/**`

**Stream D:**
- `Editor/Source/EditorCore/Reference/**`
- `Editor/Source/EditorCore/Project/ProjectValidator.*`
- `Editor/Source/EditorCore/Export/{ExecutableResources.*, Exporter.cpp}`
- `Editor/Source/EditorCore/Play/EditorPlayController.*` (decision 15 only)
- `Editor/Source/EditorCore/Automation/ObserveMethods.*`
- `Engine/Source/Engine/App/ProjectSettingsApplication.*`
- `Scripts/GenerateDocs.py`
- `Tests/Data/Generate/{MakeValidationFixtures,MakeScriptFixtures}.py`
- `Tests/Data/{Validation,SettingsObservation,Scripts/Errors,TypeChecker}/**` and `Tests/Data/Project/AllSettings.eproj`
- `Tests/Data/CoverageGaps/{Gate6,Gate7,Gate8}/**`
- `.claude/skills/**`
- `Resources/Templates/Projects/*/AGENTS.md`
- `Tests/Automation/test_editor_script_host.py`
- the tests of these units: `Tests/Source/EditorCore/{Reference,Scripting/DocExample*,Scripting/ScriptErrorFixture*,Project/**,Export/ExecutableResources*}`, `Tests/Source/Engine/App/ProjectSettingsApplicationTests.cpp`, `Tests/Source/Engine/Core/Json/AuthoredFilesTests.cpp`, `Tests/Source/EditorCore/Play/EditorPlayControllerTests.cpp` and the `Tests/Source/Support/` helpers `DefaultFields.*`, `DocExamples` and the validator test types
- the Luau fence info strings of any document

**Lead (shared integration files and the integration task):**
- the premake files and `Scripts/ModuleRules.json`
- `BuiltinComponents.h`, `RegisterBindings.cpp` and `RegisterMethods.cpp`
- `Docs/Reference/*`, `Tools/MCP/catalog.json` and `Resources/Scripting/Engine.d.luau`, regenerated once after merging (decision 13)
- `Docs/Architecture.md`, `Docs/Roadmap.md`, `AGENTS.md`, `Docs/ReviewChecklist.md`, `Docs/Decisions/**`
- `Projects/FeatureTest/{FeatureTest.eproj, AGENTS.md, Automation/Provenance.json}` and `Projects/FeatureTest/Scaffold/{Scaffolds.json, Settings.jsonl, Suites.jsonl, Baseline/**}`
- `Tests/Automation/test_feature_test.py` (unskipped at integration)
- call-site adoption in files another stream owns, after both streams merged: `ApplyGameSettings` in `RuntimeApp.cpp`'s factory, `ApplyTestSuiteOverrides` in `EditorFeatureTestHost.cpp` and `RuntimeApp.cpp`, and `CheckReloadTypes` in `EditorFeatureTestHost::ReloadScript`

## Approvals

No new approval is needed:

- the OTF, MP3 and FLAC format fixtures are approved in ADR 0001;
- the OTF comes from the already pinned and licensed Inter archive (decision 16);
- every download is pinned (sha256), checksummed and licensed in `Tests/Data/LICENSES.md`.

Nothing else needs the operator.

## Risks

1. **Golden tests and FeatureTest content.** Every golden render copies the whole project and fails on any scan diagnostic, and `GoldenSceneTests` refreshes the repository project. New assets must scan cleanly without a GPU or a checker provider. ShadowMapSize 1024 leaves the goldens unchanged (decision 9); the golden stage at integration confirms it.
2. **Hashes are brittle.** Replay hashes change with any id, scene, settings or simulation change. The replays are recorded last, in `Replays.jsonl`, after the final regeneration (decision 8).
3. **GPU dependence.** The feature stage needs a device and runs only in the local gate. The hosted runners do not run it; the Linux Lavapipe job may join in the ADR 0011 batch. The Timing and Determinism acceptance tests and the determinism stage run without a GPU, so replay hashes are checked on every machine.
4. **Frozen-interface changes.** `AssetManager.h` (M6), `IFeatureTestHost` (M13), `EditorCommandLine.h`, `EditorPlayController.h` and `ProjectSettings.h` change, and each change is reviewed here. The new `IFeatureTestHost` pure virtual requires every host to implement it.
5. **Validator coverage.** `COMPONENT_MISSING_REQUIREMENT` relies on test components, and `PATH_CASE_MISMATCH` relies on a case-insensitive file system or on how git checks out file names that differ only in case. Stream D documents both in the fixtures.
6. **Determinism of coverage.** No gate outcome may depend on wall time:
   - Format counts are distinct handles, and suites wait for the asynchronous loads they rely on (decision 5).
   - Scene counts come from fixed recording points of a deterministic simulation, and script counters count dispatches in simulation order.
   - The only wall-clock influence is the Errors suite's watchdog faults (`CallbackBudgetMs: 50`). Their spin loops must be pure Luau, with no counted API call inside the loop, so even the counts stay independent of how long a loop ran before the watchdog fired.
7. **Run time.** A full FeatureTest run in Debug includes per-dispatch counting (decision 18) and scene scans at session boundaries. If the Debug feature stage exceeds a few minutes, stream A profiles and reports to the lead before cutting coverage points.
8. **Scaffolds and `Test.ReloadScript`.** The scaffolds' `asset.import` of an `.hdr` needs a device, and `Test.ReloadScript` may rewrite a committed `.meta`. Stream C keeps reload targets byte-stable, and the feature suite's snapshot check (decision 7) catches a regression.

## Before milestone completion

- [x] Every `ENGINE_CONTRACT_STUB` and every skipped test of this contract is replaced; strict `PreCommit.py` and full `CI.py` (including the clang-cl portability builds) are green on Windows.
- [x] `Editor --headless --project Projects/FeatureTest --run-tests` exits 0 with all six editor-mode gates evaluated and passing, in Debug and Release (`Test.py --suite feature --require-gpu`), and leaves `Projects/FeatureTest` byte-identical outside `Library/`.
- [x] "FeatureTest: the Timing suite passes with 0, 1, 2 and 5 steps per frame" and "FeatureTest: Determinism.replay matches its committed hash in both editor configurations" pass in the Debug and Release unit suites, and `Test.py --suite determinism` is green over both editor configurations.
- [x] "CoverageGates: each seeded-gap fixture makes exactly its gate (and mode) fail and names the gap" passes for 18 fixtures; the gate 5, 6, 7 and 8 seeded-gap tests and the merge tests pass.
- [x] Gates 5 (automation suite), 6, 7 and 8 are green.
- [x] `GenerateDocs.py --check` is clean over the full reference set; "DocExamples: every Luau example in the docs type-checks" passes.
- [x] Every validation code has a fixture and `GetCodeInfos` documentation; `Clean` reports nothing.
- [x] The four new skills exist and the five existing skills are updated; AGENTS.md lists them.
- [x] `Scaffold.py --check` reproduces `Projects/FeatureTest` byte for byte (`test_feature_test.py`); `Provenance.json` covers every M14 file; `Projects/**` passes the authored-files test.
- [x] `Tests/Data/LICENSES.md`, `FetchAssets.py fixture --check` and ADR 0001's approvals row record every pinned fixture.
- [x] Architecture and Roadmap carry the amendments below; the review records which goldens and hashes changed and why.

## Architecture and Roadmap amendments

This contract amends the documents where a decision changes their text: §2.3 (Test.py's feature and determinism suites and `--merge-coverage`, `Scaffold.py`, GenerateDocs.py), §6.1, §11.7, §11.8, §11.9, §11.10, §12.1, §13.7, §15.5, §15.6 and §15.9 of Architecture, and Roadmap M14 and M15. §15.1 T3d and the §15.8 matrix already schedule the determinism stage over the editor configurations for M14 and need no change. The remaining wording, such as AGENTS.md's skill list and command table and the reference list in the commit gate, lands with the streams' results at integration.

## Contract review

The spec-lens and design-lens reviews were applied as follows. "Applied" means the contract, its stubs, its skipped tests or this ADR changed as described.

| Finding | Disposition |
|---|---|
| Spec MAJOR 1: determinism stage pushed to M15 without amending §15.1 and §15.8 | Applied the Architecture's schedule instead of amending it: decision 7 adds the determinism stage over the Debug and Release editors in M14 (stream A); M15 adds the exported runs (Roadmap M14 and M15 amended). |
| Spec 2: the `EditorOnly` fixture rule contradicts the mode scheme | Applied: the rule is limited to gates 1 (a field), 2, 3 and 4, and `EvaluateCoverageGates` rejects a component, enum-value or format counter not in all three modes and any counter without the Editor (decisions 2 and 6; test extended). |
| Spec 3: merge rules leave skipped cases open and rule names unfrozen | Applied: eight frozen rules with subjects and ordering, `skipped` among them; the cases are frozen (decision 6). |
| Spec 4: C++ and Python checks of a fixture can disagree | Applied: the invariant is in decision 6, and the C++ acceptance test asserts it. |
| Spec 5 / design m1: observer doc contradicts the recorder; observer lifetime | Applied: const queries allowed, no lock or iteration around `NotifyLoaded`, removal on every end of a run including the destructor and a failed `Begin` (`AssetManager.h`, `FeatureTestRunner.h`, decision 5). |
| Spec 6: no failing-by-design tests for part of the surface | Applied: skipped tests for `FormatCoverageRecorder`, both managers' notifications, the editor host's asset manager (through a coverage run), the Runtime report and observer (Python), `WriteReferences`, the `--run-tests` reports and the `--merge-coverage` CLI; `--id-seed` is implemented with a two-run identity test. `EditorAppTests.cpp` and `RuntimeAppTests.cpp` now belong to A. |
| Spec 7 / design M1: scaffold reproducibility cannot be checked | Applied: committed `Scaffold/Baseline/`, frozen seeds, `project.refreshAssets` at the end of each scaffold (found while testing `--id-seed`), `Scaffold.py --check` (A) and the lead's `test_feature_test.py` (decision 8). |
| Spec 8: the Sources mirror is ambiguous | Applied: a subset mirror with an explicit import list (decision 16). |
| Spec 9: the observation fixture project is unspecified | Applied: `Tests/Data/SettingsObservation/Project/`, generated by `MakeValidationFixtures.py`, owned by D; the skeleton opens it (decision 12). |
| Spec 10: two gate 8 consumers are copies; `PhysicsWorld` | Applied (decision 12). |
| Spec 11 / design m2: doc amendments incomplete | Applied: §2.3, §6.1, §11.8 and §11.10 amended; `FeatureTest.eproj` made canonical through a batch run. |
| Spec 12: no Python test for the `type` kind | Applied: `test_reload_refused_for_type_errors_reports_a_type_error` (D). |
| Spec 13 / design M4: gate accountability overlaps; `PostProcess` is unique per scene | Applied: per-counter accountability (prefab and script ids and the native `.prefab`, `.luau`, `.replay` formats to C), variant scenes for B, B never references C's assets (decision 9). |
| Spec 14 / design m4: gate 7 in a Dist module and implemented twice | Applied: moved to `EditorCore/Reference/DescriptionInventory.h` under D, reused by `GenerateReferences`. |
| Spec 15: Risk 3 contradicts `ci.yml`, which has no owner | Applied: Risk 3 corrected; `ci.yml` belongs to A; the determinism stage joins the hosted Windows job (decision 7). |
| Spec 16: the gate 1b narrowing loses asset-enum coverage | Accepted and recorded with the tests that cover those values (decision 4); §15.6 says so. |
| Spec 17: field id format | Applied (decision 2, `CoverageGates.h`). |
| Spec 18 / design nit: proxy counters and `RequiresRead`/`RequiresWrite` | Applied (decision 2, `CoverageGates.h`). |
| Spec 19 / design nit: `SceneCoverageRecorder` has no registry at `Begin` | Applied: created at the first recorded scene from `Scene::GetTypeRegistry()` (decision 4). |
| Spec 20 / design nit: the Timing filter is a substring | Applied: `Timing/`, the suite name is checked, and a naming rule is added (decision 9). |
| Spec 21: reports are always named `FeatureTest-<Mode>` | Rejected: the name identifies the runner and mode, not the project, and M15's export stage relies on fixed names. The cwd hazard behind it is fixed by the test-hygiene rule (decision 7). |
| Spec 22: `NonCanonical`, the block count's home, the OTF pin | Applied: `NonCanonical` in the example, `MinimumCheckedBlocks` in `DocExampleTests.cpp`, and the OTF member, size, SHA-256 and CFF kind verified against the pinned archive (decisions 11, 14 and 16). |
| Spec 23: what the feature suite's `--junit` writes | Applied: `bin/TestResults/Feature-<Config>.xml` and `.json`, otherwise a temporary directory (decision 7). |
| Design M2: streams cannot verify through `EditorApp.cpp` and the references | Applied: `EditorApp.cpp` belongs to A; `--dump-reference` goes through D's `DumpReferences`; `--id-seed` is implemented; streams regenerate the reference locally (decisions 8 and 13). |
| Design M3: ShadowMapSize 1024 changes the GltfFixture golden | Rejected with evidence: GltfFixture's directional light has `CastShadows: false` (its `true` entries are mesh renderers), no M8 golden scene has a shadow-casting light, and every golden with one renders through `RendererIIGoldenFixture`, which pins 1024. The golden stage at integration confirms it (decision 9). |
| Design m3: Risk 6 is false (per-frame loads, async timing, watchdog spins) | Applied: distinct-handle counts, waiting for relied-on async loads, pure-Luau spin loops (decision 5, Risk 6). |
| Design m5: ownership gaps | Applied: `AllSettings.eproj`, `EditorAppTests.cpp`, `RuntimeAppTests.cpp`, `LICENSES.md` and the shared walk (`Support/DefaultFields.*`, created by the contract) have owners. |
| Design m6: decision 15 split across streams; §11.8 | Applied: one owner and one gate, `CheckReloadTypes`; the lead adopts the call in `EditorFeatureTestHost`; §11.8 amended (decision 15). |
| Design m7: GPU-less acceptance runs constrain C's scenes | Applied: the rule for replay scenes and `Timing.scene` (decision 9). |
| Design m8: gate 2 relies on undocumented behaviour | Applied: §11.10 documents it, and an implemented regression test pins it (decision 4). |
| Design m9: default report location | Partly applied: the location stays, and every test passes `--junit` into its temporary directory or runs in one (decision 7). |
| Design nits: element defaults, `Formats/` exclusion, evaluation errors, Editor superset | Applied (decisions 2, 3 and 4; `ReferenceGeneratorTests.cpp`). |

## Integration

The lead merged the four streams' staged results on the contract commit `3f5a3e7` without conflicts, adopted the call sites, fixed the engine defects the streams found, wrote the lead-owned FeatureTest files, regenerated the project and the references, and ran the strict gate. This section records every integration decision; where it changes documented behaviour, Architecture carries the amendment.

### Engine fixes

1. **CFF glyphs (stream B).** stb_truetype's distance-field solver (`stbtt_GetGlyphSDF`) handles only lines and quadratic curves and skips the cubic segments of CFF outlines, so every curved `.otf` glyph imported garbled ("Inter OTF" rendered as "Int_r TF"). The vendored header stays untouched; the first-party `AssetPipeline/ThirdParty/TrueTypeRasterizer.cpp` now computes the field itself:
   - `ReplaceCubics` replaces each cubic by `n` equal-parameter quadratic pieces, `n` the smallest count whose approximation error bound `sqrt(3) / 36 * |P3 - 3 P2 + 3 P1 - P0| / n^3` is at most a quarter of a font unit. Each piece's control point is its best single quadratic, and the points are rounded to whole font units like every stb_truetype vertex, so the solver's integer-vertex assumptions hold. Only arithmetic is used, so the field is the same on every CPU and configuration.
   - `ComputeGlyphSdf` is the body of `stbtt_GetGlyphSDF` with the vertices passed in, in the same arithmetic order, calling stb_truetype's own `stbtt__compute_crossings_x` and `stbtt__solve_cubic`. A temporary comparison during integration confirmed that every TrueType glyph the unit suite imports (Inter's ASCII and Latin-1, `Glyphs.ttf`) gives a field byte-identical to `stbtt_GetGlyphSDF`'s, so no golden image changes.
   - `FontImporter::Version` is 2, so cached version 1 atlases of `.otf` fonts are rebuilt (§7.4: importers are pure functions of source, settings and version). FeatureTest's two font `.meta` files record the new version.
   - Regression test: "FontImporter: the OTF's cubic outlines rasterize like the TrueType outlines of the same design" samples every printable ASCII glyph of the pinned Inter OTF and of the engine's Inter TTF at the same em-space points (bilinearly): at most 4 levels apart per glyph (measured: at most 1, mean 0.04), and the ring of `O` inside and its counter outside. Without the fix, dozens of glyphs differ by far more.
2. **Host time in the script watchdog (streams B and C).** `Test.Screenshot` (rendering and encoding the capture) and `Test.ReloadScript` (the reimport with its type check) block inside a case's resume, and that wall time counted toward `CallbackBudgetMs`, so suites timed out on a loaded machine.
   - `ScriptWatchdog::BeginHostTime` and `EndHostTime` bracket host work that runs no Luau: the deadline moves later by its duration, and inside the period it moves with the clock. A protected entry inside the period (Luau the host work ran after all) ends the excluded time there, so Luau time always counts; a timeout latched before the host work stays latched.
   - `ScriptEngine::RunHostOperation(operation)` is the public form (a reviewed addition to the M13 header `ScriptEngine.h`; internally `SandboxAccess::BeginHostTime` and `EndHostTime`). The `Test.Screenshot` binding runs the host's capture through it, in every mode. `EditorFeatureTestHost::ReloadScript` runs the reimport and the type gate through it; the reload itself runs the new code and counts.
   - The budget overrides that existed only for this are gone from `Suites.jsonl`: stream B's `CallbackBudgetMs: 10000` (Components, Physics, Formats) and stream C's `5000` (Api.EditorOnly, Callbacks.HotReload). The Errors suite keeps decision 9's `{CallbackBudgetMs: 50, MemoryLimitMB: 16}`.
   - Tests: "ScriptWatchdog: host time moves the deadline while Luau time and latched timeouts still count" (exact samples) and "TestBindings: slow host work inside a case stays out of the callback budget while a Luau spin still times out" (a capture and a reload that each take 10 s of the safety clock under a 500 ms budget pass; a pure-Luau spin still times out). The second fails without the fix.
3. **Wall-clock type-check limit (stream B).** `ScriptTypeChecker` set Luau's `moduleTimeLimitSec = 1.0`, so a long module on a loaded machine reported "script analysis exceeded its safety limit" as a `SCRIPT_TYPE_ERROR` and blocked test discovery. Analysis now has no wall-clock limit: Luau's own complexity limits, which count work rather than time, bound every check and report what they cut short as a located type error. Only a time limit or a cancellation token makes Luau report a timeout and the checker sets neither, so `timeoutHits` is asserted empty. §11.9 says so. The review found the one exception to "the complexity limits bound every check", user-defined type functions, which the checker now refuses before analysis (review fix M23 below). Test: "ScriptTypeChecker: findings do not depend on how long the analysis takes" (a 600-function strict module whose analysis takes about 3.5 s in Debug reports exactly its one real finding, twice); it fails with the old limit.
4. **Build configuration in replay headers (stream C).** A replay header recorded the recording editor's build configuration (`Config: "Debug"` or `"Release"`), so a Debug and a Release regeneration of FeatureTest could not be byte-identical, while PreCommit checks the scaffolds with the Debug editor and CI with Release. `Config` now records the run mode of the recording process: `"Editor"` for the editor, `"Release"` or `"Dist"` for the Runtime (`"Release"` in its Debug and Release builds). The format keeps version 1, every reader still accepts any string (the field stays informational), and committed replays depend on no build configuration (§9.1). §6, `ReplayData.h`, the Replay schema's description and the FileFormats example follow. Tests: the editor's recording in "TestMethods: a recorded suite writes a replay that verifies without its test body" and the exported Runtime's in `test_runtime_scripts.py`.
   > **Superseded in part** by `Docs/Decisions/0026-m16-contract.md` ("Integration (session 2 fixes)", friction S2-01, F21): the Replay format is version 2 (`ReplayFormatVersion`), which adds the required `Checkpoints` array (`{Tick, StateHash}`, one every `ReplayCheckpointInterval` = 60 completed ticks before `FinalTick` in a recording; any strictly increasing ticks between 0 and `FinalTick` for a reader), and the cooked replay is `ReplayData::FormatVersion` 2 with `ReplayImporter::Version` 2, so version 1 artifacts are rebuilt. A version 1 source still reads, as version 2 without checkpoints (its `FinalStateHash` alone is compared), and `project.upgrade` rewrites it. `Config` keeps this decision's meaning.
5. **Wall-clock import budget (found at integration).** `Scaffold.py --check --config Debug` logged `ASSET_IMPORT_FAILED Assets/Scripts/Api/Probe.luau: Timeout: script exceeded 250 ms in script.<module top>` for a 1.3 KB module while the editor recorded a replay with four Jolt workers busy: the load-time VM's 250 ms budget (§11.2) was wall time, so an import could fail because other threads kept the machine busy or the import thread waited (in Debug, on the CRT's heap lock). That output still matched, but a timed-out import of a replayed behaviour would change a recorded hash, and it breaks importer purity (§7.4). The new `Platform/ThreadClock.h` (`GetThreadTimes` on Windows, `CLOCK_THREAD_CPUTIME_ID` on Linux and macOS) gives the calling thread's CPU time, and `ScriptImporter` passes it as the load-time VM's safety clock (`LoadTimeVmSpecification::ClockSeconds`). The budget and its messages are unchanged; a runaway module top still times out, since it spends the import thread's CPU time. The runtime watchdog stays on `lua_clock` (§11.1: it bounds hitches the player sees). Test: "ThreadClock: a thread's CPU clock advances with its own work and not while it waits".

### Call-site adoptions

- `EditorFeatureTestHost::ReloadScript`: reimport, then `EditorPlayController::CheckReloadTypes`, both through `RunHostOperation`, then `ScriptEngine::Reload`. A refused reload fails `Test.ReloadScript` in the case with the gate's located Script error. Test: "TestMethods: Test.ReloadScript refuses new source with type errors and keeps the running code" (the new source arrives on disk while the case waits; `pcall` sees the refusal, the running code is kept, one `type` ScriptError with `Callback: "Reload"` at the finding is published, and the unprotected call fails the case with the located error).
- `ApplyTestSuiteOverrides` in `EditorFeatureTestHost::CreateSuiteSession` and `RuntimeApp::State::CreateSuiteSession` replaces the two identical copies. Test: "TestMethods: a suite's overrides set the budget and memory limit of its session" (the live session VM runs with the overridden budget and soft memory limit, and the project's own values are untouched). The Runtime calls the same function, which stream D's unit tests cover; the exported runs arrive with M15.
- `ApplyGameSettings` in `CreateRuntimeApp` replaces the factory's inline mapping. The Runtime process tests observe the result: the log and crash folders follow the manifest's `Name` ("a pak that does not match Game.json exits 3 naming it, logging under the manifest's name") and the headless game view follows its window size ("--screenshot-at writes the game view at the window's size"); stream D's unit tests cover every field of the function.

### Lead-owned FeatureTest files

- `Scaffold/Settings.jsonl` (seed 1501): one `project.setSettings` patch with decision 9's values: `StartScene` `Assets/Scenes/Components.scene`; window `FeatureTest` 1280 x 720; `FixedHz` 64, `MaxStepsPerFrame` 5, `Seed` 1414, `MaxEntities` 8192; gravity `[0, -10, 0]`, the six layers with `Default` first and the twelve collision pairs; stream C's input actions (`Fire`: `Mouse.Left`, `Key.F`, `Gamepad.RightBumper`; `Jump`: `Key.Space`, `Gamepad.South`; `MoveX`: `D`/`Right` against `A`/`Left` and `Gamepad.LeftX`; `MoveY`: `W`/`Up` against `S`/`Down` and `Gamepad.LeftY`, inverted); `ShadowMapSize` 1024; scripting 128 MB, 500 ms, `PauseOnError` false, `BlockPlayOnTypeErrors` true; `Testing.CoverageGates` true and `Testing.Replays` `["Assets/Tests/Replays/*.replay"]`.
- `Scaffold/Suites.jsonl` (seed 1515): the whole `Testing.Suites` list and the `Replays` glob in one patch: stream B's Components, Physics, Audio and Formats, then stream C's 37 `Api.<Type>` suites, `Api.EditorOnly` (`Modes: ["Editor"]`), Callbacks, `Callbacks.HotReload` (`Modes: ["Editor"]`), Prefabs, SceneLoad (`Parameters`), Input, Timing (`Clock`), Errors (`Isolation: "Case"` and its overrides) and QuitApp (`ExpectQuit: 3`): 50 suites.
- `Scaffold/Scaffolds.json`: decision 8's sixteen files, seeds and order unchanged; only `Formats.jsonl` uses `vulkan`. No stream asked for a different order or renderer.
- `Golden.jsonl` and `Scaffold/Baseline/` are untouched. `python Scripts/Scaffold.py --config Release` regenerated the project once; the streams' staged scaffold outputs were byte-identical to the regeneration except the two font `.meta` files (importer version 2) and the two replays (`Config`). `Automation/Provenance.json` and `FeatureTest.eproj` are the regeneration's. `Projects/FeatureTest/AGENTS.md` takes the templates' `docs.get` wording (stream D) and describes the scaffolds.
- Replays and their committed hashes: `Assets/Tests/Replays/Determinism.replay` `FinalStateHash` `c6c208436a8c52db` (600 ticks) and `Input.replay` `ff0e3f70e3200f86` (40 ticks), recorded last by `Replays.jsonl`. They equal the hashes stream C recorded with the same settings; only `Config` changed (fix 4). Review fix M07 re-recorded `Determinism.replay` as `2a75dca847e98e59` (a CRT `pow` term in its scripted forces); `Input.replay` is unchanged.

### Contract changes the streams reported, accepted

- **A:** `EditorFeatureTestHost::CaptureCoverageBaseline` (not frozen), called by `TestMethods` right before `Begin`; `EvaluateCoverageGates` also refuses a mode listed twice and an id that is only its kind's prefix; `coverage.merge_coverage` reads its reports by position in `MODES` order, and a failed `report` rule stops the other rules; a completed coverage run drops counters that do not exist in its mode; JUnit coverage cases carry `classname="FeatureTest.coverage"` and the totals include the gates; `--run-tests` logs a summary instead of the whole result JSON; `Scaffold.py` also deletes `Library/` in place and never compares the gitignored `*.bak` backups; the feature suite adds a temporary `--user-data-dir`; `InMemoryAssetManager::Load` notifies the load observer like the production managers.
- **B:** the FormatFixtures acceptance test also checks the fixture set against `RegisterBuiltinImporters` (and, since A landed, against `GetSourceFormats`); `Physics.scene` carries no Script component (the suite attaches `ContactProbe.luau` at run time, because `Export.Exclude` leaves out `Assets/Tests/**`); `Blip.sfx` lives in `Assets/Formats/Audio/`; the budget overrides B asked for were removed by fix 2.
- **C:** the `TestSuite.Clock` description states the clock rule, and the renamed test "ProjectSettings: test suites need non-negative frame deltas with one positive, unique modes, object parameters and a budget of 0 or at least 10" follows it.
- **D:** the structural validation fixtures are written from the `Clean` baseline with the same defects instead of byte-copying `Tests/Data/Scenes/Invalid/*.scene`, which would report incidental codes; `ProjectValidator` computes entity paths lazily (an O(n^2) cost made `PHYSICS_LIMIT_EXCEEDED` take about 1,350 s); `FileFormats.md`'s "Importable" column comes from `FindSourceFormat`; the authored-files walk of `Projects/**` also skips each project's `Library/` and `Build/`; DocExampleTests gains "DocExamples: every skill document has the docs.get frontmatter and fits inline" (48 KB).

### Accepted additional files

- `Tests/Automation/test_fetch_assets.py` (stream B, offline tests of `FetchAssets.py fixture`) is stream B's.
- `Tests/Source/Support/DocExamples.{h,cpp}` and `ValidatorTestTypes.{h,cpp}` (stream D).
- `Engine/Source/Engine/Platform/ThreadClock.h`, `Platform/Windows/ThreadClockWindows.cpp`, `Platform/Posix/ThreadClockPosix.cpp` and `Tests/Source/Engine/Platform/ThreadClockTests.cpp` (lead, fix 5).
- `Tests/Data/Generate/MakeCoverageGapFixtures.py` and `Projects/FeatureTest/Assets/Audio/Beep.sfx` (review fixes M05 and M08).

### Other integration changes

- The descriptions of `Time.GetDeltaTime` (the fixed phase returns the fixed step; the time scale changes the step count, not the step) and of the `TestExtraction` alias (`Frame` counts the completed extractions) now say what the engine does, as stream C observed.
- The `test fixtures` static step (`FetchAssets.py fixture --check`) is named in AGENTS.md's commit gate, in the build-and-test and commit-review skills and in the PreCommit.py and CI.py step lists. ADR 0001's format-fixture row says where the pins live.
- AGENTS.md lists the nine skills and gains rows for the feature and determinism suites, `--merge-coverage` and `Scaffold.py`.
- `Tests/Automation/test_feature_test.py` is unskipped; no `ENGINE_CONTRACT_STUB` and no `doctest::skip` outside `Test::ChildTargetSuite` remain.
- "EditorPlayController: a reload refused for type errors keeps the running code and reports a type error" (stream D) failed in the Release unit stage of `CI.py`: it waited for a `test.run` to be between two ticks, but `test.run` advances as many runner steps per poll as fit its wall-clock frame budget, and the Release build finished the whole run within one poll. The test now gives its server a stepping clock (one runner step per poll), as "TestMethods: disconnect restores the suspended session and edit selection without another poll" already did, so it observes the run however fast the build is. The two new TestMethods tests use the same clock.
- `Docs/Reference/**`, `Tools/MCP/catalog.json` and `Resources/Scripting/Engine.d.luau` were regenerated once after merging (`GenerateDocs.py --config Debug`).

### Review fixes

The milestone review (24 verified findings, M01 to M28 with gaps) was fixed before the milestone commit. Each behaviour fix has a regression test; "fails without the fix" below was established by building the tree with the fix reverted (or, for content and Python, by running the old file) and running the test.

- **M23 (major): a looping user-defined type function hung every check.** Fix 3 left the checker without a time limit or a cancellation token, and Luau 0.741 evaluates `type function` bodies as Luau in its analysis VM, whose only interrupt checks exactly those two (`UserDefinedTypeFunction.cpp`), so `type function Spin(t) while true do end return t end` never returned from `ScriptImporter`'s check, `script.check`, `project.validate`, `--check-scripts` or test discovery. Design: the checker refuses user-defined type functions before analysis. `ScriptTypeChecker::CheckScript` parses the root and returns one located `SCRIPT_TYPE_ERROR` per `type function`/`export type function` declaration (at its name), plus the root's syntax errors, without running the frontend; `CheckerFileResolver::readSource` does the same for each required module and hands the frontend no source for it, so no type function reaches the type-function VM in any module. Findings stay independent of machine load: no time limit, token or backstop was added, because a time-based backstop would make findings depend on load again and nothing else in analysis runs user code (the complexity limits, which count work, bound the rest). No engine script or document uses type functions; the luau-gameplay skill says they are refused. §11.9, the `ScriptTypeChecker.h` contract and the code comment say this; fix 3's statement that the complexity limits bound every check is corrected by it. Test: "ScriptTypeChecker: a user-defined type function is refused before analysis, so a looping one cannot hang a check" (a looping root, a raising body that is never run, an exported looping type function in a required module, and a plain alias that stays clean; `doctest::timeout(120)`). Without the fix the case hangs until the watchdog stops the run at 120 s (exit 5).
- **M07 (major): `Determinism.replay` did not reach the CRT `pow`.** `Pusher.luau` (Determinism.jsonl) now also scales each target's force by `(1 + (index % 3) * 0.29) ^ self.Exponent`; the exponent is the authored field (1.7 in Determinism.scene), so the compiler cannot fold it and Luau's `^` calls the C runtime's `pow` (only 2, 3 and 0.5 are special-cased), next to the existing `math.pow` (DetMath) and DetMath trigonometry terms. Replays.jsonl re-recorded the replay in the regeneration: `Determinism.replay` `FinalStateHash` `c6c208436a8c52db` becomes `2a75dca847e98e59` (reason: the new force term changes the simulation; its expectations still hold), `Input.replay` stays `ff0e3f70e3200f86`. The determinism stage gives `2a75dca847e98e59` in the Debug editor (debug CRT, `ucrtbased`) and the Release editor (`ucrtbase`), so Architecture §4.12's statement that the cross-configuration determinism stage covers the CRT `pow` path, §15.5's replay contents and decision 9's table are true; M15 adds the exported Release and Dist runs.
- **M01: `--run-tests` and `--feature-test` wrote no report when a run failed before its result existed.** The new `MakeFailedTestRunResult(mode, message)` (`Testing/TestResults.h`, a reviewed addition to the contract header) is a failed result with one `error` row (suite `FeatureTest`, case `run`) naming the error, no suites and no coverage. When `test.run` fails, `EditorApp` writes the reports from the result `test.run` published with its error (`error.data.testRun`, when its cases ran) or else from `MakeFailedTestRunResult` (a preflight error such as type errors, a refused admission, a failed Begin); `RuntimeApp` writes `FeatureTest-<Mode>.{json,xml}` from it when the run cannot begin. A stale report therefore never survives at the report paths, and `EditorCommandLine.h`'s "written whatever the outcome" holds. Tests: "EditorApp: --run-tests writes failed reports over stale ones when the run fails before a result exists" and "RuntimeApp: feature-test writes failed reports over stale ones when the run cannot begin" (`TestGameSpecification::TestSuiteScripts` gives the test game a suite whose script the pak lacks); both fail without the fix (the stale files stay).
- **M03: editor gate 9 counted viewport loads.** The runner's observer now records only while `FeatureTestRunner::Advance` runs (`CountingLoads`), so a load in a host frame between two calls (the editor's viewports rendering the published session, thumbnails) never counts, whatever the wall-clock poll budget; every session path loads synchronously inside Advance (setup, ticks, frames, `Test.Screenshot`, reloads, scene replacement), and LoadAsync publications, which happen at the host's main-thread drain, have no session caller. `FeatureTestRunner.h`, `AssetManager.h`, decision 5's rule and §15.6 say so. `AssetManager::HasLoadObserver()` (a reviewed addition to the M6 header) lets the lifetime test check installation, since an installed observer may now ignore a load. Tests: "TestRunner: gate 9 counts the loads of the run's own work and never a load between two Advance calls" (a "viewport" load before every Advance call is not counted, the suite's own load is) and the lifetime test's new check that a load between calls is not recorded; both fail without the fix.
- **M04: `Scaffold.py` deleted `Assets/` before it met an open editor's lock.** `project_lock_held` probes `Library/Editor.lock` like `ProjectLock::IsHeld` (a non-blocking lock of the lock's byte at `0x7FFFFFFFFFFFFFFE` on Windows, a shared non-blocking `flock` on Linux and macOS) and regeneration in place fails before removing anything while it is held. Test: `test_scaffold_refuses_a_project_an_editor_holds_before_removing_anything` (`test_feature_test.py`, a running editor holds a temporary project); the old `Scaffold.py` deleted the project's scene in the same setup before failing on `Library/`.
- **M08: `Scripting.scene` referenced an asset `Export.Exclude` leaves out.** `Beep.sfx` moved from `Assets/Tests/Api/` to `Assets/Audio/Beep.sfx` (Scripting.jsonl, its suites follow); its handle stays `57e9754aa8546eed`, because handles follow the seeded sequence, so `Scripting.scene` is byte-identical. Decision 9's placement rule gains: an asset a scene references lives outside `Assets/Tests/` (`Assets/Audio/` for scene-referenced sounds), since a plain export excludes `Assets/Tests/**`. Test: "FeatureTest: no asset a plain export includes references one that Export.Exclude leaves out" (every string of every included scene, prefab, material and sound effect against the handles of the excluded `.meta` files); on the unfixed project it names the two references of the review.
- **M13: `ASSET_TYPE_MISMATCH` was documented as not auto-fixable.** `GetCodeInfos` sets AutoFixable and the Fix text names the `.meta` rewrite; the game-debugging row and `Validation.md` follow. The fixture gains `Assets/Materials/Typed.material` with a `.meta` of type Texture, so the scan reports a fixable `ASSET_TYPE_MISMATCH`; loading it fails as a consequence, so the fixture's Codes are `ASSET_IMPORT_FAILED` and `ASSET_TYPE_MISMATCH`. "Validation: each fixture reports exactly its expected codes" now also checks that every code a fixture reports as `autoFixable` is documented AutoFixable; it fails without the fix.
- **M02: `--merge-coverage` exited 2 for a missing or unreadable report.** Such a report is a mode without a report: `merge: report: <mode>` for its position and exit 1; exit 2 stays argparse's. Test: `test_merge_coverage_command_reports_a_missing_or_unreadable_report_as_its_mode` (a missing Dist and a truncated Release report); the old CLI exits 2 without failure lines.
- **M05: the seeded-gap fixtures had no committed generator.** `Tests/Data/Generate/MakeCoverageGapFixtures.py` (standard library, `--check`, `--json`) regenerates every file of `Tests/Data/CoverageGaps/` except `Gate6/`, `Gate7/` and `Gate8/` byte for byte and owns that set (it reports extra files). Each report's gates are computed from its counters by a port of `EvaluateCoverageGates`, so the invariant of decision 6 holds by construction, and each per-gate `Expected.json` comes from that evaluation; the one deliberate exception is the `Merge/Unevaluated` Editor report, whose gates are the unevaluated set. The static checks run it like every generator.
- **M10: the Callbacks suite did not pin the trigger-enter tick.** `Journal` records the tick at which each entry first arrives; "a body that starts inside a trigger enters it at the first step" now asserts that both sides' enters arrive at the first fixed step, tick 0. The new case "deactivating a body inside a trigger synthesizes the zone's exit" deactivates a body a script creates inside the zone and asserts OnDisable, its teardown, then the zone's synthesized `OnTriggerExit` and no exit of its own, so the suite covers synthesized exits on destroy and disable for both collisions and triggers.
- **M11: FeatureTest's AGENTS.md** no longer promises a `BuildLog.jsonl` (the scaffolds run through `Editor --batch`, not the bridge that appends it) and names the provenance gap. **Provenance of the 24 M9 golden files:** no entry is added. The format records the request that wrote a file in this project (`Method`, `RequestId` and `Client` of the editor session that wrote it). These files were not written by an editor run of FeatureTest: commit `c95862a` added them together with Golden.jsonl's last 30 lines and without a `Provenance.json` change, and their bytes are those that "RendererIIGolden: scaffold reproduces the pinned scenes materials and metadata" produces by replaying those 30 requests as a batch of their own from `EditorTestIdState` in a test project (it pins them byte for byte). An entry naming Golden.jsonl's request numbers 81 to 110 would claim a whole-file run that never happened (and could not reproduce their ids: the M8 part of Golden.jsonl is unseeded), and request ids 1 to 30 would name a test fixture's session. The gap stays recorded here and in AGENTS.md; closing it needs a reviewed regeneration of the golden content through a recorded editor run before a provenance audit is pointed at FeatureTest (AuditProvenance.py, M16, covers the demo projects).
- **M12:** the Api.Test screenshot case calls `Test.Screenshot` without tolerating `Unsupported`: every FeatureTest run renders (decision 7), so a host that refuses the capture fails the case. Under `--renderer none` the case now fails with the refusal, where it passed before.
- **M16:** `Seeded.md` has three findings: one in the Behaviour-prelude example (document line 7, through the fence and prelude offsets), one in the example without a prelude (line 16) and one that surfaces in the prelude's epilogue (`Preludes/Behaviour.luau:15:8`); `Expected.json` findings carry `Elsewhere`. With `preludeLines` miscounted by one for prelude blocks, the test fails (the old document's only finding came from a block without a prelude, so it could not notice).
- **M21:** the skill size check measures what the dispatcher measures, the minified JSON of the serialized `docs.get` result (escapes, topic and path included), against `DefaultOffloadThresholdBytes`; a synthetic text below the threshold in raw bytes but above it serialized pins the measure.
- **M22 and M26:** the settings-observation replay records `Config: "Editor"`; "AuthoredFiles: every committed replay records a run mode as its Config" checks every committed replay (fixtures and projects) and fails on the old fixture.
- **M17:** decision 17's "`--update-golden` candidates under `bin/TestResults`" is wrong: `--update-golden` writes each candidate into `Tests/Golden/<DeviceClass>/`, where it is reviewed in the git diff, as `GoldenImage.h`, AGENTS.md and the golden-images skill say (`bin/TestResults` receives the actual, expected and diff images of mismatches).
- **M06, M15, M19, M20, M27, M28 (text):** the HotReload suite's comment says that a reload's reimport and type check are host time outside the callback budget (Callbacks.jsonl); the build-and-test skill lists the determinism stage in the hosted Windows job and says that `Scaffold.py --check` names each file that differs, is missing or is extra; `--dump-reference`'s help text and `EditorCommandLine.h`/`ReferenceGenerator.h` comments describe the whole reference set; the game-debugging skill lists the real debug views (no depth view); `Watchdog.h` says runtime VMs sample `lua_clock` and load-time VMs the import thread's CPU clock; `Tests/Data/LICENSES.md` names the third-party copies under `Projects/FeatureTest/Assets/Formats/` (the OTF, MP3 and Ogg fixtures) and their licenses.
- **Found by the gate during the review fixes:** "ProjectSettings: every AllSettings.eproj value is observed by its consumer" waited for its suite's `test.run` (more ticks than the default suite tick limit, about 25 s in Debug) with `Test::DefaultWaitBound` (30 s), so a loaded machine failed it in the strict gate's unit stage. Its waits for the run and the export now use a failure bound of 480 s, below the case's 600 s timeout, so only a hang reaches it (ADR 0008 decision 15).
- **M25:** the Python lines M14 added over CodeStyle's 120 columns are rewrapped: 121 added lines in 14 scripts, tests and generators (pure formatting: the generators' `--check` shows identical output, and docstrings aside each file's syntax tree differs from its staged version only by the review fixes above), plus 5 older long lines of the same files.

### Results

On Windows (nvidia-61x), on the integrated tree:

- `python Scripts/PreCommit.py` (strict): passed. Unit Debug 3,321 test cases, 31 skipped, all child-process targets; gpu Debug 252 under each API cap; golden Debug 25 of 25 matched; feature Debug passed with the six editor-mode gates evaluated and green; automation Debug 261 tests, method coverage 86/86 (`test_feature_test.py`'s `Scaffold.py --check` included).
- `python Scripts/CI.py` (strict, every stage including the clang-cl Release and Dist portability builds): passed. Its first run found the Release timing dependence of the reload type-gate test (above); the rerun after the fix passed every stage: unit Debug 3,464 and Release 3,463 test cases (31 child targets skipped in each), gpu Debug and Release, golden Release, feature Debug and Release, automation Release, determinism.
- `Test.py --suite feature --config Release --require-gpu`: passed, also from an empty `Library/`.
- `Test.py --suite determinism --junit`: passed; both replays give their committed hashes in the Debug and Release editors.
- `Editor --headless --project Projects/FeatureTest --run-tests`: exit 0 in Debug and Release, 170 case rows (169 passed and QuitApp's expected quit), 52 suites and replays, gates 1, 1b, 2, 3, 4 and 9 evaluated and passed, `Projects/FeatureTest` byte-identical afterwards. The Debug and Release reports have identical ticks and final state hashes for all 52 suite and replay rows.
- `Scaffold.py --check --config Debug` reproduces the project that `Scaffold.py --config Release` generated, byte for byte.
- No golden image changed; the replay hashes are unchanged from stream C's recording.
- Observation outside M14's files: `Automation/Provenance.json` has no entry for 24 golden files that predate M14 (the M9 Renderer II golden scenes `Assets/Scenes/Golden/{AnnotatedScreenshot,GtaoOff,GtaoOn,GtaoOrtho,SelectionOutline,ShadowsFar,ShadowsNear,ShadowsOrtho,SpotShadows}.scene` and the materials `Assets/Materials/Golden/RendererII/{Blue,Grey,Red}.material`, each with its `.meta`); the contract's baseline already lacked them, and decision 8 keeps the golden content and the baseline untouched. Every file the M14 scaffolds write has its entry.

After the review fixes (above), on the same machine, on the final staged tree:

- `python Scripts/Scaffold.py --config Release` regenerated the project last (replays recorded last); a second regeneration was byte-identical. `Determinism.replay` `FinalStateHash` `2a75dca847e98e59` (600 ticks, re-recorded for M07), `Input.replay` `ff0e3f70e3200f86` (40 ticks, unchanged).
- `python Scripts/Test.py --suite feature --config Release --require-gpu`: passed; 171 case rows (the new Callbacks case included), 52 suites and replays, gates 1, 1b, 2, 3, 4 and 9 evaluated and green, `Projects/FeatureTest` unchanged.
- `python Scripts/Test.py --suite determinism --junit`: passed; both replays give their committed hashes in the Debug and Release editors.
- `python Scripts/Scaffold.py --check --config Debug`: the Debug editor's scaffolds reproduce the Release regeneration byte for byte.
- `python Scripts/GenerateDocs.py --config Debug`: regenerated once; only `Validation.md` changed (`ASSET_TYPE_MISMATCH` auto-fixable).
- `python Scripts/CI.py` (strict, every stage, run once on the final staged tree): passed in 2,522 s. Static checks (the new `MakeCoverageGapFixtures.py --check` among the fixture generators), Debug, Release and Dist builds without warnings, references verified, unit Debug 3,470 and Release 3,469 test cases (31 child targets skipped in each), gpu Debug and Release 257 under each API cap, golden Release 25 of 25 matched, feature Debug and Release with the six editor-mode gates green, automation Release 264 tests with method coverage 86/86, determinism Debug and Release, portability including the clang-cl Release and Dist builds. Per the product owner's process change during the review fixes, PreCommit was not rerun separately: CI covers its stages.
- No golden image changed (25 of 25 matched); the only replay hash that changed is `Determinism.replay`'s, for the reason above.
- The provenance gap of the 24 M9 golden files stays recorded under review fix M11.
