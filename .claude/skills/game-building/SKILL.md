---
name: game-building
description: Build a game end to end through the editor's automation (the MCP tools or engine_client): launch, project settings, scenes, prefabs, the look, scripts, deterministic checks, test suites, replays recorded from recording suites (autopilots and planned input), validation and export, and the provenance, transcript, soak and demo-audit rules that define done.
---

# Game building

The workflow an agent follows to build a game with this engine, from an empty project to an exported build. Write gameplay with the `luau-gameplay` skill, and diagnose with `game-debugging`. Tools are named `domain_verb` by the MCP bridge; a method without a tool (`prefab.revert`, `project.refreshAssets`, `play.state`, `stats.get`) is reached through `engine_call {method, params}`, and `engine_client` calls every method by its `domain.verb` name.

## Documentation

Everything a builder reads is served by `docs_get`, which needs a connected editor: launch or attach first ("1. Launch"). `docs_get {}` lists every topic with its size.

- **Skills:** `skills/<name>`, and a skill's reference pages `skills/<name>/<page>`; this file is `skills/game-building`, and "Pages" lists its own.
- **Reference:** `reference/Automation` (every method with its params and results), `reference/Components` (every component field with its range and default), `reference/FileFormats`, `reference/Validation`, `reference/ScriptAPI` and `reference/BuiltinAssets` (every `engine://` asset).
- **JSON schemas** of the file formats: `reference/Schemas/<Format>`, such as `reference/Schemas/Project` (every `.eproj` field) or `reference/Schemas/SoundEffect`.
- **Scripting:** `scripting/Engine.d.luau` (the declarations the type checker uses) and the script templates `templates/Scripts/Behaviour`, `templates/Scripts/Module` and `templates/Scripts/Test`.
- **Native module:** `native/NativeApi.h`, the C++ interface a game's module includes ("4. Scripts").
- **Long topics:** a topic too large for one response returns its first page with `nextOffset` and its sections; `{topic, toc: true}` returns only the sections (with `section`, its subsections). Ask for a section by its heading's exact text or anchor (`{topic, section: "Input"}`, `"reference/ScriptAPI#input"`); a heading that shares its name carries its kind (`"Script (component)"` beside the `Script` global, `"TestSuite (type)"`), which the plain name finds when no heading is exactly that name. Page on with `{topic, offset: <nextOffset>, length?}`.
- **One method's JSON schemas:** `engine_call {method: "rpc.discover", params: {method: "prefab.instantiate"}}`.

The Architecture sections cited below (§13.12: demo acceptance) and the definition of done's `Scripts/` commands are for the people who review and gate the game; an automation-only builder needs only the topics above.

## Pages

Reference pages beside this file, each a `docs.get` topic and a file in this skill's directory:
- `skills/game-building/editor-sessions` ([editor-sessions.md](editor-sessions.md)): attaching to an editor the user has open, recovering after a crash, and a stale tool list after the catalogue changed.
- `skills/game-building/prefabs-and-levels` ([prefabs-and-levels.md](prefabs-and-levels.md)): prefabs, their per-instance values and nesting; a level's static compound body; pieces that report their own collisions.
- `skills/game-building/look` ([look.md](look.md)): the look: lights and shadows, glow, background, screen text, GTAO and framing.
- `skills/game-building/replays` ([replays.md](replays.md)): recording and verifying replays, the input check, recording suites and re-recording.
- `skills/game-building/exports` ([exports.md](exports.md)): testing, pruned and command-line exports.

## Ground rules

