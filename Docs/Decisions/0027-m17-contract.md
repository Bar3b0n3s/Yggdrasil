# 0027 — M17 Tetris contract

- **Status:** accepted; the contract, committed with `PreCommit.py --contract` (ADR 0004). Lead-owned; drafted by the contract agent from the lead's plan, reviewed by three read-only reviewers (37 findings, all fixed) and by the lead. The friction list, the session record and "Integration" are the lead's to fill in as the milestone runs.
- **Date:** 2026-10-18.
- **Base:** `e6964bf` (M16 complete, ADR 0026, and `Docs/Backlog.md`), branch `milestone/m17`.
- **Authority:** Roadmap "Rules for every milestone" (rule 3, the contract task) and M17; Architecture §11.10, §13.4, §13.6, §13.11, §13.12, §15.1, §15.8 and §15.9; ADR 0004, ADR 0009 decision 33, ADR 0010 decision 32, ADR 0023, ADR 0024 decisions 1 and 19, ADR 0025 decision 10, and ADR 0026 (decisions 3, 7, 8, 15 and 18; frictions F10, F12, F27 and F29; predicted P06 and P10). The lead's plan (`bin/Reviews/M17-Plan.md`) and the analysis behind it (`bin/Reviews/M17-Map-Pipeline.md`, `M17-Map-Content.md` and `M17-Map-Gameplay.md`: three readers, 22 adversarial verdicts on 11 candidate gaps and a completeness critic, N1 to N7; workflow `wf_c0a7539b-d7f`) are kept under the ignored `bin/Reviews/`.

## Context

M17 is the first required demo: an agent builds `Projects/Tetris` through the MCP tools only (Roadmap M17, Architecture §13.11), and the engine, editor and documentation gaps it finds are fixed in their owning modules, never worked around in the game. Three implementation streams precede the build:

- **A (C++):** automation behaviour and generated descriptions (A1 to A5).
- **B (Python):** the games suite and the hardening soak (B1 to B4).
- **C (documentation):** the DocExamples lint rule and the documents the builder reads (C1 to C5).

Then the builder's sessions run in the main checkout, with fixes between them. The facts this contract relies on were checked again in the code:

- **No engine feature is missing for Tetris.** Every feature of Roadmap M17 maps to an existing API (the Gameplay map's feature tables, the Content map's section 1; none of the 11 candidate gaps that the 22 verdicts examined is a missing game feature):
  - Input edges: `Input.IsActionDown`, `IsActionPressed` and `IsActionReleased` (`Engine/Source/Engine/Scripting/Bindings/Input.cpp:134-136`) latch per step (`Engine/Source/Engine/Platform/Input/InputState.h:38-51`), so a press between two ticks is seen once: enough for DAS/ARR, hard drop, rotation, hold and restart.
  - The 7-bag: `Random.New` (`Bindings/Random.cpp:188`), available in the Runtime and Test environments but not at a module's top level.
  - The cell pool: `Scene.Instantiate` (`Bindings/Scene.cpp:280`), `Entity:SetActive` (`Bindings/Entity.cpp:409`) and `MeshRenderer:SetMaterial` (`Bindings/Components.cpp:437`); the HUD through `Text` proxy writes; sounds through `Audio.PlayOneShot` (`Bindings/Audio.cpp:67`) with `.sfx` assets.
  - The look: GTAO and PCSS shadows work with an orthographic camera (the `GtaoOrtho` and `ShadowsOrtho` goldens of `nvidia-61x`), but need content choices: a backboard that receives them, `LightAngle` about 10, `EmissiveStrength` 2 to 4 for bloom, a HUD layout that fits 720 × 900 (Content map C05 to C10).