- **Only through automation.** Every engine-consumed file of a game (the root `<Game>.eproj` and `.luaurc`, everything under `Assets/`, every `.meta` and a native module's sources under `Native/`) is written by the editor through an automation method: never with a text editor, a shell or a file-copy tool. `Scripts/AuditProvenance.py` rejects any other origin (§13.12).
- **Provenance and transcript are committed.** In the game's project directory, `<Game>/` below (`Projects/<Game>/` for a demo, `Tests/Projects/Breakout/` for the M16 dry run), the editor records each write in `<Game>/Automation/Provenance.json` (`{Path, XXH64, Method, RequestId, Client, TranscriptLine}`; `RequestId` is null only for a `ui` write), and the MCP bridge appends every request to `<Game>/Automation/BuildLog.jsonl` and passes its line number on, so each entry names the transcript line of the request that wrote it. Never edit either by hand; never delete the transcript.
- **Windowed UI writes do not count.** A file saved from the editor's own UI is recorded with method `ui`, which the audit rejects for demo games. Use the UI to look, not to author.
- **`Library/` is a cache** (import cache, autosave, lock, offloaded results, screenshots). It and `Build/` (export output) are gitignored, as are `*.bak` files; none is committed or audited.
- **Documentation is exempt.** `<Game>/AGENTS.md` and a `README.md` are written directly (no method writes them): describe the game's scenes, actions, scripts and tests there when you finish. The unit suite type-checks every `luau` fence of `AGENTS.md` against the documentation's stub modules, not the game's (it does not lint them): a fence is either code you already ran through `script_write` and `script_check`, or carries `skip="<reason>"`, as a snippet that requires the game's own modules must.
- **A missing capability is an engine change.** When automation cannot do what the game needs, add the method (the `add-automation-method` skill) or the script API (`add-script-api`) with its tests before continuing. Never work around it by hand.

## 1. Launch

- `editor_launch {project: "Projects/<Game>", create: true, template: "Empty"}` creates and opens the project (templates: `Empty`, `Basic3D`). A relative project path (`"Projects/<Game>"`, `"Tests/Projects/Breakout"`) names a directory below the repository root, whatever the bridge's working directory, and the transcript records it that way. Without `create` it opens an existing one, always through a transcribed `project.open` on a launcher-state editor (never `--project`), so whatever opening writes, such as the `.meta` files of new sources, is attributed to that request. The bridge launches the Release editor headless with `renderer: "none"`; pass `renderer: "vulkan"` for screenshots and a Dist export's smoke test, which a `renderer: "none"` editor refuses (`Unsupported`) and leaves unrun. A rendering editor holds the machine-wide GPU lock that every checkout shares until it exits, so `editor_shutdown` it when the rendering work is done; a launch waits while a gate (`CI.py`) or another GPU run holds it (`LockUnavailable` after 30 min, naming the holder).
- **No scene is open after a launch.** Opening a project opens none of its scenes, so scene and entity tools answer `InvalidState` ("no scene open") until you open one: `scene_open {path: "Assets/Scenes/Main.scene"}` (the project's `StartScene` in `project_get_settings`), or `scene_new {path}` in a new project. An attached editor shows whatever scenes its user has open, or none. `editor_status` (its `session.scene`) and the result of a launch or an attach report the active scene's `path` and `dirty` flag, or null; `scene_list` lists every open scene.
- **Attaching to the user's editor, recovering after a crash, and a stale tool list:** see `skills/game-building/editor-sessions`.

## 2. Project settings

`project_set_settings {patch}` applies an RFC 7386 merge patch to the `.eproj` as one undoable command; a member the settings lack is dropped and named in `warnings`. Objects merge key by key; **arrays replace whole** (restate the full `Layers`, `Collisions`, `BuildScenes`, `Suites` or `Replays` list each time).

```json
{"patch": {
	"Name": "Tetris", "StartScene": "Assets/Scenes/Main.scene",
	"Window": {"Title": "Tetris", "Width": 720, "Height": 900},
	"Simulation": {"FixedHz": 60, "Seed": 42},
	"Input": {"Actions": {
		"MoveLeft": {"Type": "Button", "Bindings": ["Key.Left", "Gamepad.DPadLeft"]},
		"MoveX": {"Type": "Axis", "Positive": ["Key.D"], "Negative": ["Key.A"], "Gamepad": "Gamepad.LeftX"}}},
	"Export": {"BuildScenes": ["Assets/Scenes/Main.scene"]},
	"Testing": {
		"Suites": [{"Script": "Assets/Tests/Board.test.luau"},
			{"Script": "Assets/Tests/Tetris.test.luau", "Scene": "Assets/Scenes/Main.scene", "Isolation": "Case"},
			{"Script": "Assets/Tests/Record/Opening.test.luau", "Scene": "Assets/Scenes/Main.scene", "Modes": ["Editor"]}],
		"Replays": ["Assets/Tests/Replays/*.replay"]}}}
```

- `project_get_settings` returns the whole document; `docs_get {topic: "reference/Schemas/Project"}` lists every field with its type, range and default.
- **Build settings.** Set `StartScene` and list it in `Export.BuildScenes` (the scenes the export loads strictly beside it): the games suite fails a game whose `StartScene` is empty or whose `BuildScenes` is empty or does not list `StartScene` as written, and reports these first among each editor half's failures (its other checks still run).
- **Folders.** A new project has `Assets/{Scenes, Scripts, Prefabs, Materials, Models, Textures, Audio, Fonts, Tests}` (`asset_list` lists assets, never folders). `asset_create` creates missing parent folders, so a `Folder` op is needed only for an empty folder; one whose path is already a folder is a no-op (no command, `created: false`), and a file there stays `AlreadyExists`.
- Physics layers: `Physics.Layers` holds 1 to 16 names with `Default` first, and `Physics.Collisions` lists the pairs that collide; unlisted pairs do not.
- **Validation codes come from `project_validate {}`.** `_meta.diagnostics` only counts what was logged since your previous call (`newErrors`, `newWarnings`, `newScriptErrors`, the first three entries in `firstNew`) and never lists a validation code. `BUILD_START_SCENE_MISSING` and `BUILD_SCENE_MISSING` are expected until the start scene exists, and `TEST_SUITE_INVALID` until each suite's script exists.

### `Testing.Suites`

Each entry is `{Script, Scene?, Parameters?, Isolation?, Clock?, Overrides?, ExpectQuit?, Modes?}` (§11.10; `reference/Schemas/Project` describes each): `Script` the `*.test.luau` file; `Scene` the scene it runs in (omitted: an empty scene, right for pure-logic suites); `Parameters` the `Scene.Load` parameters (`Scene.GetLoadParameters()`), such as a timer carried into level 2; `Isolation` `"Suite"` (cases share one scene, in order, so a board carries from case to case) or `"Case"` (the scene restarts before each case, each from the same seed); `Clock` frame deltas that replace the lockstep clock (frame-timing tests); `Overrides` `{CallbackBudgetMs, MemoryLimitMB, PauseOnError}` for this suite, applied exactly; `ExpectQuit` the exit code the suite must end with through `Application.Quit` (-1: it must not quit); `Modes` the run modes it runs in, `["Editor", "Release", "Dist"]` by default, `["Editor"]` for recording suites (§7).

`Testing.Replays` holds globs of `.replay` files; every run verifies each one. `CaseTimeoutTicks` and `SuiteTickLimit` bound cases and suites. Leave `CoverageGates` false: only `Projects/FeatureTest` evaluates the engine's coverage gates.

## 3. Scenes, assets and prefabs

- `scene_new {path: "Assets/Scenes/Main.scene"}` creates and opens a scene; `scene_save` writes it; `scene_open {path, save | discardChanges}` replaces the active scene with another.
- **Several open scenes.** Up to 16 scenes stay open as tabs, one active, each with its own history, selection and autosave: `scene_new {path, keepOpen: true}` or `scene_open {path, keepOpen: true}` adds one (without `keepOpen` both replace the active scene), and `scene_list`, `scene_activate {scene}` and `scene_close {scene?, save | discardChanges}` list, switch and close them. Scene-addressing tools (`entity_*`, `prefab_*`, `edit_*`, `scene_tree`, `scene_save`, `script_eval`, ...) take an optional `scene` (a project path or a `scene_list` id; absent, the active scene); an `edit_batch` with `scene` runs every op there, its paths and `$ref`s resolving there.
- **Batches.** Build content with one atomic `edit_batch {label, ops: [{method, params}]}` per logical step: if op *k* fails, ops 0..k−1 are undone and the error names `failedOp`. Create the assets in a first batch and name them by path afterwards (`"Materials": ["Assets/Materials/Wall.material"]`, also inside component data). To name an entity that an earlier op of the same batch created, use its path (`"parent": "/Game"`, in an entity param or an `Entity` value), or give that op a `name` (`{"name": "board", "method": ...}`) and write `{"$ref": "board.entity.id"}`: unlike a positional `{"$ref": "3.entity.id"}`, neither shifts when you insert an op. Run `dryRun: true` first and check each op's `entity.path` in `results`.
- `entity_create {name, parent?, components: {Type: {Field: value}}}` and `entity_update` take component values by registry name; `component_schema {name}` lists fields, ranges, defaults and enum values. Write enum values in their canonical spelling, as the schemas list them (`"Orthographic"`, `"Game"`, `"Play"`). Entity params take an id, a unique id prefix or a path (`/Game/Board`). An **`Entity` value inside component data** (a script's `Field.Entity()`, any entity field of a component) takes the same forms, resolved in the scene the call targets. Assets are referenced by handle, `Assets/...` path or `engine://...` path. The built-ins (`engine://Meshes/Cube`, `engine://Environments/Studio`, ...) are listed by `asset_list {dir: "engine://"}` (or a folder below it, `"engine://Environments"`) and in `reference/BuiltinAssets`; an unknown engine path's NotFound suggests the closest.
- `asset_create {type: Material | SoundEffect | SpriteSheet | Scene | Prefab | Folder, path, values}` writes native assets; `asset_import {source, destDir}` imports external files (glTF, PNG, HDR, audio, fonts). A `SpriteSheet` (`.spritesheet`; its fields and limits are in `reference/FileFormats`) names a `Texture` and cuts `Frames` from it (texel `Rect`s, x right and y down from the top-left corner, with a `Pivot` and 9-slice `Borders`) and `Clips` that show frames by index for a number of `Ticks`; a frame that leaves a registered texture is refused at `/values/Frames/<i>/Rect`, and `asset_set_properties` replaces a whole `Frames` or `Clips` array. `asset_delete {asset}` moves an asset with its `.meta` to `Library/Trash/` as one undoable command; it removes a folder below `Assets/` that holds no files (only empty folders, as a trashed recording leaves), also undoably, and refuses one that holds files (`Validation`, naming them: delete its assets first).
- **Prefabs, tracks and levels:** see `skills/game-building/prefabs-and-levels`.
- Look at the result: `scene_tree {format: "Text"}` for the outline, `entity_bounds` for layout and gaps, and `viewport_screenshot {view: "Game", width: <Window.Width>, height: <Window.Height>, annotate: {labels: "all"}}` (needs `renderer: "vulkan"`). Without `width` and `height` it renders 640 × 360 whatever the window (`game-debugging` item 7). The image comes back as image content; its PNG in `Library/Automation/Out/` lasts until the next editor session prunes it, so pass `keep: true` for one you must hand over (`Library/Automation/Kept/`, never pruned).
- **The look:** see `skills/game-building/look`. Finish the look before recording replays: every look component is hashed.

## 4. Scripts

- `script_create {path, template: "Behaviour" | "Module" | "Test"}`, then `script_write {path, source}`. Each result carries compile, type and lint diagnostics with full ranges; iterate until `script_check {}` returns an empty `diagnostics` array. Its `passed` counts only errors, but the games suite fails a game on any diagnostic, and Luau's lint warnings (severity `Warning`, such as an unused local or two statements on one line) count. Set `Scripting.BlockPlayOnTypeErrors` to keep type errors out of play: play, reloads and test runs then refuse them (test runs always do).
- The Behaviour and Module templates (`templates/Scripts/<Template>` in `docs_get`) name their table `Script_<File>` (a Behaviour also its exported alias and its `Script.Define` name), a unique valid identifier: rename it in the first `script_write`. The Test template has no type and names its suite after the file stem without `.test`: `Assets/Tests/Board.test.luau` gives `Test.Suite("Board", ...)`.
- `script_write` replaces the whole file. Keep scripts small modules (rules in a Module, the behaviour that drives them in another) so each write stays short.
- Attach a behaviour with `entity_update {entity, components: {Script: {Scripts: [{Script: "Assets/Scripts/Game.luau", Fields: {...}}]}}}`: a list of slots `{Script, Fields, ExecutionOrder}`, each its own instance (`luau-gameplay`), which `Scripts` replaces whole. An override naming no declared field or of the wrong type or range is refused (InvalidParams at `/components/Script/Scripts/<slot>/Fields/<Name>`), so success means every override matched; `project_validate` reports such overrides already in files (`SCRIPT_UNKNOWN_FIELD_OVERRIDE`, `SCRIPT_FIELD_TYPE_MISMATCH`).
- Keep pure logic (boards, scoring, kick tables, and the spawn rules: bag, hold, top-out, levels) in Modules without engine calls, so the empty-scene suites test it. The behaviour reads input, calls them and mirrors their state into components (cells' active flags and materials, HUD `Text`), which the state hash and the replays see.
- **A native C++ module** (Windows, only while the user's "Allow native code" preference is on, kept in `user://Editor.json` for every editor that user starts; no tool sets it): `native_write` its `.cpp` and `.h` files below `Native/` (one includes the engine's `NativeApi.h`, the topic `native/NativeApi.h`, as `native.write`'s example in `rpc.discover` spells it, and names `ENGINE_NATIVE_MODULE`; own headers as `#include "Native/<File>.h"`), `native_build` (until no `NATIVE_FORMAT_WARNING` is left), `native_load`, then `native_set_components {entity, components: {"Native.<Name>": {...}}}`. A game kept free of `PostProcess` and physics starts from `Empty`, or deletes `Basic3D`'s `/Post Process` and Ground's `RigidBody` and `BoxCollider`. `skills/game-debugging/native-module` has the rules.

## 5. Verify deterministically

- `scene_save`, then `play_start {lockstep: true}`. Without `seed` the session runs with the session seed (the project's `Simulation.Seed` XOR the scene's `Seed`), as every suite and every recording does, so it shows their piece order and spawns, which another `seed` can change. `play_start` plays the active scene with its unsaved edits, while recordings and test runs play the saved file. Only `play_step {ticks, input?, render?}` advances it; each result carries `tick` and `stateHash`.
- Input events are tick offsets from the step's start: `{tick: 0, type: "Action", name: "MoveLeft", state: "Tap"}` (down this tick, up the next). Analog actions take `value`, which holds on every later tick until another value replaces it: send `value: 0` to let go. A `Down` button likewise stays down until an `Up`.
- `script_eval {context: "Play", entity: "/Game", code: "return self.Board:ToAscii()"}` reads state (`script: "<Name>"` binds `self` to the slot whose behaviour has that name); `play_wait_for {until: "<luau returning a boolean>", timeoutTicks}` steps until a predicate holds.
- Repeated runs with the same seed and input must give the same `stateHash`. A difference is a determinism bug in the game: `^` with a computed exponent, `pairs` over entity keys and gameplay that reads the host (`Application.IsEditor`, `Application.IsFocused`, `Time.GetRealTime`) are the usual causes, and split the editor's hashes from the export's (`luau-gameplay`, "Determinism pitfalls").
- **What the state hash covers.** Every entity with its id, name, parent, active flag, tags and every serialized component, the look included (`PostProcess`, `Environment`, lights, `Camera`, `Text`; a `Script` component's slots with their authored `Fields`, never their Luau state), the tick, the session's `Random` state, the id draws and every body's position, rotation, velocities and sleep flag. So which entities draw, with which material handles, and every `Text` string are covered. It never covers material or sound asset contents (a component stores the handle), script tables (`self.State`, module locals), a `Random.New` generator's state, audio voices, input state or rendered output (frames, interpolated positions): a score that lives only in a script table is invisible to the hash until a Text or another component shows it. Observe script state with `Expect` (replays) and `Test.Expect*` (suites).
- `play_stop` returns to the edit scene unchanged.

## 6. Test suites

- One suite per area: pure-logic suites in the empty scene (`Board.test.luau`) and integration suites bound to a scene (`Tetris.test.luau`) that drive play with `Test.InjectAction`, `Test.WaitTicks` and `Test.WaitUntil` and read the game through `GetScript()` and component values. `Test.GetStateHash` hashes the tick too, so two reads at different ticks always differ and no suite can compare two runs: compare runs with `input_replay` (§7).
- **Tick budgets.** A case has 600 ticks by default (`Testing.CaseTimeoutTicks`, or `{ TimeoutTicks = N }` on the case) and a suite 36,000 in all (`SuiteTickLimit`). Drive cases with hard drops rather than gravity, and give a gravity-paced case its own `TimeoutTicks`. `Test.WaitUntil(predicate, timeoutTicks)` checks now and after every tick, its predicate must return a boolean, and a timeout fails the case.
- `test_run {}` runs every suite and verifies every replay; `test_run {filter: "Tetris/hard drop"}` selects by a case-insensitive substring of `<suite>/<case>` (a replay by its path), so `"Breakout/"` also selects `Autopilot.Breakout/...`: name the longer suite or a case when one suite's name ends another's. The result has `passed`, `counts` (rows by status: `total`, `passed`, `failed`, `error`, `timeout`, `skipped`, `quit`), `failedCases` (up to 20 `"<suite>/<case>"` names), each case with its outcome and failures with file and line, and the `jsonPath` and `junitPath` reports. The `coverage` object's `counters` stay empty unless you pass `coverage: true` (the JSON report always holds them). A green run has no failed, errored, timed-out or skipped rows.
- **Test runs play saved scene files.** `test_run` refuses with `InvalidState` while an open scene, active or not, that a selected suite or replay plays has unsaved changes: `scene_save` it first (with `scene: <its path>` for another tab).
- Write a feature's tests first: stub the new function, write its cases and run them red, then implement it and run them green. `edit_undo` and `edit_redo` also take a `script_write` back and forth, so a test can be shown failing without the feature through automation.
- The same suites run headless with `Editor --headless --project Projects/<Game> --run-tests [--filter F]` and, in exported testing builds, with `Runtime --feature-test`.

## 7. Replays

A replay records the input applied per tick from tick 0, the state hash of every 60th tick (its **checkpoints**) and the final state hash (§6.7, §13.6). The checkpoints make a replay prove the trajectory of its run, not only where it ends.

Every committed replay comes from a recording suite, is verified twice, and fails its verification without its input: `skills/game-building/replays` has the procedure, and how to re-record after a change.

## 8. Validate and export

- `project_validate {}` must report nothing. `fix: true` applies every safe fix as one undoable command; `fix: ["<id or CODE>"]` only the selected ones. `reference/Validation` explains each code and its fix.
- `project_export {config: "Dist", smokeTest: true}` writes `Build/<Platform>-Dist/<Game>/` and runs the exported executable with `--headless --frames 300 --expect-no-errors`; it must exit 0. Check the result: `smokeTestRan: true` and `smokeTestExitCode: 0`. From a `renderer: "none"` editor (a Dist Runtime needs a graphics device) the export succeeds with `smokeTestRan: false` and a warning, which proves nothing. The Dist smoke test keeps the game's log in the user's real data folder (`%LOCALAPPDATA%\<Game>` on Windows), as a shipped game does. What an export refuses is in `game-debugging` ("An export is refused"). On Windows the executable carries the icon, version and company (`resourcesPatched`); every package holds `THIRD_PARTY_NOTICES.txt`, and a macOS export is `<Game>.app`.
- **Testing, pruned and command-line exports:** see `skills/game-building/exports`.

## Definition of done

A game is done when all of these hold:
1. `project_validate {}` reports no diagnostics and `script_check {}` none, lint warnings included; `native_build` no `NATIVE_FORMAT_WARNING`.
2. `test_run {}` is green: every suite in every mode it lists, and every replay verified with its committed hash. `Testing.Replays` matches at least one committed replay recorded from tick 0 with input events, and every replay with input events fails its verification without them (§7).
3. Every replay comes from a recording suite (every level a physics ball plays has an autopilot whose replay reaches the goal without it), and each was verified twice with `input_replay {verify: true, strictHash: true}` with equal `stateHash` values.
4. `StartScene` is set and `Export.BuildScenes` lists it.
5. `project_export {config: "Dist", smokeTest: true}` succeeds from a rendering editor, with `smokeTestRan: true` and `smokeTestExitCode: 0`.
6. **Soak-ready.** The milestone gate soaks every scene below `Assets/Scenes/` of a game in `Projects/<Game>` (not one in `Tests/Projects/<Game>`) for 10,000 rendered lockstep ticks without input, from the session seed, rendering each scene's game view too (a sample whose game view did not render fails), and fails a scene whose entity, body, voice or device-allocation counts are higher in its last twentieth than in ticks 5,000 to 5,500, whose memory or script heap grows, or that raises a new script error (in `OnStart` too). So pool entities (no `Scene.Instantiate` per lock), give pooled entities no `RigidBody` or collider, let game over wait for `Restart` with a short sound and no looping audio, keep live data bounded (no move history), and write HUD text on change. Check it: from the saved scene `play_start {lockstep: true}`, `play_step` to tick 10,000, `script_errors {}` (before any `script_eval`, whose failures it lists too), and `engine_call {method: "stats.get"}` past ticks 5,000 and 9,500 (`entities`, `bodies` and `voices` must not grow between them).
7. `python Scripts/AuditProvenance.py --project <Game>` passes on the staged project (index mode, the acceptance mode: the staged files as a fresh clone holds them): every engine-consumed file has a provenance entry with its current XXH64, written by an automation method whose request at the entry's transcript line in `BuildLog.jsonl` has the same id, method and client (or by `project.upgrade`); no entry names a file that no longer exists unless the transcript records an `asset.delete` or `asset.move` of that file (deleting its folder never excuses it); no two staged paths differ only in letter case.
8. `python Scripts/Test.py --suite games --config Debug,Release,Dist --require-gpu` passes with the game staged: the audit; the build settings (4), `script.check`, `project.validate` and the `project.upgrade` dry run clean; every suite and replay at its committed hash in the Debug and Release editors and in a testing Dist export; at least one committed replay with input events, and every one failing its verification without them (§7); and the plain Dist export's smoke test run and exiting 0.
9. `<Game>/AGENTS.md` describes the scenes, input actions, scripts, suites and replays, and its `luau` fences follow "Documentation is exempt" (Ground rules): a failing fence fails every later commit.

A demo milestone adds its Roadmap acceptance. **Tetris (M17):** `Board.test.luau` covers kicks at both walls and the floor, clears of 1–4 lines and their scores, the 7-bag yielding each piece once per bag, top-out ending the game and hold swapping once per piece; `Tetris.test.luau`, bound to `Main.scene`, has integration cases driven by `Test.InjectAction`; `Opening.replay` (several pieces placed with moves, rotations, a hold, a soft drop and hard drops, ending with them in the well) and `LineClear.replay` (a planned placement of the known pieces that clears at least one line, the HUD showing it) each come from a recording suite; and the look screenshots, from a `renderer: "vulkan"` editor in a lockstep session with a stack of cells, are `viewport_screenshot {view: "Game", target: "Play", width: 720, height: 900}` plus the same with `debugView: "AO"` and with `"ShadowCascades"`, their paths reported.

## Pitfalls

- **Arrays replace.** A settings patch with `"Suites": [one entry]` drops every other suite.
- **Unsaved scene.** Recording, replay verification, test runs (every suite bound to a scene and every replay) and export use the saved files, while `play_start` plays the active scene with its unsaved edits: `scene_save` first.
- **Renderer none.** The bridge's default editor cannot take screenshots (`Unsupported`), and a `Test.Screenshot` call errors its case there; relaunch with `renderer: "vulkan"`.
- **Lockstep ownership.** Only the client that started lockstep may step it; a disconnect releases it.
- **Large results** over 48 KB come back as `{path, truncated: true, summary}`: read the file, or a test run's `jsonPath` and `junitPath` reports, with the bridge's `result_read` (`game-debugging`), never another file tool.
- **Hot reload pauses for determinism.** Lockstep, recording, replay and test sessions defer script and asset reloads until the session ends (`game-debugging`).