- **Four of Roadmap M17's seven acceptance items are mechanical already, and two more in part** (Pipeline map section 1, which numbers them 1 to 7): the suites through `games.evaluate_run` in both editor halves and the testing Dist export (item 1; `Scripts/Test.py:1797-1813`, `:1827-1898`); the replays with `strictHash`, their checkpoints and the input check (item 2's first half; `Test.py:1814-1822`); the editor and testing Dist hashes compared by `games.compare_game_hashes` (item 3); a set `StartScene`, which the exports require (item 4's first half); `project.validate` clean (`games.check_validate`) and the Dist smoke test, which runs exactly `--headless --frames 300 --expect-no-errors` (item 5; `Editor/Source/EditorCore/Export/Exporter.cpp:1366-1383`); and the provenance audit in index mode (item 7; the games suite's audit step). Three parts need more:
  - "Replaying twice" (item 2's second half) holds across processes (the Debug and Release editors' `test.run` and the testing Dist export's `--feature-test`, compared by `games.compare_game_hashes`), but no demo replay is played twice through `input.replay` in one session: the suite's only `input.replay` call is the `withoutInput` input check (`Test.py:1491`).
  - `Export.BuildScenes` (item 4's second half) is never checked: an empty list passes the validator (`Editor/Source/EditorCore/Project/ProjectValidator.cpp:1258-1266`) and the exporter (`Exporter.cpp:559-564`), while an empty `StartScene` fails only the export (`Exporter.cpp:549-552`).
  - The look (item 6) is checked by people, on screenshots, by design.
- **Verified defects and traps** (the skipped tests of decision 2 reproduce 1, 3, 5, 6, 7, 8 and 9; item 4's new test is live and its proof is the deleted case, B3; item 2 has no test and is answered by decision 7):
  1. **`input.record` stop never replays its candidate with its input.** Stop finishes the recording (`Engine/Source/Engine/Automation/Methods/ReplayMethods.cpp:292`), writes an event-free one directly (`:293-298`) and otherwise plays it only without its input (the recording guard, `:299-317`, `WithoutInput = true` at `:315`), while `test.run {record}` verifies its candidate with its input first (`Engine/Source/Engine/Testing/FeatureTestRunner.h:58-60`; `FeatureTestRunner.cpp:1122-1135`). A script-table write from `script.eval` calls no engine API, so nothing invalidates the recording (`Engine/Source/Engine/Scripting/LuaHelpers.cpp:43-52`), and a false expectation fails the input check too, which writes the candidate (Gameplay map GTF-01, GDF-03 and GAF-09: three readers independently).
  2. **A replay recorded with `input.record` cannot be re-recorded through MCP.** §13.6 says to repeat the recorded `play.step` inputs from the transcript (`Docs/Architecture.md:1458`), but no tool reads the transcript or a replay's events: `result_read` reads only the offload folders (`Tools/MCP/engine_mcp/server.py:722-730`), `asset.getProperties` serves materials and sound effects only (`Editor/Source/EditorCore/Automation/AssetMethods.cpp:186-200`) and `input.record` returns counts (`Engine/Source/Engine/Automation/Methods/ReplayMethods.h:44-53`) (GT-01 confirmed by both lenses, GD-01 partial; "costly without" for M17).
  3. **`test.run` silently plays the saved scene.** Suites read their scene's file (`Editor/Source/EditorCore/Play/EditorPlayController.cpp:273-279`) and the preflight has no dirty-scene check (`Editor/Source/EditorCore/Play/EditorFeatureTestHost.cpp:41-69`), while `play.start` plays the open scene with its edits (`EditorPlayController.cpp:266-272`) and `input.record` start refuses a dirty scene (`:855-866`) (GTF-04).
  4. **The automation suite audits unstaged demos from the working tree:** `Tests/Automation/test_audit_provenance.py:375-381` runs `AuditProvenance.py --demos --files disk` on the repository, against ADR 0026 decisions 3, 7 and 9 (Pipeline map G3, confirmed): a half-built `Projects/Tetris` with any disk-mode finding would fail every commit's automation suite.
  5. **The Dist half names the package after the `.eproj` stem** (`Scripts/Lib/games.py:74`; `Test.py:1847`, `:1852`, `:1869`, `:1879`), while the exporter names the executable, the testing user data and its log after `ProjectSettings::Name` (`Exporter.cpp:508-514`, `:1286-1289`) (Pipeline map R6, confirmed by both lenses).
  6. **The soak cannot see script errors:** a subject fails only when `play.step` fails (`Scripts/Hardening.py:374-385`), and `play.step` keeps ticking through a non-fatal script error (GDF-11).
  7. **A refused `test.run {record}` does not say which row failed:** "recording requires passing selected cases and a valid starting scene" (`FeatureTestRunner.cpp:1311-1316`), so a recording filter that also matches a stale replay's path refuses without saying why (GTF-07).
  8. **The docs teach code with lint warnings, which the games suite fails:** `games.check_script_check` fails on any diagnostic (`games.py:425-436` in this tree), DocExamples counts errors only (`Tests/Source/Support/DocExamples.cpp:298-299`), and §13.11's excerpt puts two statements on one line (`Docs/Architecture.md:1525`, Luau's `SameLineStatement`). A run of decision 5's C1 case with warnings reported (a local change, reverted) found exactly this one warning in the checked documents (GAF-10).
  9. **An entity path inside component data is refused without a hint:** the canonicalizer resolves asset references only (`Engine/Source/Engine/Automation/Protocol/MethodRegistry.cpp:397-398`), and the strict reader reads an `EntityRef` as a UUID (`Engine/Source/Engine/Reflection/Private/ReflectionWalk.cpp:398-406`, "expected a 16-digit hex UUID string" at `Engine/Source/Engine/Core/Json/JsonReader.cpp:727-737`), so §13.11 step 6's `ScoreText: "/Game/ScoreText"` fails with InvalidParams and nothing more (Content map C02, R1).
- **Many documentation errors** in §13.11, §13.6, the seed wording of §13.4, §13.6 and §11.10, the skills and the generated descriptions (Content map D1 to D13; the Gameplay map's three doc-error lists). The builder reads only what `docs.get` serves: the skills, the generated reference with its schemas, the script declarations and the script templates (`Editor/Source/EditorCore/Automation/ObserveMethods.cpp:36-45`; critic N1). Architecture, the Roadmap and `CodeStyle.md` are not served, so every builder-facing correction must also land in a served document.
- **The soak:** `CI.py --milestone`'s hardening stage soaks every scene below `Assets/Scenes/` of every `Projects/*` project for 10,000 rendered lockstep ticks without input (`Scripts/Lib/hardening.py:435-450`; `Hardening.py:350-385`). Tetris is the first soaked demo (Breakout lives under `Tests/Projects/`). Without input its pieces fall, stack and top out after about 5,200 to 9,500 ticks at guideline gravity (the Gameplay map's simulation, unverified against the game).
- **Backlog gaps:** ADR 0026's F29 deferred an optional input-check warning to "M17 at the earliest", but `Docs/Backlog.md` has no row for it (critic N3); F10's row (`test.run {record, expect}`) says "M17 at the earliest".
- **Binaries:** `bin/Release-windows-x86_64/Editor/Editor.exe` and the Dist Runtime date from 2026-10-15 10:06 and 10:08; 104 sources under `Engine/`, `Editor/` and `Runtime/` have a later modification time (whether their content differs is not known; critic N5). This contract's Debug build of `Tests` relinked the Debug editor. Debug, Release and Dist are rebuilt on the integrated tree before session 1.

## Decisions

### 1. The gate stays `CI.py --milestone`

The product owner did not take up the proposal to make the hardening stage conditional, so the milestone closes with one strict `python Scripts/CI.py --milestone` on the final tree, as ADR 0024 decision 19 set: every stage plus `hardening` (whose soak now covers Tetris, decision 8), clang-cl required. The other commits of M17, each with one gate run on its final tree (ADR 0023 decision 1):

- **The contract** runs `PreCommit.py --contract` once (ADR 0004).
- **The stream integration** is committed on its own before session 1 and, as an integration commit, runs plain `python Scripts/CI.py` instead of PreCommit (ADR 0023 decision 1, Roadmap rule 1). That run also covers what PreCommit leaves out: the Release and Dist builds, the games stage's Release half and Dist half for Breakout (B2's package name against a real export) and the automation suite against the Release editor (`test_feature_test`'s `Scaffold.py --check`, which records FeatureTest's `input.record` replays through A1's Stop). Plain `CI.py` leaves `hardening` out, so the lead also runs the FeatureTest soak once before session 1 (decision 8).
- **Fixes between sessions** are integrated into the working tree and verified with targeted runs (ADR 0023). A fix batch is committed as an ordinary commit, running `PreCommit.py` with `Projects/Tetris` unstaged (decision 11; ADR 0026 decision 8: engine fixes in M17 and M18 land as ordinary commits), when it alters canonical output or hashes (decision 10, so its upgrades and re-recordings land with it) or when the lead wants a checkpoint; otherwise it rides into the milestone commit, as M16's fixes did.

### 2. What the contract freezes, and what it implements

No existing public C++ signature changes. A1 to A4 change the documented behaviour of existing methods, so their contract is this ADR's text ("Frozen interfaces") plus skipped tests, and the contract updates the header comments that document that behaviour (comments only): `ReplayMethods.h` (`InputRecord`: A1's verification before the input check), `TestMethods.h` (`TestRun`: A2's refusal and its place among the errors) and `MethodRegistry.h` (`Invoke`: A4's issue and hint). One C++ function is added for A2, with its struct: `FeatureTestRunner::GetSelection() const -> Result<FeatureTestSelection>` (`Engine/Source/Engine/Testing/FeatureTestRunner.h`), the selection of the last successful `Begin` (each selected suite's index into `Testing.Suites` and each selected replay's path), so `test.run` checks the scenes of what it selected without a second discovery or a copy of the runner's filter rule; its stub starts with `ENGINE_CONTRACT_STUB();` and returns `Unsupported`. Stream B adds two Python functions whose signatures are frozen with stubs.

- **Python stubs** (ADR 0024 decision 1): `games.check_build_settings(project_file: Path) -> list[str]` and `games.read_game_name(path: Path) -> str` in `Scripts/Lib/games.py`, each with the docstring that states its behaviour and the body `raise NotImplementedError("contract stub: games (ADR 0027 decision 4, B<n>, stream B)")`. `Test.py` and `Hardening.py` do not change; the behaviour B1, B2 and B4 give them is frozen here and by the skipped tests.
- **Skipped tests with their final names** (`doctest::skip(true)`, `unittest.skip` with a "contract stub: ..." reason), each body failing when forced against the contract tree:

  | Item | Test | Forced against the contract tree |
  |---|---|---|
  | A1 | "ReplayMethods: input.record stop refuses a recording that a replay with its input does not reproduce" (`ReplayMethodsTests.cpp`) | `REQUIRE( diverged.contains("error") )` fails: Stop wrote `Assets/Tests/Boosted.replay` (`events` 2, `checkpoints` 2, `inputCheckTick` 60) |
  | A2 | "TestMethods: test.run refuses while a selected suite's scene is open with unsaved changes" (`TestMethodsTests.cpp`) | `REQUIRE( response.contains("error") )` fails: the run played the saved `Main.scene`, and `Bound/sees the edit` failed |
  | A2 | "TestRunner: Begin's selection names each selected suite and replay" (`FeatureTestRunnerTests.cpp`) | the stub returns `Unsupported`: the InvalidState check before `Begin` fails, then `REQUIRE_MESSAGE( filtered.has_value() )` |
  | A3 | "TestRunner: a refused recording names the selected rows that did not pass" (`FeatureTestRunnerTests.cpp`) | 5 checks fail: the message is the bare "recording requires passing selected cases and a valid starting scene" without a hint; the report rows the message must name (`Pilot/drive` passed, `Pilot/check` failed, `replay:Assets/Tests/Replays/Pilot.replay/FinalStateHash` failed) already hold |
  | A4 | "MethodRegistry: an entity path in an EntityRef component value is refused with a hint naming the entity id" (`MethodRegistryTests.cpp`) | 3 checks fail: neither the error nor its two issues (already InvalidArgument at `/components/Script/Fields/Target` and `/components/Script/Fields/Targets/1`) carry the hint |
  | C1 | "DocExamples: every Luau example in the docs is free of lint warnings" (`DocExampleTests.cpp`) | `REQUIRE` fails: `DocExampleChecker::Check` returns no Warning finding for the seeded `SameLineStatement`; with warnings reported (local experiment), the seeded warning comes back at document line 5 and the documents hold one, `Docs/Architecture.md:1525` |
  | B1 | `test_games_suite.EvaluationTests.test_a_game_needs_a_start_scene_that_its_build_scenes_list` | `NotImplementedError` from the stub in every subtest |
  | B1 | `test_games_suite.SuiteTests.test_the_editor_half_fails_a_game_without_build_settings_first` | the editor half passes a game with neither `StartScene` nor `Export.BuildScenes` (`PASSED != FAILED`) |
  | B2 | `test_games_suite.EvaluationTests.test_the_game_name_is_read_from_a_package_manifest_or_a_project_file` | `NotImplementedError` from the stub in every subtest |
  | B2 | `test_games_suite.SuiteTests.test_the_dist_half_finds_the_package_by_the_project_name_not_the_file_stem` | the Dist step fails: "the package holds no .../Exports/Dist/Game/Game.exe"; without a manifest its detail names that, not `Game.json` |
  | B4 | `test_hardening.SoakDeadlineTests.test_a_soak_subject_whose_scripts_raise_an_error_fails` | the soak never calls `script.errors` (`'script.errors' is not in list`), so no error is reported, an OnStart error included |
  | A1 | `test_replay.ReplayTests.test_record_refuses_a_recording_changed_by_a_script_table_write` | `EngineError not raised`: the Release editor wrote the boosted replay |
  | A2 | `test_script_tests.ScriptTestRunnerTests.test_test_run_refuses_a_dirty_open_scene_that_a_selected_suite_uses` | `EngineError not raised`: the run played the saved scene |

- **Live tests added by the contract**, because they pass against the contract tree:
  - "TestMethods: test.run runs a suite of another scene while the open scene has unsaved changes" (A2's other half: a suite bound to `Main.scene` plays its saved file while `Other.scene` is open and dirty).
  - `test_audit_provenance.FixtureTests.test_demos_audits_every_staged_game_project_and_names_the_unstaged_ones` (B3): `AuditProvenance.py --demos` already audits the index by default (ADR 0026 decision 3), so the test passes today; what B3 fixes is the automation suite's disk-mode case, `test_demos_audits_every_discovered_game_project`, which stays live in the contract and stream B deletes. B3's regression proof is that the deleted case fails, and the new one passes, with an unstaged copy of a failing fixture under `Projects/` in stream B's worktree.
- **Test support the contract adds** (each a no-op for today's tests, so the existing tests keep passing once B1, B2 and B4 land):
  - `MakeEntityFieldSchemas` (`MethodRegistryTests.cpp`);
  - in `test_games_suite.py`: the `name` parameter of `FakeFeatureTestRun` (default `"Game"`); the `name` and `manifest` parameters of `FakeExportingSession`, whose testing export now writes the package of the game `name` with its `Game.json` (at the package root, or in the bundle's `Contents/Resources` on macOS) and returns `executable`, as `project.export` does (`ExportMethods.cpp`, `Exporter.cpp`); the `build_settings` parameter of `write_project` and `make_game_repository` (default true: `StartScene` is `TINY_SCENE` and `Export.BuildScenes` lists it); and the end-to-end editor-half test's project, which sets the same two settings through `project.setSettings` (verified against the Release editor);
  - `test_hardening.ScriptErrorClient`, which serves `script.errors` as the editor does (`since: "end"` returns no entry and the current cursor; a decimal `since` returns the entries after it, at most `limit`; three errors already in the stream before the soak), raises its error at `play.start` (an OnStart error) or at `play.step`, and records every `since`.
- **Unchanged:** no generated reference changes (no description, method, schema or registry type changed in the contract); `Test.py`, `CI.py`, `PreCommit.py`, `Hardening.py`, `Lib/scripts.py`, `ModuleRules.json` and the bridge do not change.

### 3. Stream A: automation behaviour and generated descriptions (C++)

Each item's exact behaviour, errors, codes and messages are under "Frozen interfaces"; the owner files are under "Ownership".

#### A1. `input.record {action: "Stop"}` verifies a recording with input events with its input first

Before the input check, a candidate with input events is replayed with its input in a fresh ordinary playback (verify, strictHash, its expectations), as `test.run {record}` already does (`FeatureTestRunner.h:58-60`): it must reproduce every checkpoint, the final hash and every expectation, and raise no script error, the rule of `test.run {record}`'s ordinary-play verification ("recording ordinary-play verification reported script errors", `FeatureTestRunner.cpp:1125-1128`) and of every replay row that `test.run`, the games suite and `--feature-test` judge (`FeatureTestRunner.cpp:1205-1206`). Otherwise Stop refuses with Validation naming the first differing checkpoint (tick, expected and observed hash), the failing expectation or the first script error, writes nothing, and leaves the recording stopped. It catches a script-table write from `script.eval` during the recording (undetectable at the write, `LuaHelpers.cpp:43-52`), an expectation that errors or is false, a script error that a later `test.run` would fail, and any nondeterminism. `input.record` is runtime-available, so the Runtime's Stop changes the same way.

One difference from `test.run {record}` stays, deliberately: **a recording without input events is written without a verification**, as now, while `test.run {record}` verifies an event-free recording too (only its input check is skipped, `FeatureTestRunner.cpp:1129-1135`). Two live tests pin this: "ReplayMethods: input.record stop refuses a recording whose final state does not depend on its input" requires the recorded session to stay the play session after an event-free Stop (`ReplayMethodsTests.cpp:463-468`), and "ScriptMethods: errors preserve distinct embedded expectation pointers across RPC cursor pages" writes an event-free replay whose expectations raise errors, to read those errors back later (`ScriptMethodsTests.cpp:139-146`). An event-free recording's expectations and any `script.eval` write during it are first checked by the `input_replay {verify: true, strictHash: true}` the docs require after every recording; the limit has a Backlog row (decision 12). The input check keeps its rule: script errors in the run without input do not decide it, as in `test.run {record}`.

- **Owner:** `Engine/Source/Engine/Automation/Methods/ReplayMethods.cpp`.
- **Tests:** C++ "ReplayMethods: input.record stop refuses a recording that a replay with its input does not reproduce" (a script-table write through `script.eval` mid-recording; a false expectation; and the same input with a satisfied expectation still written; stream A adds a subcase for a script error that the recording's own input raises, refused with the script-error reason); Python `Tests/Automation/test_replay.py` `ReplayTests.test_record_refuses_a_recording_changed_by_a_script_table_write`. Every existing test that records keeps passing, the `RuntimeAutomationServer` input-record cases and the two event-free pins above included.

#### A2. `test.run` refuses a dirty open scene that a selected suite or replay plays

InvalidState with the hint "scene.save first; test runs play the saved scene file", only when the open scene has unsaved changes and a selected suite is bound to it or a selected replay plays it, compared as the run resolves those scenes; an empty-scene suite, or a suite or replay of another scene, is unaffected. The check reads `Begin`'s selection through `FeatureTestRunner::GetSelection` (decision 2).

- **Owner:** `Editor/Source/EditorCore/Automation/TestMethods.cpp`, `Engine/Source/Engine/Testing/FeatureTestRunner.cpp` (`GetSelection`) and, if the check lives there, `Editor/Source/EditorCore/Play/EditorFeatureTestHost.cpp`.
- **Tests:** C++ "TestMethods: test.run refuses while a selected suite's scene is open with unsaved changes" and "TestRunner: Begin's selection names each selected suite and replay" (skipped), and "TestMethods: test.run runs a suite of another scene while the open scene has unsaved changes" (live, decision 2); Python `Tests/Automation/test_script_tests.py` `ScriptTestRunnerTests.test_test_run_refuses_a_dirty_open_scene_that_a_selected_suite_uses`.

#### A3. A refused `test.run {record}` names the selected rows that did not pass

The suite/case or replay path of each, with its status.

- **Owner:** `Engine/Source/Engine/Testing/FeatureTestRunner.cpp`.
- **Test:** "TestRunner: a refused recording names the selected rows that did not pass".

#### A4. An `EntityRef` value in component data that is not a 16-hex id is refused with a hint

The hint names the rule: "an Entity value in component data takes the entity's 16-hex id; use {\"$ref\": \"<op>.entity.id\"} in a batch or the id entity.get returns". The rule itself stays (ADR 0010 decision 32: an `EntityRef` keeps the handle pattern); path resolution in component data is deferred (decision 12). The A2 and A4 hints name methods (`scene.save`, `entity.get`), as the engine's other hints do (`input.record` start's "save the scene with scene.save, then start the recording", `EditorPlayController.cpp:862`), because `engine_client` and Runtime callers read them too; the MCP tools carry the same names with `_` (`scene_save`, `entity_get`).

- **Owner:** `Engine/Source/Engine/Automation/Protocol/MethodRegistry.cpp` (`ParamsCanonicalizer`).
- **Test:** "MethodRegistry: an entity path in an EntityRef component value is refused with a hint naming the entity id".

#### A5. Generated descriptions

Registration strings only; no behaviour change. The lead regenerates `Docs/Reference/*`, `Resources/Scripting/Engine.d.luau` and `Tools/MCP/catalog.json` at integration (`GenerateDocs.py`), so the references check pins them.

| Member | Owner file | New description says |
|---|---|---|
| `Text.Size` | `Engine/Source/Engine/Scene/Registration/RenderingRegistration.cpp:228` | Screen: 1080p-reference pixels scaled by viewport height / 1080; World: Size / 100 metres per em before the entity's scale, from `WorldTextPixelsPerMetre` |
| `Input.IsActionPressed`, `Input.IsActionReleased` | `Engine/Source/Engine/Scripting/Bindings/Input.cpp:135-136` | the edge since the previous latch of the current phase; a press and release between ticks reports both edges with `IsActionDown` false; a multi-tick frame reports it on the first tick; an unknown name raises `INPUT_UNKNOWN_ACTION` |
| `Random.New` | `Bindings/Random.cpp:188` | its state is outside the state hash; a constant seed repeats the sequence every session; seed it from `Random.Integer` to follow the session seed; not available at a module's top level |
| `Field.Asset` | `Bindings/Script.cpp:572` | the `AssetType` names, `AudioClip` for imported audio and `.sfx`; any other name fails the import |
| `Test.WaitUntil` | `Bindings/Test.cpp:536` | evaluated now and after every tick; must return a boolean; a timeout fails the case |
| `Test.GetStateHash` | `Bindings/Test.cpp:551` | includes the tick, so two reads at different ticks differ; compare runs with `input.replay` |
| `input.record` | `Engine/Source/Engine/Automation/Methods/ReplayMethods.cpp:415` | Stop replays a recording with input events with its input (every checkpoint, the final hash and every expectation reproduced, no script error), then runs the input check (A1); an event-free recording is written as recorded, so verify it with `input.replay` |
| `test.run` | `Editor/Source/EditorCore/Automation/TestMethods.cpp:227` | refuses (InvalidState) while the open scene has unsaved changes and a selected suite or replay plays it, since runs play saved scene files (A2); a refused recording names the selected rows that did not pass (A3) |
| `Export.BuildScenes` | `Engine/Source/Engine/Project/ProjectSettings.cpp:352` | the scenes the export loads strictly beside `StartScene` (each must be a scene file that loads strictly); what ships does not depend on it, since `Game.pak` holds every registered asset below `Assets/` (replacing "Project-relative scenes packed into the export", which misstates it, `Exporter.h:42-57`) |
| `Application.IsFocused` | `Bindings/Application.cpp:68` | nondeterministic: follows the host window in every session; never base gameplay on it |
| `Application.IsEditor` | `Bindings/Application.cpp:66` | gameplay that branches on it splits the editor's and the export's hashes |

Tests: the references check (`GenerateDocs.py --check`) after the lead's regeneration; a case that pins a changed string (`ScriptApiRegistry`, `RegisterMethods` and the reference-generator tests) follows it in stream A.

### 4. Stream B: the games suite and the soak (Python)

#### B1. `games.check_build_settings(project_file: Path) -> list[str]`

One failure line each for an empty or absent `StartScene`, an empty or absent `Export.BuildScenes`, and a `StartScene` that `Export.BuildScenes` does not list; called first in `check_game_in_editor`, so PreCommit's Debug half catches it. The engine is unchanged: an empty `BuildScenes` stays valid (FeatureTest relies on it). The first two rules are the demos' acceptance rule (Roadmap M17 and M18: "StartScene and Export.BuildScenes set"); the third, that `BuildScenes` lists `StartScene`, is this decision's convention for the demos, which the Roadmap does not state (Pipeline map P-G2's necessity verdict), and its message names this ADR. `BuildScenes` lists the scenes the export loads strictly beside `StartScene`; it does not decide what ships (`Game.pak` holds every registered asset below `Assets/`, `Exporter.h:42-57`), so the second message asks for "the scenes the export must load strictly" (A5 fixes the field's description). Breakout already complies (`Tests/Projects/Breakout/Breakout.eproj:5`, `:66`).

- **Owners:** `Scripts/Lib/games.py`, `Scripts/Test.py` (`check_game_in_editor`).
- **Tests:** `EvaluationTests.test_a_game_needs_a_start_scene_that_its_build_scenes_list` (the rules, the exact lines and every ValueError) and `SuiteTests.test_the_editor_half_fails_a_game_without_build_settings_first` (the wiring: a staged game without the settings fails the editor half with those lines first while its other checks still run, and a malformed `Export` is one failure line naming the file).

#### B2. The Dist half names the package after the game, not the file stem

The executable comes from `project.export`'s `executable` result, relative to `outputDirectory`; the game's name for the package directory, the log and the testing user data comes from the package manifest's `Name` (`games.read_game_name(manifest_or_project_file) -> str`), read where the exporter writes it ("B2" under "Frozen interfaces"). The comment of `GameProject.name` (`games.py:74`) is fixed: the stem names the suite's steps and reports only.

- **Owners:** `Scripts/Lib/games.py`, `Scripts/Test.py` (`check_game_export`).
- **Tests:** `EvaluationTests.test_the_game_name_is_read_from_a_package_manifest_or_a_project_file` (both documents and every ValueError) and `SuiteTests.test_the_dist_half_finds_the_package_by_the_project_name_not_the_file_stem` (the package, executable, log and testing user data under the Name; a testing export without its manifest is one failure line naming `Game.json`).

#### B3. The automation suite audits staged demos only

`test_demos_audits_every_discovered_game_project` is replaced by `test_demos_audits_every_staged_game_project_and_names_the_unstaged_ones` (index mode; the staged projects audited and passing; each project only on disk one passed step saying it is not staged). This restores ADR 0026 decisions 3, 7 and 9. The new test is live in the contract (decision 2); stream B deletes the old one.

- **Owner:** `Tests/Automation/test_audit_provenance.py`.

#### B4. A soak subject fails on a new script error

The soak takes `script.errors`' cursor after `scene.open` and before `play.start`, so an error in `OnStart` counts (`play.start` activates the session, which runs `OnStart`: `EditorPlayController.cpp:332-334`, `PlaySession.cpp:902`, `ScriptEngine.cpp:251-273`; decision 5's C2 builds the cell pool and the bag there), and reads from it after the soak; any new error fails the subject with the first error's message. The editor's error stream belongs to its controller and is never cleared (`EditorPlayController.cpp:732`), and each soak scene starts a fresh editor, so what comes after the cursor is that scene's play session alone.

- **Owners:** `Scripts/Hardening.py` (`soak_editor_scene`; its docstring and the module's soak paragraph), `Tests/Automation/test_hardening.py` (its `FakeClient` answers `script.errors`, so the existing soak tests keep passing).
- **Test:** `test_hardening.SoakDeadlineTests.test_a_soak_subject_whose_scripts_raise_an_error_fails` (no error, an error while playing and an error in `OnStart`, which the fake raises at `play.start`; the order `scene.open`, `script.errors`, `play.start` and the `since` sequence `["end", "3", "4"]`, or `["end", "3"]` without an error, are pinned, so a cursor read again after the soak, or a read from the start of the stream, fails it).

### 5. Stream C: documentation and the DocExamples lint rule

Stream C covers every correction of the Content map's section 4 (D1 to D13) and of the Gameplay map's three doc-error lists (GA, GD and GT), and every item the predicted-friction table below assigns to C2 or C4, whether or not the lists below name it.

#### C1. DocExamples fails on lint warnings

"DocExamples: every Luau example in the docs is free of lint warnings" (`Tests/Support/DocExamples*`, `Tests/Source/EditorCore/Scripting/DocExampleTests.cpp`): `DocExampleChecker::Check` returns Warning findings as well as Error findings, each with its `Diagnostic.Severity` (its header comment says so; no signature change), and the new case fails on every Warning finding after its seeded self-check. Every example that warns is fixed: today only the §13.11 excerpt's `then self:HardDrop() return end` (`Docs/Architecture.md:1525`).

- **The two cases that compare Check's findings keep comparing Error findings only:** "DocExamples: every Luau example in the docs type-checks", and "DocExamples: a seeded mistake is reported at its document line", which requires the findings to equal `Tests/Data/TypeChecker/DocExamples/Seeded/Expected.json` one to one (count, line, code and message, `DocExampleTests.cpp:252-296`): it filters them to Severity Error, and `Expected.json` keeps listing errors only.
- **A game's own notes are left out of the lint case:** `Projects/*/AGENTS.md` and `Tests/Projects/*/AGENTS.md` (`IsProjectNotes` in the frozen case). The reason for C1 is that the games suite fails a game on any `script.check` diagnostic, and `script.check` never reads the notes. The builder writes them in a project that stays unstaged (decision 11) and has no tool that lints a fence, while Luau enables every lint warning by default (`Vendor/Luau/Config/src/LinterConfig.cpp:9-13`, kept by `ScriptTypeChecker`), so a note's `local GRAVITY = {...}` would raise `LocalUnused` and fail the unit suite of every fix commit until the next builder session. The type-check case keeps checking the notes (decision 11).

#### C2. Architecture

- **§13.11** rewritten against the code (steps 1 to 10):
  - launch with `renderer: "vulkan"`; `project_validate` reports the build and suite codes, while `_meta` only counts log entries;
  - the scaffold with a backboard, `LightAngle` 10, `ShadowDistance` 40, HDR emission and `Camera.Clear` `Color`; the assets created in a first batch and referred to by path, `$ref` kept for `EntityRef` values, and an entity param in a batch given as a path (`"parent": "/Game"`, C-R4);
  - `Board.luau` holds the bag, hold, top-out, scoring and levels, so `Board.test.luau` covers them in the empty scene;
  - the bag generator created in `OnStart` from `Random.New(Random.Integer(0, 2147483647))`; gravity as a frozen integer table of ticks per row, never `^`;
  - a pool of 220 well cells plus preview cells without colliders, touched only where changed; `Entity` fields wired by id;
  - screenshots at 720 × 900 in play with `target: "Play"`, `debugView` `AO` and `ShadowCascades`;
  - `play_start {lockstep: true}` without a seed after `scene_save`;
  - both replays recorded from Editor-only recording suites with `test_run {record}` and verified twice with `input_replay`, plus the input check;
  - `test.run` refuses a dirty scene;
  - `AGENTS.md` written directly, its `luau` fences type-checked unless `skip=` (not linted, C1);
  - "`script_check` reports zero" says lint diagnostics included, since the games suite fails on any.
- **§13.4:** the seed default wording (below), and the `EntityRef` bullet: ids, unique prefixes and paths apply to entity params, while an `EntityRef` inside component data (component fields, script `Field.Entity()` values) takes the 16-hex id only (ADR 0010 decision 32; A4; decision 12).
- **§13.5's `test.run` row:** the dirty-scene refusal (A2) and a refused recording naming the selected rows that did not pass (A3).
- **§13.6:** re-recording through recording suites; an `input.record` replay is re-recorded by planning its input again; Stop now verifies a recording with input events with its input (an event-free one is written as recorded, A1).
- **§13.4 and §13.6/§11.10 seed default wording:** the session seed, the project's `Simulation.Seed` XOR the scene's `Seed`.
- **§15.1 T5 and §15.8's games row:** the build-settings check (B1).
- **§15.1 T6, §15.8's hardening row and `CI.py`'s docstring:** the soak covers every scene of every `Projects/*` project (FeatureTest and the demos under `Projects/`, not `Tests/Projects/`), and a new script error during a scene's soak fails it (B4).

#### C3. Roadmap

M17's "replaying twice gives identical final state hashes (`input.replay` reports them)" and M18's matching line are reworded to what proves it: the builder's two `input_replay {verify, strictHash}` runs with equal `stateHash` in the transcript, and the games stage's three strict plays (Debug and Release editors, testing Dist export) equal to the committed hash and to each other.

#### C4. Skills (served by `docs.get`, so the builder sees them)

- **game-building:**
  - The definition of done gains M17's items: two named replays, replaying twice, `StartScene` set and `Export.BuildScenes` listing it (B1), the look screenshots and their paths, the soak rules (B4's script-error rule included), the `Board.test` topics.
  - Also: recording suites as the default for every replay; a Look subsection; screenshot size and play target; `_meta` against `project_validate`; `Entity` values by id; entity params in a batch as paths; `AGENTS.md` fences (type-checked, not linted); the seed and `scene_save`; `test.run`'s dirty-scene refusal; recording-suite names that no replay path matches; a fresh Claude Code session after a catalogue change; the determinism traps (`^`, `pairs` over entity keys, host queries).
  - From the predicted-friction table: text anchoring, pivots and proportional digits (C11); the canonical enum spellings (C22); cases driven by hard drops, and `TimeoutTicks` for gravity-paced ones (GDF-12, GTF-05); the first input inside the first 60 ticks and an expectation that only the input satisfies (GDF-02).
  - The Gameplay map's corrections: the Test template names its suite after the file stem and has no `Script_<File>` type (also in luau-gameplay); `Test.GetStateHash` leaves the list of integration tools (compare runs with `input_replay`); the hash-coverage wording (`Random.New` generators are outside the hash; drawn components are covered); the expectation example.
- **luau-gameplay:** the `Random` rules, the pooling cost, `^`, evaluation during recordings, the function-field record idiom for a `Board` with methods, action-name constants, the Test template's suite name.
- **game-debugging:** the screenshot size, evaluation during recordings.
- **build-and-test:** the soak wording (the projects it covers and B4's script-error rule) and the games suite's build-settings check (B1).
- **Size:** "DocExamples: every skill document has the docs.get frontmatter and fits inline" (`DocExampleTests.cpp:338`) caps each skill's serialized `docs.get` result at `DefaultOffloadThresholdBytes`, 48 KB (`ResultOffload.h:35`). game-building is already 32.4 KB raw and build-and-test 43.1 KB, so C4 measures both after its edits. Content that would push game-building over goes to luau-gameplay or game-debugging (both served by `docs.get` and linked from game-building), and build-and-test stays terse.

#### C5. Backlog

`Docs/Backlog.md` gains a row for every deferral and accepted limit this contract makes (decision 12) and rewords F10's row; ReviewChecklist §9. Each new row's source is "ADR 0027 decision 12" (the F10 and F29 rows also keep ADR 0026), never the maps, which live under the ignored `bin/Reviews/`.

### 6. The build

- **Builder:** one agent per session, using only the `mcp__engine__*` tools (loaded with ToolSearch) and `docs_get`. It may Read and Write exactly one file, `Projects/Tetris/AGENTS.md`: its own notes, which no automation method reads or writes (critic N2), so a later session can read what an earlier one learnt. It never reads engine source or any other file. It keeps a friction log (every awkward step, unclear error, missing method, documentation gap and workaround it was tempted to make, with evidence: the transcript line, the tool error, the code) and returns it. No other agent uses the bridge while it runs (one bridge process holds one editor connection; ADR 0026 risk 2).
- **Prompt:** it quotes Roadmap M17's "Features" and "Acceptance" verbatim, as they stand when the session starts (C3 rewords one acceptance line first), because the builder cannot read the Roadmap through `docs.get` (critic N1); and the M17 additions of this ADR: the two named replays from recording suites and the twice-replayed check (decision 7), soak readiness (decision 8), the look screenshots (decision 9), the one-file `AGENTS.md` rule, and that the skills are the reference. The `AGENTS.md` rule includes its fences: the unit suite type-checks every `luau` fence of the notes against the documentation stubs (not lint, C1), and the builder has no tool that does, so a fence is either code it has already run through `script_write` and `script_check` or carries `skip="notes"`.
- **Where:** the main checkout `D:\Engine`, because the session's MCP bridge spawns `D:\Engine\bin\<Config>-windows-x86_64\Editor\Editor.exe`. Stream agents work in worktrees branched from the contract commit; fix agents between sessions work in worktrees branched from the latest commit on `milestone/m17` (the committed integration, decision 1, plus the earlier fixes), so their diffs apply to the integrated tree. The lead applies their staged diffs (ADR 0026 decision 15).
- **Sessions:** the Roadmap sets no time box for M17. Sessions continue until acceptance; if five sessions have not reached it, the lead reports to the product owner before a sixth. Each session's dates and reach go in the session record below.
- **Before session 1:** streams A, B and C are integrated, the references regenerated, Debug, Release and Dist rebuilt, the integration committed through its plain `CI.py` (decision 1), the FeatureTest soak run (decision 8), and the bridge restarted (the lead ends its `run.py` processes; the client starts it again on the next engine tool call). A5 changes only the catalogue's descriptions, which a builder subagent's cached schemas may keep without losing a parameter; a change of a method's parameters or of the method set needs a fresh top-level Claude Code session, which the lead asks the product owner for (ADR 0026 F27, F30).

### 7. The replays

- **`Opening.replay`:** several pieces placed with moves, rotations, a hold, a soft drop and hard drops, ending with them in the well.
- **`LineClear.replay`:** a planned placement of the known pieces that clears at least one line, the HUD showing it.
- **How they are recorded:** each from tick 0 by an Editor-only recording suite (`Modes: ["Editor"]`, a single case, `Test.InjectAction` at planned ticks or a board-reading autopilot, `Test.Expect` on what only the input causes), with `test_run {filter: "<the suite's exact name>", record: "<path>"}`, so re-recording is one call and its input plan stays readable through `script_read` (GT-01). The suite's name must not occur in any replay path (for example `Record.Opening`), so the filter selects no committed replay (GTF-07; A3 names such a row). Both end with the played state on screen (no restart), so the checkpoints and the final hash prove the play.
- **Replaying twice:** each replay is verified twice with `input_replay {verify: true, strictHash: true}` (equal `stateHash`, both in the transcript) and once with `withoutInput: true` (must fail). Together with the games stage's three strict plays (Debug and Release editors, testing Dist export, each equal to the committed hash and to each other) this is what C3's reworded acceptance line names. The games suite does not replay twice in one session: Pipeline map G1's code change is not adopted, which keeps ADR 0026's decline of the same second `input.replay` pass in the editor half (`0026-m16-contract.md:496`, "Integration"), because the three plays in three processes already prove it (ADR 0023).
- **Expectations:** ADR 0026 F10 (`test.run {record, expect}`) stays deferred, with the default of decision 12: the checkpoints and the hashed HUD state prove the play, and the recording suite's own `Test.Expect` asserts it at record time. `input.record`'s `expect` remains available, and with A1 the expectations of a recording with input events are verified with its input before anything is written.

### 8. Soak readiness

Tetris is the first soaked demo: every `Projects/*` scene, 10,000 lockstep ticks, no input.

- **What the game must do:** the cell pool has no bodies (no `RigidBody` or collider on a cell); game over waits for `Restart` without looping audio, and the game-over sound is short; live data stays bounded (no move history or per-lock log; HUD text written on change); no script error occurs at start (`OnStart`) or at top-out (B4 counts both).
- **The builder checks it through MCP:** a 10,000-tick lockstep run without input from the saved scene (`play_start {lockstep: true}` without a seed, then `play_step`), `script_errors {}` afterwards, and `engine_call stats.get` past ticks 5,000 and 9,500 (entities, bodies and voices must not grow between them).
- **FeatureTest under B4:** B4 also changes the verdict for FeatureTest's 30 soaked scenes (`Errors.scene` and the golden scenes among them), which have never been checked for idle script errors. Before session 1, once the integration is built, the lead runs `python Scripts/Hardening.py --steps soak --soak-only "FeatureTest:*"` once (a development aid that ends as a warning; it needs the Release editor and a device), and an existing scene that raises a script error at idle is fixed before the build starts, as a friction item.
- **Before the gate,** the lead runs `python Scripts/Hardening.py --steps soak --soak-only "FeatureTest:*,Tetris:*"` (every editor subject; the Runtime subject reads no script errors and needs a current export): engine fixes during the build can change FeatureTest's scenes too. B4 makes a new script error fail the subject.

### 9. Look screenshots and the copy rule

- Screenshots from the orthographic camera at 720 × 900 in a lockstep play session with a stack of cells: `viewport_screenshot {view: "Game", target: "Play", width: 720, height: 900}`, plus `debugView` `AO` and `ShadowCascades`, from an editor launched with `renderer: "vulkan"`.
- The builder returns their paths. The PNGs live in `Projects/Tetris/Library/Automation/Out/`, which the next editor session prunes, so the lead copies them to `bin/Reviews/M17-Look/` before any later editor launch on the project; they are attached to the milestone review, and nothing is committed.
- Optionally a suite of its own with `Modes: ["Dist"]`, bound to `Main.scene`, calls `Test.Screenshot`, which the games suite's Dist half writes below `bin/Exports/Dist/Tetris/bin/TestUserData/Tetris/TestResults/`: shipping-binary screenshots from the gate itself (critic N7). Run modes belong to `Testing.Suites` entries, and the Test API has no run-mode query (`Engine.d.luau:1087-1131`), so a case inside `Tetris.test.luau` would also run in the builder's editor, which fails it without a renderer (GTF-11); such a suite is never a recording suite.

### 10. Between sessions, and the engine-change checklist

- The lead triages the builder's friction log into the friction list below. Fix agents implement engine, editor, bridge and documentation changes with tests in worktrees (decision 6); the lead applies them, rebuilds Debug, Release and Dist, regenerates the references, and restarts the bridge when its code or the catalogue changed. A builder subagent inherits this session's tool list (ADR 0026 F27), so after a change of a method's parameters or of the method set the next builder starts from a fresh top-level session, which the lead asks the product owner for; a description-only change needs only the bridge restart.
- **An engine change that alters canonical output or simulation hashes** (ReviewChecklist §9; critic N6) is committed only with everything it forces, in these steps:
  1. A fix agent makes the fix and its regression test; the lead applies the staged diff. Nothing is committed yet.
  2. The lead rebuilds Debug, Release and Dist, runs `GenerateDocs.py --config Debug` when a description changed, and restarts the bridge when its code or the catalogue changed (a fresh top-level session when a method's parameters or the method set changed, as above).
  3. With no Tetris builder running, Breakout is upgraded and re-recorded through automation, so its transcript records it: by a dedicated agent the lead starts with the builder's rules (only the `mcp__engine__*` tools, no file reads), or by the lead's own session through those tools, the only bridge user at that time (risk 5). It runs `project_upgrade` and, for each affected replay, `test_run {filter: "Autopilot.Breakout", record: "Assets/Tests/Replays/Opening.replay"}`. A canonical-only change may use `Editor --headless --project Tests/Projects/Breakout --upgrade` instead (its `cli` lines the audit accepts).
  4. The lead runs `Scaffold.py`, then `Scaffold.py --check`, which regenerate FeatureTest and re-record its affected replays.
  5. The review, `PreCommit.py`, and the commit, with `Projects/Tetris` unstaged (decision 11).
  6. Tetris's `project.upgrade` (or `Editor --upgrade` when no editor holds it) and its affected re-recordings, through its recording suites, are made in its next builder session, and the session record notes them. Until then the unstaged Tetris may fail checks that no commit runs on it (decision 11).

### 11. Tetris stays unstaged until done, with backups

- `Projects/Tetris` stays out of the index until the game meets acceptance: staging it earlier would put a half-built game into the games suite and into any commit (Pipeline map R3). No fix commit stages it.
- The disk readers still read it (Pipeline map R2; default in force): the AuthoredFiles canonical-JSON and replay-`Config` cases, DocExamples' type-check case over `Projects/*/AGENTS.md` (not its lint case, C1) and Lint's JSON step. The editor writes canonical JSON, and the builder's `AGENTS.md` fences type-check or carry `skip="<reason>"` (decision 6's prompt); B3 removes the one disk reader that audited provenance.
- The replay-`Config` case is named "every committed replay records a run mode as its Config" (`AuthoredFilesTests.cpp:178`) but walks the working tree, untracked files included (Pipeline map doc error 5). The name is kept, an accepted inaccuracy of these disk readers: what it checks holds for every replay on disk, committed or not, since the editor writes the `Config`.
- After each session the lead copies the project, without `Library/` and `Build/`, to `bin/Reviews/M17-Backup/S<N>/`, because the work has no git history until it is committed.

### 12. Deferrals, accepted limits and declined proposals

Each deferral and accepted limit gets a `Docs/Backlog.md` row in C5 (ReviewChecklist §9), with its default in force and "ADR 0027 decision 12" as its source; the last column names where the item came from:

| Item | Default in force | Origin |
|---|---|---|
| Entity paths in component data (an `EntityRef` value resolved from a path or unique prefix, as method params are) | ADR 0010 decision 32: an `EntityRef` keeps the handle pattern; A4's hint says to use `{"$ref": "<op>.entity.id"}` or the id `entity.get` returns | Content map R1, C02 |
| Named `$ref` (an op name instead of its index) | positional op indices; the docs create the assets in a first batch and refer to them by path, and advise `dryRun: true` first | Content map R4, C13 |
| Reading a replay's events back through automation | recording suites (`test.run {record}`); a replay made with `input.record` is re-recorded by planning its input again | Gameplay map GT-01, GD-01 |
| A builder-side Dist hash self-check (a testing export that runs `--feature-test` and reports its hashes) | the games suite's Dist half | Gameplay map GTF-08 |
| Screenshots kept outside `Library/Automation/Out` | the lead copies them to `bin/Reviews/M17-Look/` before the next editor launch (decision 9) | Content map R3, C14 |
| ADR 0026 F10, `test.run {record, expect}` (the row reworded) | checkpoints and the hashed HUD state prove the play; the recording suite's own `Test.Expect` asserts it at record time | ADR 0026 F10; critic N3 |
| ADR 0026 F29, the input check's optional final-hash warning (the missing row) | not done | ADR 0026 F29; critic N3 |
| Accepted limit: `input.record` Stop writes a recording without input events unverified (A1), unlike `test.run {record}` | its expectations and any `script.eval` write during it are checked by the `input_replay {verify: true, strictHash: true}` the docs require after every recording | A1; the two pins of decision 3 |
| Accepted limit: A1's verification counts the script errors raised up to the recording's final tick, not those raised only at teardown (`OnDestroy`), because a refused verification must leave its session live (added at integration, stream A's review) | `test.run` stops a replay's scripts before it reads their errors (`FeatureTestRunner.cpp:1098-1102`), so it counts a teardown error when it plays the replay: the games suite and the builder's `test_run` catch it | A1; stream A's review |
| Accepted limit: two staged game projects whose `.eproj` files share a file stem are not refused | not an M17 case; their games steps and reports (`games <stem> ...`, `Games-<stem>-...`) collide, and two whose manifests share a `Name` share the Dist half's package directory (`bin/Exports/Dist/<Name>`); two `.eproj` files in one directory still fail the suite (ADR 0026 R13) | Pipeline map R7 |

**Declined proposals** ("not adopted" in the predicted-friction table) are not deferrals: the proposal is rejected, the engine keeps its documented behaviour with the alternative named in that row in force, and no work is planned, so they get no Backlog row. Reopening one needs an ADR, normally prompted by a friction item that hits it. They are GAF-05's `SetMaterial` early-out (cells are touched only where they change), a typed `Field.Asset` (GAF-13; A5 documents the names), `Test.GetStateHash` without the tick (GTF-02; runs are compared with `input_replay`), and two that keep standing decisions: a game-view screenshot defaulting to the project window (Content R2; ADR 0009 decision 33's 640 × 360 default stays, and the docs pass the size) and the games suite's second `input.replay` pass (Pipeline G1; ADR 0026's decline, decision 7 above).

### 13. Acceptance and the gate

- Roadmap M17's acceptance (as C3 rewords it), with `Opening.replay` and `LineClear.replay` as decision 7 describes; `games.check_build_settings` passes; the look screenshots attached to the milestone review (decision 9).
- `AuditProvenance.py --project Projects/Tetris` passes in index mode on the committed tree; `Test.py --suite games --require-gpu` passes in Debug, Release and Dist for Breakout and Tetris; the soak passes.
- Every friction item is closed (a change with its regression test) or recorded as an ADR decision with its default in force; every predicted item below is dispositioned.
- Then the milestone review and its fixes, one `CI.py --milestone` (decision 1), the milestone commit, a fast-forward of `main`, the push, and a pause for the product owner.

## Frozen interfaces

| File | Interface | Stream |
|---|---|---|
| `Scripts/Lib/games.py` | `check_build_settings(project_file: Path) -> list[str]`; `read_game_name(path: Path) -> str` (stubs; every ADR 0026 name unchanged) | B |
| `Scripts/Test.py` | `check_game_in_editor(client, engine_client, project, config, copy, hashes, data, arguments, console) -> list[str]` and `check_game_export(client, engine_client, project, copy, hashes, work, data, arguments, console) -> Step`, signatures unchanged; behaviour per B1 and B2 below | B |
| `Scripts/Hardening.py` | `soak_editor_scene(engine_client, editor, project_file, scene, user_data, console, deadline, frames=hardening.SOAK_FRAMES) -> SoakRun`, signature unchanged; behaviour per B4 below | B |
| `input.record` (`ReplayMethods.cpp`; `ReplayMethods.h`'s `InputRecord` comment) | params and result unchanged; Stop's behaviour per A1 below, which the header comment states | A |
| `test.run` (`TestMethods.cpp`; `TestMethods.h`'s `TestRun` comment) | params and result unchanged; the refusal per A2 below, which the header comment states | A |
| `FeatureTestRunner` (`FeatureTestRunner.h`, `FeatureTestRunner.cpp`) | `struct FeatureTestSelection { std::vector<size_t> Suites; std::vector<std::string> Replays; }` and `GetSelection() const -> Result<FeatureTestSelection>` (stub, A2); the refusal message per A3 below | A |
| `MethodRegistry::Invoke` (`MethodRegistry.cpp`; `MethodRegistry.h`'s `Invoke` comment) | signature unchanged; the `EntityRef` issues per A4 below, which the header comment states | A |
| `Tests/Source/Support/DocExamples.h` (test support) | `DocExampleChecker::Check(const DocExampleBlock&) const -> Result<std::vector<DocExampleFinding>>`, signature unchanged; it returns Warning findings too (C1) | C |

### A1: `input.record` Stop

- **Scope:** a Stop whose recording has input events, in the editor and the Runtime. A recording without input events is written directly, as now, without a verification: a pure determinism replay, which Stop does not replay (verify it with `input.replay`), so the recorded session stays the play session (the deliberate difference from `test.run {record}`, decision 3; the accepted limit of decision 12).
- **Order:** the existing preflight (destination, expectations compiled, the recording finished), then the verification, then the input check as now (the candidate written only when that check fails; the result, `inputCheckTick` and the paused session as now).
- **The verification:** the play session restarts from the candidate's header, as the input check does, and plays it with its input, `{verify: true, strictHash: true, withoutInput: false}`, evaluating the Stop call's compiled `expect`. It passes when the playback passes as `input.replay` judges it (every checkpoint and the final hash equal, every expectation evaluated and satisfied, and no early stop: a quit or a fatal error) and raises no script error, as `test.run {record}`'s ordinary-play verification requires (`FeatureTestRunner.cpp:1125-1128`). The input check that follows keeps its own rule: script errors in the run without input do not decide it.
- **A refusal:** Validation (-32003), located at `/path`.
  - Message: `refusing to write '<path>': replayed with its input, the recording does not reproduce itself: <reason>; nothing was written`, where `<reason>` names the first failure in tick order (an expectation at a tick comes before that tick's checkpoint):
    - `the state hash at checkpoint tick <t> is <observed>, recorded <expected>`;
    - `expectation <i> at tick <t> (/expect/<i>/luau) is false`, or `... failed: <error message>` when it raised;
    - `the final state hash at tick <t> is <observed>, recorded <expected>`;
    - `playback stopped at tick <t>: <stopReason>`;
    - and, only when the playback otherwise passed, `the playback raised <n> script error(s); the first: <script>:<line>: <message> (tick <t>)`.
  - Hint, exactly: `a script.eval or play.waitFor that writes script state (a self field, a module table) during a recording is neither recorded nor detected, an expectation must hold when the recording's own input is replayed, and a script that reads the host (Application queries, pairs over entity keys) diverges; record again without them`.
  - Error data: `verification`, the `InputReplayResult` of that playback (`withoutInput` false), as `input.replay`'s error data `replay`; no `inputCheck`.
  - After it: nothing written, the recording ended (`play.state` `recording` false), and the play session is the verification's, paused where it stopped, lockstep released, as after `input.replay`.
- **Operational failures of the verification** keep their error's code, write nothing, end the recording and add a context, as the input check's do (`ReplayMethods.cpp:159-163`, `:308`): a failure to start it (`RestartForReplay` or the playback's `Begin`), `while starting the verification of '<path>' with its input; nothing was written`; a failure during it (an `Advance` error, a cancelled session), `while verifying '<path>' with its input; nothing was written`. A disconnect during it writes nothing. The input check's two contexts stay as they are and apply only to the input check.
- **Timeout:** `input.record`'s 600 s covers both playbacks.

### A2: `test.run` and a dirty open scene

- **When:** after `FeatureTestRunner::Begin` succeeds and before `EditorFeatureTestHost::Acquire` takes the run's lease (`TestMethods.cpp:176-194`), with or without `record`, before any case or replay plays. So every earlier error wins: the `timeoutTicks` and `record` param errors, `ValidateReplayOutput`, Preflight's (admission, script type errors, loading every replay), `CaptureCoverageBaseline`'s and `Begin`'s (InvalidArgument reported as Validation: the selection, "recording requires exactly one selected suite", the Parameters). The check reads the selection with `FeatureTestRunner::GetSelection()` (decision 2), never a second `Discover` or a copy of the runner's filter rule.
- **The rule:** the run is refused when the editor has an open scene with unsaved changes whose path (`EditorContext::GetScenePath()`, a `VfsPath`) equals, compared as resolved `VfsPath`s:
  - (a) the `Scene` of a selected suite, resolved as a suite session reads it: by `play.start`'s scene-path rule (`ResolvePlayScenePath`, `EditorPlayController.cpp:62-80`, `:275`), which takes a project-relative path or a `project://` path to the same `VfsPath`; or
  - (b) the scene a selected replay plays: the asset registry's `SourcePath` of its header's `Scene.Handle`, as `EditorFeatureTestHost::CreateReplaySession` plays it (`EditorFeatureTestHost.cpp:167-171`), not the header's `Scene.Path`, which a moved or renamed scene leaves stale.
  - A suite `Scene` that does not resolve, or a replay whose scene asset is missing, matches nothing here; the run reports it as before.
- **Error:** InvalidState (-32002).
  - Message: `'<scene path>' is open with unsaved changes, and the selected suite '<name>' plays its saved file`, or `..., and the selected replay '<path>' plays its saved file`, `<scene path>` being the open scene's project-relative path, naming the first such selection: suites in `Testing.Suites` order, then replays in canonical path order.
  - Hint, exactly: `scene.save first; test runs play the saved scene file`.
- **After it:** nothing ran: no case and no replay played, no report file was written, no `testRun` error data, the lease was never taken, and the open scene and its history are untouched.
- **Unaffected:** an empty-`Scene` suite, a suite or replay of another scene, a clean open scene, no open scene, and `test.list`.

### A3: a refused recording

- When `FeatureTestRunner` with `Record` refuses because a selected row did not pass, the Validation message becomes `recording requires passing selected cases and a valid starting scene; <n> selected row(s) did not pass: <row>, <row>, ...`. Each row is `<suite>/<case> (<status>)`, a replay's suite being `replay:<path>`, in report order, at most 10, then `, and <k> more`.
- When a replay row is among them, the hint, exactly: `the filter also selects the replays named; give the recording suite a name that no replay path contains (for example Record.<Name>) and record with exactly that filter`.
- When every selected row passed but there is no recording (the suite's session never started), the message is unchanged. The run's rows, reports and `testRun` error data are unchanged; nothing is written.

### A4: `EntityRef` values in component data

- Every `EntityRef` value in component data (a component field, a script field reached through `Script.Fields`, an element of an array or map of `EntityRef`) that is a string but not a 16-hex-digit id is refused: InvalidArgument (InvalidParams, -32602), one issue per value at its JSON pointer, the issue's message the strict reader's `expected a 16-digit hex UUID string` (unchanged), and the issue's hint, exactly: `an Entity value in component data takes the entity's 16-hex id; use {"$ref": "<op>.entity.id"} in a batch or the id entity.get returns`. The handler does not run.
- **The error's hint** is that hint when every issue of the error is such an `EntityRef` issue; otherwise it follows `MakeParamsError` (the one issue's hint, none for several, `MethodRegistry.cpp:254-266`). An `EntityRef` issue is never NotFound, so a request with such a value and an unresolved asset reference is InvalidArgument with both issues.
- **Which pass reports it:** the pass that reaches the value, as for asset references (`MethodRegistry.cpp:725-752`). The schema-less first pass reaches component fields and refuses before `Script.Fields` are canonicalized with their field schemas, so a request that also holds such a value in `Script.Fields` reports only the component fields' issues; a request whose only such values are in `Script.Fields` reports them all from the pass with schemas. This follows the asset-reference precedent and is accepted: fixing the reported values and calling again reports the rest.
- Unchanged: a 16-hex id and null are accepted; a value of another JSON type keeps the strict reader's error; method params that name an entity (an id, a unique prefix or a path, §13.4) resolve as before.

### B1: `check_build_settings`

The failure lines, exactly, in this order:

1. `StartScene is empty: set it to the scene the game starts with (Roadmap M17 and M18: StartScene and Export.BuildScenes set)` (absent or empty);
2. `Export.BuildScenes is empty: list the scenes the export must load strictly, StartScene included (Roadmap M17 and M18: StartScene and Export.BuildScenes set)` (`Export` or `Export.BuildScenes` absent, or an empty list);
3. `Export.BuildScenes does not list the StartScene '<StartScene>': add it (ADR 0027 decision 4: the demos' convention)` (both non-empty, compared as written).

An empty list when every rule holds. It raises ValueError naming the file for a file that cannot be read, a document that is not a `.eproj` JSON object (`Format` `Project`), a `StartScene` that is not a string, an `Export` that is not an object or an `Export.BuildScenes` that is not an array of strings. `check_game_in_editor` calls it first, on `copy.project_file`, its lines first among the half's failures; a ValueError becomes one failure line naming the file, and the half's other checks run either way.

### B2: the Dist half's game name

- **Order:** after the testing `project.export` and before `install_package`, `check_game_export` takes `executable` from the result and requires it below `outputDirectory`, then reads the game's name with `games.read_game_name(<manifest>)`, the manifest being `<outputDirectory>/Game.json`, or on macOS `Path(executable).parent.parent / "Resources" / "Game.json"` (the bundle's `Contents/Resources`, beside the `Contents/MacOS` that holds the executable; `Exporter.cpp`'s `GetBundle` and `GetPackageResources`). The bundle is never spelled from the stem or from `project.name`, which would bring the stem back on macOS, where the Windows gate cannot see it.
- **Then:** it installs the package to `exports.package_directory(DIST_HALF, <Name>)` (`install_package(output, DIST_HALF, <Name>)`), takes the executable as the package directory joined with `executable` relative to `outputDirectory`, and looks for the log and the testing user data with `exports.testing_user_data(package, <Name>)` and `check_testing_user_data(package, <Name>)`. The step and report names (`games <stem> Dist`, `Games-<stem>-Dist.{json,xml}`) and `data["package"]` (the installed package's display path) keep their form.
- **Failures:** a result without `executable`, an `executable` outside `outputDirectory`, or a ValueError from `read_game_name` fails the Dist step at once with one line naming it (the manifest's path for the ValueError), the way a failed `install_package` does today; nothing is installed or run.
- `read_game_name` raises ValueError naming the file for a file that cannot be read, a document that is neither a `GameManifest` nor a `Project` JSON object, or one whose `Name` is not a non-empty string.

### B4: the soak's script errors

`soak_editor_scene` calls `script.errors {since: "end"}` after `scene.open` and before `play.start`, and keeps its `nextCursor`: `play.start` activates the session and so runs `OnStart`, whose errors must count. After the soak's last step (all its ticks, or where an error ended it) it reads `script.errors {since: <cursor>}`, following `nextCursor` until a page is empty. When any error is new, it appends one line to the subject's errors: `<n> new script error(s) during the soak; the first: <script>:<line>: <message> (tick <tick>)`, `<n>` counting the entries returned. A failed `script.errors` call is an error line naming it. The soak runs all its ticks either way, and `run_soak_subjects` fails a subject with errors, as now.

## Ownership

Streams own disjoint files. A stream that needs a change in another stream's or the lead's file lists it in its result; a defect found in a file no stream owns goes to the stream that found it.

**Stream A (C++):**
- `Engine/Source/Engine/Automation/Methods/ReplayMethods.cpp` (A1, and A5's `input.record` description)
- `Editor/Source/EditorCore/Automation/TestMethods.cpp` (A2, and A5's `test.run` description), `Editor/Source/EditorCore/Play/EditorFeatureTestHost.cpp` (A2)
- `Engine/Source/Engine/Testing/FeatureTestRunner.cpp` (A3, and A2's `GetSelection`)
- `Engine/Source/Engine/Automation/Protocol/MethodRegistry.cpp` (A4)
- A5's registration strings: `Engine/Source/Engine/Scene/Registration/RenderingRegistration.cpp`, `Engine/Source/Engine/Scripting/Bindings/{Input, Random, Script, Test, Application}.cpp`, `Engine/Source/Engine/Project/ProjectSettings.cpp` (`Export.BuildScenes`)
- the header comments the contract froze, for comment-only refinements the implementation shows to be needed (each named in the stream's result for the lead's review, Roadmap rule 3): `ReplayMethods.h` (`InputRecord`), `TestMethods.h` (`TestRun`), `MethodRegistry.h` (`Invoke`) and `FeatureTestRunner.h` (`FeatureTestSelection`, `GetSelection`)
- tests: `Tests/Source/Engine/Automation/Methods/ReplayMethodsTests.cpp`, `Tests/Source/EditorCore/Automation/TestMethodsTests.cpp`, `Tests/Source/Engine/Testing/FeatureTestRunnerTests.cpp`, `Tests/Source/Engine/Automation/Protocol/MethodRegistryTests.cpp`, the `RuntimeAutomationServer` cases that record, any C++ case that pins a description A5 changes, `Tests/Automation/test_replay.py`, `Tests/Automation/test_script_tests.py`, and any Python automation test that ran `test.run` on a dirty scene (it saves first; no test is weakened)

**Stream B (Python):**
- `Scripts/Lib/games.py` (B1, B2, the `GameProject.name` comment and the module docstring)
- `Scripts/Test.py` (`check_game_in_editor`, `check_game_export`, the games paragraph of its docstring)
- `Scripts/Hardening.py` (`soak_editor_scene`, its docstrings)
- `Tests/Automation/test_games_suite.py`, `Tests/Automation/test_audit_provenance.py` (deleting the disk-mode case), `Tests/Automation/test_hardening.py`

**Stream C (documentation):**
- `Tests/Source/Support/DocExamples.{h,cpp}`, `Tests/Source/EditorCore/Scripting/DocExampleTests.cpp` (C1, the seeded-mistake case's Error filter included)
- `Docs/Architecture.md` (C2), `Docs/Roadmap.md` (C3), `.claude/skills/{game-building, luau-gameplay, game-debugging, build-and-test}/SKILL.md` (C4), `Docs/Backlog.md` (C5), and `Scripts/CI.py`'s docstring only (C2's soak wording)
- the Luau fences of the skills and documents it changes, which both DocExamples cases check

**Lead (integration, the build, completion):**
- this ADR (the friction list, the session record, the predicted-friction "Hit as" column, "Integration"), `AGENTS.md` (its games-suite and hardening rows gain B1's build-settings check and B4's script-error rule), `Docs/CodeStyle.md`, `Docs/ReviewChecklist.md`, `Docs/Decisions/**` (notes on ADR 0026 F10 and F29 pointing to decision 12, and on ADR 0026 decision 7: its clause "the disk-mode static check (decision 9) still audits it" is stale since decision 9 as amended made `--demos --files disk` a local command, and the automation suite's disk-mode case goes with B3)
- the shared integration files: `Docs/Reference/*`, `Resources/Scripting/Engine.d.luau`, `Tools/MCP/catalog.json` (regenerated once after merging, and after any later description change), `Editor/Source/EditorCore/Automation/RegisterMethods.cpp`, the premake files, `BuiltinComponents.h`, `RegisterBindings.cpp`, `Scripts/ModuleRules.json`, `.github/workflows/ci.yml`, the rest of `CI.py`, `PreCommit.py` and `Scripts/Lib/scripts.py`
- the build: `Projects/Tetris/**` (written through the bridge only, its `AGENTS.md` by the builder), `bin/Reviews/M17-Backup/` and `bin/Reviews/M17-Look/`, the Breakout upgrade and re-record sessions (decision 10's step 3), the FeatureTest regenerations, the triage and the assignment of each friction fix to the owning module's files

## Approvals

None needed: nothing is downloaded or vendored, no license or new kind of dependency is involved, and no new tool is required.

## Risks

1. **Unbounded sessions.** No time box; the five-session report to the product owner bounds the drift (decision 6).
2. **A1 doubles Stop's playback.** Stop now plays a recording with input events twice (with and without its input); `input.record`'s 600 s timeout covers Tetris's short replays, and Breakout's replay was recorded with `test.run {record}`, not `input.record`. FeatureTest's scaffold records `Input.replay` and `Determinism.replay` through `input.record` Stop with expectations (`Projects/FeatureTest/Scaffold/Replays.jsonl`), so stream A runs `test_feature_test` (`Scaffold.py --check`).
3. **A2 refuses runs that relied on a dirty scene.** A test that ran `test.run` after an unsaved edit of a scene it plays now gets InvalidState; stream A runs the automation suites that call `test.run` and makes such a test save first.
4. **More lint warnings and larger skills as the docs grow.** C4 adds Luau fences to the skills, and C1 makes each warning a failure; stream C runs both DocExamples cases and the fits-inline case (the 48 KB `docs.get` budget, C4's "Size"). The builder's notes are type-checked but not linted (C1), and decision 6's prompt tells the builder to keep their fences checked code or `skip="notes"`.
5. **One shared bridge.** Only the builder loads the engine tools while it runs, and between sessions only decision 10's Breakout driver, with no builder running; fix agents never use the bridge, and worktrees have no `.mcp.json`.
6. **Stale binaries and tool lists.** The Release and Dist binaries predate 104 sources' modification times; Debug, Release and Dist are rebuilt before session 1 and after each fix batch, and a catalogue change needs a fresh top-level session (N5, F27).
7. **A canonical or hash-changing engine change during the build** forces both demos' upgrades, the FeatureTest regeneration and the affected re-recordings, in decision 10's steps.
8. **An unstaged project has no history.** The backups after each session (decision 11) are the only copy besides the working tree.
9. **The soak sees no input.** It proves an idle top-out stays steady (decision 8); gameplay errors after tick 300 are caught by the suites, the builder's 10,000-tick check and B4.
10. **Screenshots are pruned** by the next editor session; the copy rule (decision 9).
11. **Tetris-specific determinism traps** (`^` with a computed exponent, `pairs` over entity keys, host queries such as `Application.IsEditor`) would split the editor and Dist hashes; C4 and A5 document them, and the games stage's three strict plays catch them.
12. **B4 judges FeatureTest's scenes too.** Its 30 soaked scenes were never checked for idle script errors; the FeatureTest soak before session 1 and the soak before the gate find one before the milestone gate does (decision 8).

## Verification plan (ADR 0023)

- **Streams never run `PreCommit.py` or `CI.py`.** Each builds what it changed, runs the affected cases, shows each regression test failing against the reverted code by running that case alone (the contract's skipped bodies already fail when forced, decision 2), and finally runs once the suites its change can affect:
  - **A:** the changed C++ cases alone in Debug (`Tests.exe -tc=...`), then once the unit cases of the Automation, Testing and EditorCore modules it touched and the `RuntimeAutomationServer` cases; a Release editor build and `Test.py --suite automation --filter` runs of `test_replay`, `test_script_tests` and `test_feature_test` (`Scaffold.py --check`, which records FeatureTest's `input.record` replays through A1's Stop), and of the other Python tests that call `input.record` or `test.run`.
  - **B:** `test_games_suite.py`, `test_audit_provenance.py` and `test_hardening.py` (no editor needed except the end-to-end class), `Lint.py --paths` on its files; B3's proof with an unstaged failing fixture under `Projects/` in its worktree, removed before handing back. B2 and B4 are proved with fakes here; their first runs against a real export and real scenes are the integration's (below).
  - **C:** both DocExamples cases, the seeded-mistake case and the fits-inline case, `Lint.py --paths Scripts/CI.py`; the references check is the lead's after regeneration.
- **The contract:** the lead runs `PreCommit.py --contract` once.
- **Integration:** the lead merges the streams on the contract commit, regenerates the references, rebuilds Debug, Release and Dist, and makes the changed suites green with targeted runs; then the review and the integration commit's one plain `CI.py` (decision 1), which runs the games stage in Debug, Release and Dist (Breakout through B2's Dist half) and the automation suite against the Release editor; then, before session 1, the FeatureTest soak under B4 (`Hardening.py --steps soak --soak-only "FeatureTest:*"`, decision 8), which plain `CI.py` leaves out. Fix batches between sessions are verified with targeted runs, and those committed on their own run `PreCommit.py` (decision 1). The milestone review and its fixes come before the single `CI.py --milestone` on the final tree.

## Friction list

Every awkward step, unclear error, missing method, documentation gap or tempted workaround a builder session reports, triaged by the lead. A resolution is either a change with its regression test or an ADR decision with the default in force; the milestone review blocks on an open row.

| Id | Session | Symptom and evidence (BuildLog line, tool error, code) | Predicted | Owning module | Resolution (change and regression test, or ADR decision with the default in force) |
|---|---|---|---|---|---|

## Predicted friction

From the three maps and the critic (`bin/Reviews/M17-Map-*.md`, which hold the evidence). Unlike M16 (ADR 0026 decision 15), the confirmed defects are fixed before the build (streams A to C); every other item is dispositioned here, and the lead fills in "Hit as" from the friction list.

| Id | Predicted item | Hit as | Disposition |
|---|---|---|---|
| Content C01, D1, D6 | The builder waits for `BUILD_START_SCENE_MISSING` to leave `_meta`, or reads `_meta` as a validation report | | C2 (§13.11 step 2), C4 (`_meta` against `project_validate`) |
| Content C02, R1, D3, D7, D8; Gameplay GAF-15 | `ScoreText: "/Game/ScoreText"` is refused (an `EntityRef` in component data takes an id); `_meta` said to confirm overrides | | A4 (the hint); C2 and C4 (`Entity` fields wired by id, `$ref` kept); decision 12 (entity paths deferred) |
| Content C03, R2, D4, D9, D12 | Screenshots default to 640 × 360, misframing the portrait game and scaling the HUD | | C2 and C4 (`width: 720, height: 900`); decision 9. R2 (defaulting to the project window) is not adopted: the docs pass the size, and ADR 0009 decision 33's 640 × 360 default stays (decision 12, declined proposals) |
| Content C04, C10, D11 | The Edit-mode screenshot shows an empty well; the HUD and previews do not fit beside the well at §13.11's framing | | Decision 9 (lockstep play, `target: "Play"`, a stack of cells); C4 Look subsection (framing iterated on 720 × 900 screenshots) |
| Content C05, C06, C07, C08, C09, D2 | Shadows invisible without a backboard, near-hard at `LightAngle` 2; bloom invisible at emission below 1; rendering defaults unknown; the orthographic skybox is one flat colour | | C2 (the scaffold: backboard, `LightAngle` 10, `ShadowDistance` 40, HDR emission, `Camera.Clear` `Color`); C4 Look subsection |
| Content C11, R5, D13 | Text sizes and offsets in 1080p-reference pixels; World text's em undocumented; proportional digits jitter | | A5 (`Text.Size`); C4 |
| Content C12; Gameplay GAF-05, GDF-10 | HUD text written every tick; per-tick `SetMaterial` and proxy writes rewrite the component; live data must stay bounded for the soak | | C2 (cells touched only where changed), C4 (luau-gameplay pooling cost; write on change), decision 8. GAF-05's optional `SetMaterial` early-out is not adopted (decision 12, declined proposals) |
| Content C13, R4 | Hand-numbered `$ref` indices shift when an op is inserted | | C2 (assets in a first batch, referred to by path); decision 12 (named `$ref` deferred) |
| Content C14, R3 | Screenshots are pruned by the next editor session | | Decision 9 (the lead copies them); decision 12 (screenshots outside `Library/Automation/Out` deferred) |
| Content C15, D5, D10; Gameplay GTF-09; Pipeline R2 (fences) | `AGENTS.md` has no MCP write path, and its `luau` fences are type-checked against the doc stubs | | Decision 6 (the one-file rule, and the prompt's fence rule); C2 and C4 (fences type-checked unless `skip=`; not linted, C1) |
| Content C16 | Whole-file `script.write` grows the transcript | | ADR 0026 F12, default in force (whole files; scripts stay small modules, game-building §4) |
| Content C17, C18 | Offloaded results; methods reachable only through `engine_call` | | Already documented (game-building), default in force |
| Content C19 | A fresh machine without the built-in bake fails the export | | Default in force: the gate machine has the bake; the export's hint names the bake |
| Content C20, C21, C23 | The Dist smoke runs 300 headless frames; audio is adequate; `Export.Exclude` still validates test scripts | | No change needed (documented in game-building §10) |
| Content C22 | Lowercase enum spellings in the docs | | C2 and C4 (canonical spellings) |
| Gameplay GAF-01, GAF-14 | Bag, hold and top-out in `Game.luau` cannot be tested in the empty scene; the 220-cell pool covers the well only | | C2 (`Board.luau` holds them; 220 well cells plus preview cells), C4 (the `Board.test` topics) |
| Gameplay GAF-02, GAF-03, GAF-04, GDF-04 | `Random.New` needs a seed source and is outside the hash; unavailable at a module's top level; `Shuffle` refuses a frozen list | | A5 (`Random.New`); C2 (the generator in `OnStart` from `Random.Integer`); C4 (luau-gameplay `Random` rules) |
| Gameplay GAF-06, GAF-07 | Pool creation inside one `OnStart` budget; colliders add body churn; cells moved by `Translation` writes slide | | C2 (cells without colliders, fixed positions), C4 (pooling), decision 8; the games suite's Debug half measures the budget |
| Gameplay GAF-08 | No `Scene.Reload`, no handle to the current scene | | ADR 0026 P06, default in force (restart in place or `Scene.Load` of the saved scene; Backlog §5) |
| Gameplay GAF-09, GDF-03, GTF-01 | Script-table writes from `script.eval` go undetected, and Stop does not verify with its input | | A1; C4 (evaluation during recordings) |
| Gameplay GAF-10 | Lint warnings fail the games suite while `script.check` passes | | C1; C2 (the excerpt fixed) |
| Gameplay GAF-11 | No typed idiom for a `Board` with methods | | C4 (luau-gameplay function-field record idiom) |
| Gameplay GAF-12 | Action names are untyped strings | | C4 (luau-gameplay action-name constants) |
| Gameplay GAF-13 | `Field.Asset` takes a free string; a `.sfx` needs `AudioClip` | | A5 (`Field.Asset`); typing the parameter is not adopted (decision 12, declined proposals) |
| Gameplay GDF-01, GDF-02 | State kept only in script tables looks input-independent; Tetris-specific guard refusals | | Decision 7 (recording suites with `Test.Expect` on what only the input causes, the played state kept in the scene); C2 (the pool mirrors the board); C4 |
| Gameplay GDF-05 | Gravity curves written with `^` call the CRT `pow` | | C2 (a frozen integer table of ticks per row), C4 (`^` and the determinism traps) |
| Gameplay GDF-06, GTF-08 (host queries) | Host-dependent reads split the editor and Dist hashes | | A5 (`Application.IsFocused`, `IsEditor`); C4 (determinism traps) |
| Gameplay GDF-07 | Pool bookkeeping through `pairs` over entity keys | | C4 (determinism traps) |
| Gameplay GDF-08, GDF-09; Pipeline R1 | Soak counters (entities, bodies, voices) fail a per-lock instantiation, cells with bodies, or a late game-over sound; the soak is a de facto acceptance criterion | | Decision 8; C2 (§15.1 T6 wording) and C4 (the soak rules) |
| Gameplay GDF-11 | The Dist smoke and the soak are blind to gameplay errors after tick 300 | | B4; decision 8 (the builder's `script_errors` check) |
| Gameplay GDF-12, GTF-05 | Gravity-paced cases exceed the 600-tick default; a `WaitUntil` timeout fails the case | | A5 (`Test.WaitUntil`); C4 (drive cases with hard drops, `TimeoutTicks`) |
| Gameplay GTF-02 | `Test.GetStateHash` includes the tick, so a suite cannot compare two moments or runs | | A5 (`Test.GetStateHash`); C2 (step 9: compare runs with `input_replay`). The optional `IncludeTick = false` is not adopted (decision 12, declined proposals) |
| Gameplay GTF-03 | `play_start {seed: 42}` shows another piece order than suites and recordings | | C2 (`play_start {lockstep: true}` without a seed after `scene_save`; the seed wording of §13.4, §13.6 and §11.10), C4 |
| Gameplay GTF-04 | `test.run` silently plays the saved scene | | A2; C2 and C4 |
| Gameplay GTF-06 | Default Suite isolation carries the board from case to case | | Default in force (Suite); `Isolation: "Case"` is documented (game-building §2) |
| Gameplay GTF-07 | A recording filter that matches a stale replay's path refuses the recording | | A3; decision 7 and C4 (recording-suite names no replay path matches) |
| Gameplay GTF-08 (Dist parity) | The builder cannot check the Dist hashes itself | | Decision 12 (deferred; default: the games suite's Dist half) |
| Gameplay GTF-10 | `input.record` expectations are compiled, never type-checked | | Decision 7 (recording suites, whose `Test.Expect` is type-checked); A1 (in a recording with input events, a misspelled field fails the verification before anything is written; an event-free recording's expectations are first evaluated by the `input_replay` the docs require after it, decision 12's accepted limit) |
| Gameplay GTF-11 | `Test.Screenshot` errors its case in a renderer-none editor | | C2 (launch with `renderer: "vulkan"`); decision 9 (never in a recording suite) |
| Gameplay GTF-12 | Planning `LineClear` inside `input.record` has no undo | | Decision 7 (recording suites with planned ticks; re-recording is one call) |
| Gameplay GT-01, GD-01 | A replay made with `input.record` cannot be re-recorded through MCP | | Decision 7 (recording suites); C2 (§13.6) and C4; decision 12 (reading a replay's events back deferred) |
| Pipeline G1 | "Replaying twice" is not checked as written | | Decision 7 and C3 (the builder's two `input_replay` runs plus the games stage's three strict plays); the suite's code change is not adopted, keeping ADR 0026's decline of it (`0026-m16-contract.md:496`) beside ADR 0023 |
| Pipeline G2 | `Export.BuildScenes` is never checked | | B1 |
| Pipeline G3 | The automation suite audits unstaged demos from the working tree | | B3 |
| Pipeline R2 (other readers) | AuthoredFiles, DocExamples and Lint read the unstaged working tree | | Decision 11, default in force (the editor writes canonical JSON; fences type-check or carry `skip=`, and DocExamples' lint case leaves the notes out, C1) |
| Pipeline R3 | Staging Tetris before it is done puts it into the gate | | Decision 11 (unstaged until done, backups) |
| Pipeline R4 | Two demos to upgrade and re-record after an engine change | | Decision 10 (the engine-change checklist) |
| Pipeline R5 | CI time grows with Tetris | | Accepted: the games stage's 3,600 s and PreCommit's 5,400 s bounds hold; measured at the gate |
| Pipeline R6 | The Dist half finds the package by the file stem | | B2 |
| Pipeline R7 | Discovery edge cases (two `.eproj` files; one name under both roots) | | Default in force (two files fail the suite by design, ADR 0026 R13); duplicate names across the roots are an accepted limit with a Backlog row (decision 12) |
| Pipeline R8 | The Dist smoke writes `%LOCALAPPDATA%\Tetris` | | ADR 0026 decision 18, default in force |
| Pipeline R9 | Flakiness: strict GPU rules with GTAO, PCSS and bloom; the soak's heap rule can be inconclusive | | Default in force; the documented remedy `--soak-frames 40000` (build-and-test) |
| Pipeline R10 | `.gitattributes` and `.gitignore` | | No change needed (`test_provenance.py` pins the `-text` rules for `Projects/Tetris`) |
| Pipeline doc errors 1 and 5 | ADR 0026 decision 7 still says the disk-mode static check audits an unstaged demo; the AuthoredFiles case "every committed replay records a run mode as its Config" walks the working tree | | The lead's note on ADR 0026 decision 7 (Ownership; B3); decision 11 (the case's name kept, an accepted inaccuracy) |
| Critic N1 | The builder cannot read §13.11, the Roadmap or CodeStyle | | Decision 6 (the prompt quotes the Roadmap verbatim); C4 (every builder-facing correction in a served skill) |
| Critic N2 | Later sessions cannot read the game's own notes | | Decision 6 (the one-file Read and Write rule) |
| Critic N3 | F10's stopgap removes expectations; F29 has no Backlog row | | Decisions 7 and 12; C5 |
| Critic N4 | No M17 contract ADR | | This ADR |
| Critic N5 | Stale tool list and binaries between sessions | | Decisions 6 and 10 (rebuilds, the bridge restart, a fresh session after a catalogue change); C4 (game-building) |
| Critic N6 | Engine fixes also touch FeatureTest | | Decision 10 (the regeneration and FeatureTest's re-recordings in the checklist) |
| Critic N7 | `Test.Screenshot` in the Dist half keeps shipping-binary screenshots | | Decision 9 (optional Dist-mode case) |

## Session record

No time box (decision 6); the lead reports to the product owner before a sixth session.

| Session | Date | Builder invocation | Reached | Friction ids | Rebuilt or reconnected after |
|---|---|---|---|---|---|

## Before milestone completion

- [ ] Every skipped test of this contract is replaced by its live version (6 C++ cases, 7 Python tests), the `FeatureTestRunner::GetSelection` stub and both `games.py` stubs are implemented, and `test_demos_audits_every_discovered_game_project` is deleted; one strict `CI.py --milestone` run is green on Windows, the games stage, the hardening stage with the Tetris soak, and the clang-cl builds included.
- [ ] A1 to A5, B1 to B4 and C1 to C5 landed with their tests; the references are regenerated; the Backlog rows of decision 12 exist.
- [ ] The integration was committed through one plain `CI.py`, and the FeatureTest soak under B4 ran before session 1 (decisions 1 and 8).
- [ ] Tetris meets Roadmap M17's acceptance as C3 words it: the suites green, `Opening.replay` and `LineClear.replay` recorded from recording suites and each verified twice and with the input check, `games.check_build_settings` clean, `project.validate` clean, the Dist smoke run and exiting 0, the look screenshots in `bin/Reviews/M17-Look/` and attached to the milestone review.
- [ ] `AuditProvenance.py --project Projects/Tetris` passes in index mode on the committed tree; `Test.py --suite games --require-gpu` passes in Debug, Release and Dist for Breakout and Tetris; `Hardening.py --steps soak --soak-only "FeatureTest:*,Tetris:*"` passed before the gate.
- [ ] Every engine change that altered canonical output or hashes followed decision 10's steps: both demos' upgrades, the FeatureTest regeneration and the affected re-recordings.
- [ ] Every friction item is closed or recorded as an ADR decision with its default; every predicted item has its "Hit as" and disposition; the session record holds each session's dates and reach, and the five-session report is recorded if it was due.
- [ ] Architecture, the Roadmap, `AGENTS.md`, the skills and the Backlog describe what landed; the notes on ADR 0026 F10, F29 and decision 7 are in place.

## Architecture and Roadmap amendments

The contract amends no standing document; it changes three C++ header comments to document A1, A2 and A4, and adds `FeatureTestSelection` and `GetSelection` to `FeatureTestRunner.h` for A2 (decision 2). Stream C amends Architecture §11.10 (execution step 1's seed wording), §13.4 (the seed default, and the `EntityRef` bullet: ids, prefixes and paths for entity params, the 16-hex id only inside component data), §13.5's `test.run` row (A2's refusal, A3's message), §13.6 (recording's seed default, Stop's verification of a recording with input events with its input, re-recording through recording suites), §13.11 (rewritten, C2), §15.1 T5 and §15.8's games row (B1's build-settings check), §15.1 T6 and §15.8's hardening row (the soak's projects and B4's script-error rule), `CI.py`'s docstring, the four skills (C4) and `Docs/Backlog.md` (C5), and the Roadmap's M17 and M18 "replaying twice" lines (C3). The lead updates `AGENTS.md`'s games-suite and hardening rows (B1, B4) and adds notes on ADR 0026 F10 and F29 pointing to decision 12, and on ADR 0026 decision 7 (its disk-mode clause, stale since decision 9 as amended; B3).

## Integration

The streams ran in worktrees from `d61d9e8` (workflow `wf_ae47c33e-248`), each implemented, then reviewed by two read-only lenses and fixed in its worktree (stream A 7 findings, B 6, C 13; all fixed or answered with evidence). The lead applied the three staged diffs to `D:\Engine` (`git -C <worktree> diff --cached --binary HEAD | git apply --index --3way`; all clean), regenerated the references once after the merge, and rebuilt Debug, Release and Dist.

### Stream A (decision 3)

- **A1** as frozen, in the editor and the Runtime: `ReplayMethodOperation` gains a Verification stage between the recording and the input check (`ReplayMethods.cpp`), `input.replay` shares its restart helper unchanged, and the phase marker gains `Record:verification`. The count is the verification session's occurrence cursor, so an `OnStart` error counts.
- **A1's first script error,** refined at the integration review (finding K-01, the lead's decision as the contract owner): the error stream moves a repeated error to its latest occurrence, so its first entry is not always the first occurrence (a repeat in the same tick after another error). `PlaySession` gains `[[nodiscard]] const std::optional<ScriptError>& GetFirstScriptError() const` (`PlaySession.h`, a public addition to a frozen header), which latches the first error the session publishes, and A1's refusal names it; "ReplayMethods: input.record stop names the first script error of its verification although a repeated error follows another in one tick" (one tick and `OnStart`) fails with the stream's first entry.
- **A1's teardown limit** (found by stream A's review, accepted): the verification counts the script errors raised up to the final tick, not those raised only at teardown (`OnDestroy`), because a refused verification leaves its session live; `test.run` counts them when it plays the replay. The `input.record` description, the `InputRecord` comment of `ReplayMethods.h` (a comment-only refinement of a frozen header comment, reviewed by the lead) and a decision 12 row with its Backlog row say so.
- **A2** as frozen: `FeatureTestRunner::GetSelection` implemented; `TestMethods.cpp` checks after `Begin` and before `Acquire`. Interpretations, accepted: the suite name in the message is the suite script's import-time `Test.Suite` name (`ScriptData::Name`; the script path when the script does not load); a suite's `Scene` is resolved with `EditorMethodContext::ResolveProjectPath(scene, "/scene", ".scene")`, which applies `ResolvePlayScenePath`'s rules (that function is file-static in `EditorPlayController.cpp`); `EditorFeatureTestHost.cpp` is unchanged.
- **A3** as frozen; the hint is added when a named row is a replay row. The unchanged bare message for "every row passed but nothing was recorded" is unreachable through the public API (an open session always records or sets a recording error, and a session that fails to open adds a failing `<setup>` row), so it stays as a guard without a test.
- **A4** as frozen, inside component data only (values under a component-map element, script fields included).
- **A5:** the descriptions of the table, each checked against the code; `Text.Size` also names Billboard text (an em of Size / 100 metres facing the camera).
- **Tests:** the 5 skipped C++ cases and 2 Python tests un-skipped with their frozen names; new: "ReplayMethods: input.record stop names a raised expectation, an early stop and every script error of its verification", "RuntimeAutomationServer: input.record stop verifies a recording with its input before its input check", "TestMethods: test.run refuses a dirty scene that a replay plays after a move and a suite names with project://", "MethodRegistry: an EntityRef issue beside an unresolved asset reference makes the call InvalidArgument with both issues", "TestRunner: a refused recording names at most ten rows and counts the rest"; "RuntimeAutomationServer: input.record stop refuses a recording whose outcome does not depend on its input" now requires A1's second restart (stricter). Each fails without its change (P1 to P7 and P-a to P-i in the stream's report). `Scaffold.py --check` reproduced FeatureTest byte for byte, so no committed replay hash changed.

### Stream B (decision 4)

- **B1, B3 and B4** as frozen. B3's proof: with an unstaged copy of `Tests/Data/Provenance/HashMismatch/Project` at `Projects/ProvenanceProof`, the deleted disk-mode case failed (exit 1, hash mismatch) and the staged-only case passed.
- **B2,** with two refinements of the frozen text, accepted:
  - the testing log is named after the executable's stem, as the engine names it (`ProcessContext.cpp`, Architecture §4.4 `Logs/<exe>.log`), not after the `Name` (equal on Windows; a dotted `Name` differs on Linux and macOS); `check_testing_user_data` gains an optional `log_name` (empty keeps the export suite's behaviour);
  - `read_game_name` also refuses a `Name` that is not one folder name (a character of `<>:"/\|?*`, or `.` or `..`, as `Paths::ValidateAppName` refuses), because `install_package` removes `bin/Exports/Dist/<Name>` before installing.
- **Tests:** the 5 skipped tests un-skipped; new tests for the executable taken from the result, the macOS bundle path (on every host), the log under the executable's stem, the build-settings lines first with the other checks still running, an early-ending soak, malformed or stuck `script.errors` pages, a failed `script.errors` call, and the fake's own refusals; each proved with its bug applied alone.
- **B4's line,** refined at the integration review (finding P-01, the lead's decision): `script.errors` lists each distinct error once, at its latest occurrence (a repeated error moves after the cursor with a new id, its latest tick and its count + 1), and keeps the 256 errors with the latest occurrences, so the soak cannot know the first occurrence that the frozen line names. The line says what it shows: `<n> new script error(s) during the soak; the earliest listed: <script>:<line>: <message> (latest at tick <tick>, <count> occurrence(s))`, followed by `; earlier errors were evicted` when any page answered `evicted: true`. `test_hardening`'s `ScriptErrorClient` now models the stream's merge, count and eviction (`ScriptError.cpp`), pinned by `test_the_fake_script_error_stream_merges_and_evicts_as_the_editor_does`; `test_the_soak_line_names_the_earliest_listed_error_with_its_latest_tick_and_count` (a repeated error, an error raised before the cursor too, and 300 errors with evictions) fails against the old line.

### Stream C (decision 5)

- **C1** as frozen; the only warning in the checked documents was §13.11's excerpt, now four lines.
- **C2 to C5** as frozen. Beyond decision 5's lists, accepted as corrections the rewrite needed: §4.12 and luau-gameplay on `^` (exact also when the exponent is a local set to 2, 3 or 0.5 and never reassigned, which constant folding turns into a literal), §8.7's PCSS bullet (the formula gives the filter radius, half the penumbra's width), §11.9 (lint warnings are Warning diagnostics; the lint case), §13.7 (the state hash includes the tick; the outline example's camera), §13.10 (the game-building skill's description), and a new Seed bullet in §13.4 (there was no sentence to reword).
- `project.validate` reports lint warnings as `SCRIPT_TYPE_ERROR` with severity Warning, so the games suite's validate check fails on them too; the skills say so. At the integration review (finding X-05) the code table says so too: `SCRIPT_TYPE_ERROR`'s documented severity is `Error; Warning for a Luau lint warning`, a fourth value beside ADR 0022 decision 10's three (`ProjectValidator.h`'s comment, `Validation.md`, the game-debugging row; "ProjectValidator: a Luau lint warning is a SCRIPT_TYPE_ERROR warning, as the code's documented severity says").
- Skill sizes after the edits: game-building 43,958 bytes, build-and-test about 44 KB, game-debugging about 27.3 KB, luau-gameplay 21,788 (the fits-inline case passes).
- **C3,** one extra correction accepted at the integration review (finding X-06, the lead's decision): Roadmap M18's acceptance gains "`StartScene` and `Export.BuildScenes` set", matching M17's line, which B1's failure lines, `games.py`'s `BUILD_SETTINGS_RULE` and `Test.py`'s docstrings cite as "Roadmap M17 and M18".

### Lead items

- `AGENTS.md`: the games-suite row (B1's build-settings check; the Dist package named after the manifest's `Name`) and the hardening row (the soak's projects and B4's script-error rule).
- ADR 0026: notes on F10, F29 and decision 7's disk-mode clause.
- `Docs/Backlog.md`: the teardown accepted limit.
- The references regenerated once after the merge (`GenerateDocs.py --config Debug`, then `--check`), and again after the integration review's fixes.

### Integration review

The integrated tree was reviewed before its gate (workflow `wf_1eac71be-89c`: three lenses, cross-stream consistency, C++ and Python, each finding verified by an adversarial skeptic): 11 findings, all confirmed, none above minor (X-01 to X-07, P-01 to P-03, K-01). Two fix agents with disjoint files fixed them (workflow `wf_2ab6ecdc-337`) and a read-only recheck of each found five nits, which the lead fixed (the §13.6 wording of A1's departure from `test.run {record}`'s rule, a stale test comment, `ProjectValidator.h`'s severity comment, a general `SCRIPT_TYPE_ERROR` description, these notes). The refinements are recorded above: K-01 and the teardown limit (stream A), P-01 (stream B), X-05 and X-06 (stream C).
